# qrcon (PoC)

A proof-of-concept Linux kernel driver which continuously collects kernel messages (kmsg) and during a panic, dumps it all from the beginning. This provides a debugging solution when only Simple Framebuffer is available, and things like PSTORE, USB or UART are unavailable.

## Installing

Simply download the zip/git clone this repo and place it in drivers/misc, or wherever you want, ensuring to add the dir in Makefile and Kconfig of misc. i.e ```source "drivers/misc/qrcon/Kconfig"``` and ```obj-y		+= qrcon/```

### Decoding 
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

The qrcode generation will automatically trigger after a panic, if it doesn't, use ```panic=5``` in your cmdline. It likely won't survive a catostrophic panic, but it has not been tested.

- Download the [Binary Eye](https://github.com/markusfisch/BinaryEye) app on Android, enable continuous scanning. (optionally enable qrcode only mode)
- After scanning go to history, select everything, export as JSON, and share with localsend/termux, etc.
- Use decode.py "json" to decode with color like dmesg :D

## Usefulness

It is only really useful in a niche set of circumstances, specifically when mainlining Android devices, Just early enough that only simple-framebuffer works. Usually this would be enough on its own, but you won't see any kernel messages before simple-framebuffer and framebuffer console was initialized, hence why I created this.

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