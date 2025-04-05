# qrcon (PoC)

A proof-of-concept Linux kernel driver which continuously collects kernel messages (kmsg) and during a panic, dumps it all from the beginning in batches. This provides a debugging solution when only Simple Framebuffer is available, and things like PSTORE, USB or UART are unavailable.

## Installing

Simply download the zip/git clone this repo and place it in drivers/misc, or wherever you want, ensuring to add the dir in Makefile and Kconfig of misc. i.e ```source "drivers/misc/qrcon/Kconfig"``` and ```obj-y		+= qrcon/```

## Decoding 
**Note:** Only tested on 6.14 and 6.1. >4.19 requires additional modifications

Some important settings to adjust depending on your screen size are:
```c
static int qr_position = QRPOS_TOP_RIGHT;
static int qr_x_offset = 10;
static int qr_y_offset = 200;
static int qr_size_percent = 60;
static int qr_border = 5;
```
```c
#define QR_MAX_MSG_SIZE 1200 // this is how much data will be encoded in each qrcode
```
```c
static int qr_refresh_delay = 700; // give you enough time to scan the qrcode
```
```c
static int recent_only = 0; // optionally show only recent messages for panic
```

The qrcode generation batch will automatically trigger after a panic. It likely won't survive a catostrophic panic, but it has not been tested.

Download the [Binary Eye](https://github.com/markusfisch/BinaryEye) app on Android and enable continuous scanning. (optionally enable qrcode only mode)

```bash
Usage:
  ./decode.py                 # On Termux: Monitor Binary Eye DB for QR codes and log decoded kernel messages.
  ./decode.py <filename>      # Read hex data or JSON from the specified file, decode, and print.
  ./decode.py -h | --help     # Show this help message.
```
### Automatic method
- Root is required
- Setup [ssh](https://wiki.termux.com/wiki/Remote_Access) to your device, i.e ```ssh u0_a129@192.168.8.XXX -p 8022```
- Enter ```tsu```
- Run ```python3 decode.py &```
- Read logs using ```cat logs/1.log```, etc, this can be adjusted in the code

### Manual Method
- After scanning go to history, select everything, export as JSON, and share with localsend/termux, etc.
- Use decode.py "json" to decode with color like dmesg :D

You can use ```panic=1``` for panic auto-reboot.

Ensure ```log_buf_len=16M``` is larger than ```QR_BUF_SIZE``` so you won't miss very verbose messages

## Usefulness

It is only really useful in a niche set of circumstances, specifically when mainlining Android devices, Just early enough that only simple-framebuffer works. Usually this would be enough on its own, but you won't see any kernel messages before simple-framebuffer and framebuffer console was initialized, hence why I created this.

An example on how you can use this with initramfs for debugging usb, ufs, clk, can be found [here](https://gist.github.com/stableversion/fe864dde24405d2a163f54ae31a8b389) 

### Caveats

- This was hacked up in a couple of days, and largely written by AI as a proof of concept, if you couldn't tell from the code quality... but it miraculously works.
- Expect horrendous bugs


### drm_panic_qr

The QR code library originally came from [drm_panic_qr](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/drm_panic_qr.rs), but was "re-written" from Rust to C for the following reasons:
- I am too much of a novice to understand it
- Rust is incompatible on older Linux/Android kernel versions, i.e, >4.19 >6.1
- I have no clue how Claude managed to one-shot this, I am in fear

## TODO:
Clean up AI slop basically
- Fix ZSTD_estimateCCtxSize, anything higher than 3 fails.
- Sometimes framebuffer console can interfere and overwrite some qrcode data.
