#!/usr/bin/env python3
import sys
import struct
import binascii
import subprocess
import os
import json
import re
import tempfile
import sqlite3
import time
from datetime import datetime

# ANSI color codes for log levels
COLORS = {
    0: '\033[1;31m',  # EMERG - bright red
    1: '\033[1;31m',  # ALERT - bright red
    2: '\033[1;31m',  # CRIT - bright red
    3: '\033[0;31m',  # ERR - red
    4: '\033[0;33m',  # WARNING - yellow
    5: '\033[0;34m',  # NOTICE - blue
    6: '\033[0m',     # INFO - default
}
RESET = '\033[0m'

DB_PATH = "/data/data/de.markusfisch.android.binaryeye/databases/history.db"
POLL_INTERVAL = 0.5  # seconds
INACTIVITY_THRESHOLD = 8.0  # seconds
LOG_DIR = "logs"
OVERWRITE_LOG = False  # Set to True to overwrite existing log files


def decode_qrcon_data(data):
    """Decode ZSTD compressed data. Accepts a hex string or binary data."""
    if isinstance(data, str):
        try:
            binary_data = binascii.unhexlify(data.strip())
        except binascii.Error as e:
            print(f"Error: Invalid hex data - {e}")
            return None
    else:
        binary_data = data

    if len(binary_data) < 8:
        print("Error: Data too short, missing header")
        return None

    magic, uncompressed_size = struct.unpack("<II", binary_data[:8])
    if magic != 0x5A535444:
        print(f"Error: Invalid magic number: 0x{magic:08X}, expected 0x5A535444")
        return None

    print(f"Compressed data size: {len(binary_data) - 8} bytes")
    print(f"Expected uncompressed size: {uncompressed_size} bytes")

    temp_file = None
    try:
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp.write(binary_data[8:])
            temp_file = tmp.name
        result = subprocess.run([
            "zstd", "-d", "-c", temp_file
        ], capture_output=True, check=True)
        return result.stdout.decode('utf-8', errors='replace')
    except FileNotFoundError:
        print("Error: 'zstd' command not found. Please install zstd to decompress data.")
        return None
    except subprocess.CalledProcessError as e:
        print("Error during decompression:", e)
        print("STDERR:", e.stderr.decode('utf-8', errors='replace'))
        return None
    finally:
        if temp_file and os.path.exists(temp_file):
            os.unlink(temp_file)


def parse_qrcode_json(json_data):
    """Parse JSON data containing QR code entries and decode their content."""
    try:
        qr_entries = json.loads(json_data)
        if not isinstance(qr_entries, list):
            print("Error: Expected JSON array of QR code entries")
            return None

        # Sort entries by _datetime field in ascending order
        qr_entries = sorted(qr_entries, key=lambda entry: entry.get('_datetime', '') if isinstance(entry, dict) else '')

        combined_output = ""
        for i, entry in enumerate(qr_entries):
            if not isinstance(entry, dict) or 'content' not in entry:
                print(f"Warning: Entry {i} is missing 'content' field, skipping")
                continue
            print(f"\nProcessing QR code {i+1}/{len(qr_entries)} (datetime: {entry.get('_datetime', 'unknown')})")
            result = decode_qrcon_data(entry['content'])
            if result:
                combined_output += result
        return combined_output
    except json.JSONDecodeError as e:
        print(f"Error parsing JSON: {e}")
        return None


def colorize_output(text):
    """Add colors to log messages based on level prefixes like <3>[...]"""
    lines = []
    pattern = re.compile(r'<(\d)>\[(.*?)\](.*)')
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            level = int(match.group(1))
            timestamp = match.group(2)
            message = match.group(3)
            color = COLORS.get(level, RESET)
            colored_line = f"{color}<{level}>[{timestamp}]{message}{RESET}"
            lines.append(colored_line)
        else:
            lines.append(line)
    return '\n'.join(lines)


def get_last_row_id(cursor):
    cursor.execute("SELECT MAX(_id) FROM scans")
    result = cursor.fetchone()
    return result[0] if result and result[0] is not None else 0


def get_next_log_filename():
    if OVERWRITE_LOG:
        return os.path.join(LOG_DIR, "1.log")
    try:
        files = os.listdir(LOG_DIR)
        log_numbers = []
        for filename in files:
            base, ext = os.path.splitext(filename)
            if ext == ".log":
                try:
                    number = int(base)
                    log_numbers.append(number)
                except ValueError:
                    pass
        next_number = max(log_numbers) + 1 if log_numbers else 1
        return os.path.join(LOG_DIR, f"{next_number}.log")
    except Exception as e:
        print("Error reading log directory, defaulting to 1.log:", e)
        return os.path.join(LOG_DIR, "1.log")


def monitor_database():
    """Monitor the history.db for new entries and process them."""
    if not os.path.exists(DB_PATH):
        print(f"Error: Database file not found at {DB_PATH}")
        print(f"Are you root? Is the app installed?")
        sys.exit(1)

    os.makedirs(LOG_DIR, exist_ok=True)
    db_uri = f"file:{DB_PATH}?mode=ro"  # Construct the read-only URI

    try:
        conn = sqlite3.connect(db_uri, uri=True, timeout=10)
        with conn:
            cursor = conn.cursor()
            last_processed_id = get_last_row_id(cursor)
        print(f"Monitoring database '{DB_PATH}'. Starting after ID: {last_processed_id}")
    except sqlite3.Error as e:
        print("Error initializing database connection:", e)
        sys.exit(1)

    raw_decoded_buffer = []
    last_activity_time = time.time()

    while True:
        try:
            time.sleep(POLL_INTERVAL)
            with sqlite3.connect(db_uri, uri=True, timeout=10) as conn:
                cursor = conn.cursor()
                cursor.execute("SELECT _id, raw, _datetime FROM scans WHERE _id > ? ORDER BY _id ASC", (last_processed_id,))
                new_rows = cursor.fetchall()

            if new_rows:
                last_activity_time = time.time()
                print(f"--- Found {len(new_rows)} new entries ---")
                for row_id, raw_data, dt in new_rows:
                    print(f"Processing entry ID: {row_id}, Datetime: {dt}")
                    last_processed_id = row_id
                    if raw_data:
                        decoded_data = decode_qrcon_data(raw_data)
                        if decoded_data:
                            raw_decoded_buffer.append(decoded_data)
                    else:
                        print(f"Skipping entry ID {row_id}: 'raw' column is NULL or empty.")
            elif raw_decoded_buffer and (time.time() - last_activity_time >= INACTIVITY_THRESHOLD):
                filename = get_next_log_filename()
                with open(filename, 'w') as f:
                    f.write(colorize_output("".join(raw_decoded_buffer)))
                print(f"\n--- Inactivity detected. Saved {len(raw_decoded_buffer)} entries to {filename} ---")
                raw_decoded_buffer = []
                last_activity_time = time.time()
        except sqlite3.Error as e:
            print("Database error during polling:", e)
            time.sleep(POLL_INTERVAL * 2)
        except KeyboardInterrupt:
            if raw_decoded_buffer:
                filename = get_next_log_filename()
                with open(filename, 'w') as f:
                    f.write(colorize_output("".join(raw_decoded_buffer)))
                print(f"\n--- Saved remaining {len(raw_decoded_buffer)} entries to {filename} on exit ---")
            print("Exiting monitoring loop.")
            break
        except Exception as e:
            print("An unexpected error occurred:", e)
            time.sleep(POLL_INTERVAL * 2)


def main():
    # Check for help flag
    if len(sys.argv) > 1 and sys.argv[1] in ('-h', '--help'):
        print("""Usage:
  ./decode.py                 # On Termux: Monitor Binary Eye DB for QR codes and log decoded kernel messages.
                              # Not on Termux: Read hex data or JSON from stdin, decode, and print.
  ./decode.py <filename>      # Read hex data or JSON from the specified file, decode, and print.
  ./decode.py -h | --help     # Show this help message.
""")
        sys.exit(0)

    # Check if running on Termux by detecting the TERMUX_VERSION environment variable
    # or the existence of a common Termux directory
    is_termux = os.environ.get("TERMUX_VERSION") is not None or \
                os.path.exists("/data/data/com.termux/files/usr")

    if len(sys.argv) == 1:
        if is_termux:
            print("Detected Termux environment. Starting database monitoring.")
            monitor_database()
        else:
            print("Not running on Termux. Please use './decode.py <filename>' instead.")
            sys.exit(1)
    else:
        try:
            with open(sys.argv[1], 'rb') as f:
                file_data = f.read()
            try:
                data_text = file_data.decode('utf-8')
                if data_text.strip().startswith('[{') and data_text.strip().endswith('}]'):
                    print("Detected JSON format, parsing as QR code entries...")
                    result = parse_qrcode_json(data_text)
                else:
                    hex_data = ''.join(data_text.replace('0x', '').split())
                    result = decode_qrcon_data(hex_data)
            except UnicodeDecodeError:
                result = decode_qrcon_data(file_data)
        except Exception as e:
            print(f"Error reading file: {e}")
            return
        if result:
            print("\nDecoded kernel messages:")
            print("-" * 40)
            print(colorize_output(result))
            print("-" * 40)
            lines = result.splitlines()
            duplicates = {}
            for i, line in enumerate(lines, start=1):
                duplicates.setdefault(line, []).append(i)
            duplicate_found = False
            for line, indices in duplicates.items():
                if len(indices) > 1:
                    duplicate_found = True
                    print(f"Duplicate message warning: Lines {indices} are duplicates: \"{line}\"")
            if not duplicate_found:
                print("No duplicate messages found.")

if __name__ == "__main__":
    main()
