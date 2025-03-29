#!/usr/bin/env python3
import sys
import struct
import binascii
import subprocess
import os
import json
import re
import tempfile

# ANSI color codes for dmesg-like output
COLORS = {
    0: '\033[1;31m',  # EMERG - bright red
    1: '\033[1;31m',  # ALERT - bright red
    2: '\033[1;31m',  # CRIT - bright red
    3: '\033[0;31m',  # ERR - red
    4: '\033[0;33m',  # WARNING - yellow/brown
    5: '\033[0;34m',  # NOTICE - blue
    6: '\033[0m',     # INFO - default
    7: '\033[0;30m',  # DEBUG - gray/black
}
RESET = '\033[0m'

def decode_qrcon_data(hex_data):
    # Convert hex string to binary
    try:
        binary_data = binascii.unhexlify(hex_data.strip())
    except binascii.Error as e:
        print(f"Error: Invalid hex data - {e}")
        return None
    
    # Check if we have enough data for the header
    if len(binary_data) < 8:
        print("Error: Data too short, missing header")
        return None
    
    # Extract header
    magic, uncompressed_size = struct.unpack("<II", binary_data[:8])
    
    # Check magic number
    if magic != 0x5A535444:  # "ZSTD" in little-endian
        print(f"Error: Invalid magic number: 0x{magic:08X}, expected 0x5A535444")
        return None
    
    print(f"Compressed data size: {len(binary_data) - 8} bytes")
    print(f"Expected uncompressed size: {uncompressed_size} bytes")
    
    # Decompress using system zstd
    try:
        # Create a temporary file using a simpler approach
        temp_dir = tempfile.gettempdir()
        temp_file = os.path.join(temp_dir, f"qrcon_{os.getpid()}.zst")
        
        with open(temp_file, "wb") as f:
            f.write(binary_data[8:])
        
        # Run zstd decompression
        result = subprocess.run(
            ["zstd", "-d", "-c", temp_file],
            capture_output=True,
            check=True
        )
        
        # Clean up
        try:
            os.unlink(temp_file)
        except OSError:
            pass
            
        return result.stdout.decode('utf-8', errors='replace')
    except subprocess.CalledProcessError as e:
        print(f"Error during decompression: {e}")
        print(f"STDERR: {e.stderr.decode('utf-8', errors='replace')}")
        return None

def parse_qrcode_json(json_data):
    """Parse JSON data containing QR code entries and decode their content."""
    try:
        qr_entries = json.loads(json_data)
        if not isinstance(qr_entries, list):
            print("Error: Expected JSON array of QR code entries")
            return None
        
        # Sort entries by _datetime field in ascending order
        qr_entries = sorted(qr_entries, key=lambda entry: entry.get('_datetime', '') if isinstance(entry, dict) else '')
        
        # Process each QR code entry
        combined_output = ""
        for i, entry in enumerate(qr_entries):
            if not isinstance(entry, dict) or 'content' not in entry:
                print(f"Warning: Entry {i} is missing 'content' field, skipping")
                continue
                
            print(f"\nProcessing QR code {i+1}/{len(qr_entries)} (datetime: {entry.get('_datetime', 'unknown')})")
            
            # Decode the content
            result = decode_qrcon_data(entry['content'])
            if result:
                combined_output += result
        
        return combined_output
    except json.JSONDecodeError as e:
        print(f"Error parsing JSON: {e}")
        return None

def colorize_output(text):
    """Add colors to kernel messages based on log level prefixes like <3>."""
    lines = []
    pattern = re.compile(r'<(\d)>\[(.*?)\](.*)')
    
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            level = int(match.group(1))
            timestamp = match.group(2)
            message = match.group(3)
            color = COLORS.get(level, RESET)
            
            # Format: apply color to the whole line
            colored_line = f"{color}<{level}>[{timestamp}]{message}{RESET}"
            lines.append(colored_line)
        else:
            # No log level detected, print as is
            lines.append(line)
    
    return '\n'.join(lines)

def main():
    if len(sys.argv) > 1:
        # Check if file is binary
        try:
            with open(sys.argv[1], 'rb') as f:
                file_data = f.read()
                
            # Try to interpret as text
            try:
                data = file_data.decode('utf-8')
                
                # Check if the data looks like JSON
                if data.strip().startswith('[{') and data.strip().endswith('}]'):
                    print("Detected JSON format, parsing as QR code entries...")
                    result = parse_qrcode_json(data)
                else:
                    # Treat as hex data
                    hex_data = ''.join(data.replace('0x', '').split())
                    result = decode_qrcon_data(hex_data)
            except UnicodeDecodeError:
                # It's likely binary data, treat it as raw hex
                hex_data = file_data.hex()
                result = decode_qrcon_data(hex_data)
        except Exception as e:
            print(f"Error reading file: {e}")
            return
    else:
        # Read from stdin
        print("Enter hex data or JSON array of QR code entries (Ctrl+D to finish):")
        data = sys.stdin.read()
        
        # Check if the data looks like JSON
        if data.strip().startswith('[{') and data.strip().endswith('}]'):
            print("Detected JSON format, parsing as QR code entries...")
            result = parse_qrcode_json(data)
        else:
            # Remove any whitespace and '0x' prefixes
            hex_data = ''.join(data.replace('0x', '').split())
            result = decode_qrcon_data(hex_data)
    
    if result:
        print("\nDecoded kernel messages:")
        print("-" * 40)
        print(colorize_output(result))
        print("-" * 40)
        
        # Check for duplicate lines and warn if any
        lines = result.splitlines()
        duplicates = {}
        for i, line in enumerate(lines, start=1):
            duplicates.setdefault(line, []).append(i)
        has_duplicates = False
        for line, indices in duplicates.items():
            if len(indices) > 1:
                has_duplicates = True
                print(f"Duplicate message warning: Lines {indices} are duplicates: \"{line}\"")
        if not has_duplicates:
            print("No duplicate messages found.")

if __name__ == "__main__":
    main()