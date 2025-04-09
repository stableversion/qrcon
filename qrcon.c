/*
 * qrcon.c - QR code encoder for kernel messages
 *
 * This module captures kernel log messages using kmsg_dump mechanism,
 * encodes them into QR codes, and displays them on the framebuffer.
 * When a buffer of messages reaches approximately QR_MAX_MSG_SIZE bytes, a new
 * QR code is generated and displayed.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/string.h>
#include <linux/kmsg_dump.h>
#include <linux/delay.h>
#include <linux/zstd.h>
#include <linux/panic_notifier.h>
#include "qr_generator.h"

static int qr_version = 20; // around ~842 bytes
static int qr_refresh_delay = 700; // in ms
static int recent_only = 0;

#define QRCON_RECENT_ONLY_SIZE 8096

/* QR code positioning macros */
#define QRPOS_CENTER      0
#define QRPOS_TOP_LEFT    1
#define QRPOS_TOP_RIGHT   2
#define QRPOS_BOTTOM_LEFT 3
#define QRPOS_BOTTOM_RIGHT 4
#define QRPOS_CUSTOM      5

/* QR code position parameters */
static int qr_position = QRPOS_TOP_RIGHT;
static int qr_x_offset = 10;
static int qr_y_offset = 200;
static int qr_size_percent = 60;
static int qr_border = 5;

/* Maximum size of kernel message history buffer to collect (10MB) */
#define KMSG_HISTORY_BUF_SIZE (10 * 1024 * 1024)

/* Size for payload (compressed kmsg) buffer, also used for QR image output */
#define QR_PAYLOAD_AND_IMAGE_BUF_SIZE 8192
/* Temp workspace for qr_generate, needs >= 3706 bytes */
#define QR_TMP_WORKSPACE_SIZE 4096

/* Compression related defines */
#define QR_COMPRESSION_MAGIC 0x5A535444  /* "ZSTD" */
#define QR_COMPRESSION_HEADER_SIZE 8     /* 4 bytes magic + 4 bytes uncompressed size */
#define QR_SKIP_SIZE 1024  /* Bytes to skip on compression error */

/* Compression level (1-22) */
/* Annything above 3 is broken. TODO!!!!!!!! */
static int compression_level = 3;

/* Add extern declaration for registered_fb */
extern struct fb_info *registered_fb[FB_MAX];

/* Framebuffer globals */
static struct fb_info *fb_info;
static u8 *fb_screen_base;
static u32 fb_screen_size;
static u32 bytes_per_pixel;
static u32 line_length;
static u32 xres, yres;

/* QR code buffers */
/* Buffer holds compressed payload before qr_generate, overwritten with QR image after */
static u8 qr_payload_and_image_buf[QR_PAYLOAD_AND_IMAGE_BUF_SIZE];
static u8 qr_tmp_workspace[QR_TMP_WORKSPACE_SIZE];
static size_t qr_payload_len;
static u8 qr_width;

/* Compression context */
static ZSTD_CCtx *cctx;
static void *compression_workspace;
static size_t compression_workspace_size;

/* kmsg history data */
static bool qrcon_initialized = false;
static u8 kmsg_history_buf[KMSG_HISTORY_BUF_SIZE];
static size_t kmsg_history_len = 0;
static size_t kmsg_history_pos = 0;

/* Panic notification handling */
static bool panic_in_progress = false;
static bool panic_rendering_complete = false;

/* Function prototypes */
static int qrcon_render_qr(void);

/* Helper: Write a pixel's color into memory */
static inline void write_color_to_ptr(u8 *ptr, u32 color, u32 bpp)
{
    switch (bpp) {
    case 4:
        *(u32 *)ptr = color;
        break;
    case 3:
        ptr[0] = color & 0xFF;
        ptr[1] = (color >> 8) & 0xFF;
        ptr[2] = (color >> 16) & 0xFF;
        break;
    case 2:
        *(u16 *)ptr = color;
        break;
    case 1:
        *ptr = color;
        break;
    default:
        break;
    }
}

/* Draw rectangle on framebuffer */
static int qrcon_draw_rect(int x, int y, int width, int height, u32 color)
{
    int i, current_width;
    int max_y;
    u8 *row;
    int j;
    
    if (!fb_screen_base)
        return -EINVAL;
    max_y = (y + height > yres) ? yres : y + height;
    for (i = y; i < max_y; i++) {
        current_width = (x + width > xres) ? (xres - x) : width;
        if (current_width <= 0)
            continue;
        row = fb_screen_base + i * line_length + x * bytes_per_pixel;
        for (j = 0; j < current_width; j++) {
            write_color_to_ptr(row + j * bytes_per_pixel, color, bytes_per_pixel);
        }
    }
    return 0;
}

/* Open framebuffer, only fb0 is supported */
static int qrcon_open_fb(void)
{
    fb_info = registered_fb[0];
    if (!fb_info) {
        pr_err("qrcon: Failed to get fb_info for fb0\n");
        return -ENODEV;
    }
    fb_screen_base = fb_info->screen_base;
    fb_screen_size = fb_info->screen_size;
    bytes_per_pixel = fb_info->var.bits_per_pixel / 8;
    line_length = fb_info->fix.line_length;
    xres = fb_info->var.xres;
    yres = fb_info->var.yres;
    pr_info("qrcon: Framebuffer opened: %dx%d, %d bpp\n", xres, yres, fb_info->var.bits_per_pixel);
    return 0;
}

/* Render QR code on the framebuffer */
static int qrcon_render_qr(void)
{
    int block_size;
    int start_x, start_y;
    int max_size_pixels, qr_render_width;
    int x, y;
    u8 *qr_image_ptr; /* Pointer to the QR image data in the buffer */
    u32 black = 0x00000000;
    u32 white = 0x00FFFFFF;

    if (!fb_screen_base)
        return -EINVAL;
    /* Check if payload length is zero (nothing compressed yet) */
    if (qr_payload_len == 0)
        return 0; // Nothing to encode

    pr_debug("qrcon: Generating QR code from payload: %zu bytes\n", qr_payload_len);

    /* Generate QR code using external library.
     * qr_generate writes the image output into qr_payload_and_image_buf,
     * overwriting the compressed payload that was there.
     * It uses qr_tmp_workspace as temporary scratch space.
     */
    qr_width = qr_generate(NULL, qr_payload_and_image_buf, qr_payload_len,
                           (u8)qr_version,
                           QR_PAYLOAD_AND_IMAGE_BUF_SIZE, qr_tmp_workspace,
                           QR_TMP_WORKSPACE_SIZE);
    if (qr_width == 0) {
        pr_err("qrcon: qr_generate failed\n");
        return -EINVAL;
    }

    max_size_pixels = ((xres < yres) ? xres : yres) * qr_size_percent / 100;
    block_size = max_size_pixels / qr_width;
    if (block_size < 1)
        block_size = 1;
    qr_render_width = qr_width * block_size;

    /* Determine QR code position */
    switch (qr_position) {
    case QRPOS_CENTER:
        start_x = (xres - qr_render_width) / 2;
        start_y = (yres - qr_render_width) / 2;
        break;
    case QRPOS_TOP_LEFT:
        start_x = qr_x_offset;
        start_y = qr_y_offset;
        break;
    case QRPOS_TOP_RIGHT:
        start_x = xres - qr_render_width - qr_x_offset;
        start_y = qr_y_offset;
        break;
    case QRPOS_BOTTOM_LEFT:
        start_x = qr_x_offset;
        start_y = yres - qr_render_width - qr_y_offset;
        break;
    case QRPOS_BOTTOM_RIGHT:
        start_x = xres - qr_render_width - qr_x_offset;
        start_y = yres - qr_render_width - qr_y_offset;
        break;
    case QRPOS_CUSTOM:
        start_x = qr_x_offset;
        start_y = qr_y_offset;
        break;
    default:
        start_x = (xres - qr_render_width) / 2;
        start_y = (yres - qr_render_width) / 2;
    }

    if (start_x < 0) start_x = 0;
    if (start_y < 0) start_y = 0;
    if (start_x + qr_render_width > xres)
        start_x = xres - qr_render_width;
    if (start_y + qr_render_width > yres)
        start_y = yres - qr_render_width;

    /* Draw white border */
    qrcon_draw_rect(start_x - qr_border, start_y - qr_border,
                    qr_render_width + 2 * qr_border,
                    qr_render_width + 2 * qr_border, white);

    /* Render QR modules (black squares) from the generated image */
    qr_image_ptr = qr_payload_and_image_buf;
    for (y = 0; y < qr_width; y++) {
        for (x = 0; x < qr_width; x++) {
            u8 byte = qr_image_ptr[y * ((qr_width + 7) / 8) + (x / 8)];
            u8 bit = 0x80 >> (x % 8);
            if (byte & bit) {
                qrcon_draw_rect(start_x + x * block_size, start_y + y * block_size,
                                block_size, block_size, black);
            }
        }
    }

    /* Force framebuffer update to immediately display the QR code */
    if (fb_info && fb_info->fbops && fb_info->fbops->fb_pan_display) {
        struct fb_var_screeninfo var = fb_info->var;
        fb_info->fbops->fb_pan_display(&var, fb_info);
    }

    pr_debug("qrcon: QR code rendered at (%d,%d), size %dx%d\n",
            start_x, start_y, qr_render_width, qr_render_width);
    return 0;
}

/* Initialize compression */
static int qrcon_init_compression(void)
{
    size_t const workspace_size = ZSTD_estimateCStreamSize(compression_level);

    compression_workspace = kmalloc(workspace_size, GFP_KERNEL);
    if (!compression_workspace)
        return -ENOMEM;

    compression_workspace_size = workspace_size;
    cctx = ZSTD_initStaticCCtx(compression_workspace, workspace_size);

    if (!cctx) {
        kfree(compression_workspace);
        return -ENOMEM;
    }

    return 0;
}

/* Cleanup compression */
static void qrcon_cleanup_compression(void)
{
    if (compression_workspace)
        kfree(compression_workspace);
}

/* Compress data to fit within the target QR version capacity.
 * Attempts to compress the entire source buffer.
 * If the compressed data exceeds the capacity for the configured qr_version,
 * it fails and returns 0.
 * Writes compressed output directly to dst buffer.
 *
 * Return: total compressed size + header, or 0 on failure.
 */
static size_t qrcon_compress_data(const void *src, size_t src_size, void *dst, size_t dst_capacity,
                                 size_t *processed_size)
{
    size_t compressed_size;
    u32 *header = (u32 *)dst;
    size_t target_capacity;
    int level = compression_level;
    size_t dst_payload_capacity;
    size_t low, high, mid;
    size_t best_size = 0; /* Largest src size that fits */
    size_t best_compressed_payload = 0; /* Size of compressed payload for best_size */

    *processed_size = 0; /* Initialize */

    /* Validate qr_version */
    if (qr_version < 1 || qr_version > 40) {
        pr_err("qrcon: Invalid qr_version (%d), must be 1-40\n", qr_version);
        return 0;
    }

    /* Determine target capacity based on the configured version */
    target_capacity = qr_max_data_size((u8)qr_version, 0);
    if (target_capacity == 0) {
        pr_err("qrcon: Failed to get capacity for version %u\n", qr_version);
        return 0;
    }

    /* Clamp target_capacity to actual destination buffer size */
    if (target_capacity > dst_capacity) {
        pr_warn("qrcon: Version %u capacity (%zu) exceeds dst buffer (%zu), clamping.\n",
                qr_version, target_capacity, dst_capacity);
        target_capacity = dst_capacity;
    }

    /* Check if destination payload buffer is too small */
    if (target_capacity <= QR_COMPRESSION_HEADER_SIZE) {
         pr_err("qrcon: Target capacity too small for header (%zu <= %d)\n",
                target_capacity, QR_COMPRESSION_HEADER_SIZE);
        return 0;
    }
    dst_payload_capacity = target_capacity - QR_COMPRESSION_HEADER_SIZE;

    /* Clamp compression level */
    if (level < 1)
        level = 1;
    else if (level > 22)
        level = 22;

    /* Binary search for the largest chunk of src that fits target_capacity */
    low = 1;
    high = src_size;
    while (low <= high) {
        mid = low + (high - low) / 2;
        if (mid == 0) break; /* Avoid infinite loop if src_size is huge */

        /* Try compressing 'mid' bytes of src */
        /* Note: We don't need to write the header here yet */
        compressed_size = ZSTD_compressCCtx(cctx, dst + QR_COMPRESSION_HEADER_SIZE, /* Use dst as temp workspace */
                                         dst_payload_capacity,
                                         src, mid, level);

        if (ZSTD_isError(compressed_size)) {
            /* Compression error likely means 'mid' is too small or data is bad.
             * Try a smaller chunk, although ideally shouldn't usually happen here. */
            pr_debug("qrcon: ZSTD err (%s) compressing %zu bytes, trying smaller.\n",
                    ZSTD_getErrorName(compressed_size), mid);
            high = mid - 1;
            continue;
        }

        /* Check if compressed_size + header fits */
        if (QR_COMPRESSION_HEADER_SIZE + compressed_size <= target_capacity) {
            /* This fits. Record it as the best candidate so far.
             * Try to fit a larger chunk. */
            best_size = mid;
            best_compressed_payload = compressed_size;
            low = mid + 1;
        } else {
            /* Too big. Try to fit a smaller chunk. */
            high = mid - 1;
        }
    }

    /* Check if we found any size that fits */
    if (best_size > 0) {
        /* We found the largest prefix (best_size) that fits.
         * Now, perform the final compression of exactly best_size bytes. */
        header[0] = QR_COMPRESSION_MAGIC;
        header[1] = (u32)best_size; /* Header reflects the *uncompressed* size */

        compressed_size = ZSTD_compressCCtx(cctx, dst + QR_COMPRESSION_HEADER_SIZE,
                                         dst_payload_capacity,
                                         src, best_size, level);

        if (ZSTD_isError(compressed_size)) {
            /* This should ideally not happen if the search worked, but handle it. */
            pr_err("qrcon: ZSTD err (%s) on final compression of %zu bytes\n",
                    ZSTD_getErrorName(compressed_size), best_size);
            return 0; /* Indicate failure */
        }
        
        /* Verify the final compressed size is what we expected from the search */
        if (compressed_size != best_compressed_payload) {
            pr_warn("qrcon: Final compressed size %zu != search size %zu\n",
                    compressed_size, best_compressed_payload);
            /* Check if it *still* fits */
            if (QR_COMPRESSION_HEADER_SIZE + compressed_size > target_capacity) {
                 pr_err("qrcon: Final compressed size %zu unexpectedly overflowed capacity %zu\n",
                        QR_COMPRESSION_HEADER_SIZE + compressed_size, target_capacity);
                 return 0;
            }
        }
        
        *processed_size = best_size;
        pr_debug("qrcon: Compressed %zu -> %zu bytes (%u%% of V%d capacity %zu) at level %d\n",
                best_size, QR_COMPRESSION_HEADER_SIZE + compressed_size,
                (u32)(((QR_COMPRESSION_HEADER_SIZE + compressed_size) * 100) / target_capacity),
                qr_version, target_capacity, level);

        return QR_COMPRESSION_HEADER_SIZE + compressed_size;
    } else {
        /* No chunk size (not even 1 byte) could be compressed to fit */
        pr_warn("qrcon: Could not compress any prefix of %zu bytes to fit V%d capacity %zu\n",
                src_size, qr_version, target_capacity);
        *processed_size = 0;
        return 0;
    }
}

static void qrcon_process_history(void)
{
    size_t remaining;
    size_t processed_src = 0; /* How much source kmsg was processed */
    size_t compressed_size = 0; /* Size of the compressed payload */
    bool first_delay = true;
    size_t target_capacity; /* For logging */

    if (kmsg_history_len == 0)
        return;

    pr_info("qrcon: Processing %zu bytes of historical kernel messages for QR v%d\n",
            kmsg_history_len, qr_version);

    /* Validate qr_version here as well, before entering the loop */
    if (qr_version < 1 || qr_version > 40) {
        pr_err("qrcon: Invalid qr_version (%d) in process_history. Aborting.\n", qr_version);
        kmsg_history_len = 0; /* Prevent further processing */
        kmsg_history_pos = 0;
        return;
    }
    target_capacity = qr_max_data_size((u8)qr_version, 0);
    if (target_capacity == 0) {
         pr_err("qrcon: Failed to get capacity for version %u in process_history. Aborting.\n", qr_version);
         kmsg_history_len = 0;
         kmsg_history_pos = 0;
         return;
    }


    if (recent_only && kmsg_history_len > QRCON_RECENT_ONLY_SIZE) {
        pr_info("qrcon: Recent only mode: total history %zu, processing last %d bytes\n",
                kmsg_history_len, QRCON_RECENT_ONLY_SIZE);
        kmsg_history_pos = kmsg_history_len - QRCON_RECENT_ONLY_SIZE;
        /* Ensure pos is not negative or past the end (shouldn't happen with check above) */
        if (kmsg_history_pos >= kmsg_history_len) {
             pr_warn("qrcon: Invalid history position calculated in recent_only mode (%zu >= %zu), processing full history.\n",
                     kmsg_history_pos, kmsg_history_len);
             kmsg_history_pos = 0; /* Reset to start */
        }
        pr_debug("qrcon: Starting history processing from offset %zu\n", kmsg_history_pos);
    } else if (recent_only) {
         pr_info("qrcon: Recent only mode: total history %zu <= %d bytes, processing all.\n",
                 kmsg_history_len, QRCON_RECENT_ONLY_SIZE);
         kmsg_history_pos = 0; /* Ensure we start from 0 if condition isn't met */
    } else {
        kmsg_history_pos = 0; /* Ensure we start from 0 if recent_only is off */
    }

    /* Process the history buffer in chunks matching the entire remaining data */
    while (kmsg_history_pos < kmsg_history_len) {
        remaining = kmsg_history_len - kmsg_history_pos;

        /* Attempt to compress the *entire* remaining chunk */
        compressed_size = qrcon_compress_data(kmsg_history_buf + kmsg_history_pos,
                                            remaining,
                                            qr_payload_and_image_buf,
                                            QR_PAYLOAD_AND_IMAGE_BUF_SIZE,
                                            &processed_src);

        if (compressed_size == 0) {
            /* Compression failed OR no prefix fit the capacity.
             * Skip a fixed amount of the *original* source data and try again.
             * processed_src should be 0 in this case from qrcon_compress_data. */
            size_t skip_amount = (remaining < QR_SKIP_SIZE) ? remaining : QR_SKIP_SIZE;
            pr_err("qrcon: Skipping %zu bytes of history data after compression failure/overflow for QR v%d\n",
                   skip_amount, qr_version);
            kmsg_history_pos += skip_amount;
            continue; /* Try compressing the next chunk */
        }

        /* Set payload length for qr_render_qr */
        qr_payload_len = compressed_size;

        /* Render the QR code */
        qrcon_render_qr();
        /* Payload length is implicitly reset/ignored on next loop iteration */
        /* qr_payload_and_image_buf is overwritten by qr_generate inside qrcon_render_qr */

        kmsg_history_pos += processed_src; /* Advance by the amount successfully processed */

        /* Delay between QR codes */
        if (first_delay) {
            mdelay(2000);
            first_delay = false;
        } else {
            if (panic_in_progress)
                mdelay(qr_refresh_delay);
            else
                msleep(qr_refresh_delay);
        }
    }
        
    pr_info("qrcon: Completed processing historical kernel messages\n");
    
    /* Reset history data state after processing */
    kmsg_history_len = 0;
    kmsg_history_pos = 0;
    qr_payload_len = 0; /* Ensure payload length is zero after finishing */
}

/* Refactored qrcon_panic_notifier to capture panic messages by accumulating extra log lines */
static int qrcon_panic_notifier(struct notifier_block *nb, unsigned long event, void *buf)
{
    if (!qrcon_initialized)
        return NOTIFY_DONE;

    if (panic_in_progress && panic_rendering_complete)
        return NOTIFY_DONE;

    panic_in_progress = true;

    /* Flush pending messages from kernel log */
    kmsg_dump_get_buffer(NULL, true, NULL, 0, NULL);

    /* Debug: Print the first kmsg line for verification */
    {
         struct kmsg_dump_iter iter;
         char first_line[256];
         size_t line_len;
         kmsg_dump_rewind(&iter);
         if (kmsg_dump_get_line(&iter, true, first_line, sizeof(first_line) - 1, &line_len))
         {
             if (line_len > 0 && first_line[line_len - 1] == '\n')
                 first_line[line_len - 1] = '\0';
             else
                 first_line[line_len] = '\0';
             pr_debug("qrcon: First kmsg line: %s\n", first_line);
         }
    }

    /* Accumulate additional kernel messages from the log to capture panic messages */
    {
         struct kmsg_dump_iter iter;
         /* Use a static temporary buffer to avoid stack overflow */
         static char temp_line_buf[QR_PAYLOAD_AND_IMAGE_BUF_SIZE];
         size_t len;
         kmsg_dump_rewind(&iter);
         /* Read lines until buffer is full or no more lines */
         while (kmsg_history_len < KMSG_HISTORY_BUF_SIZE && 
                kmsg_dump_get_line(&iter, true, temp_line_buf, sizeof(temp_line_buf) - 1, &len)) 
         {
              if (len == 0)
                  break;
              /* Check if the new line fits */
              if (kmsg_history_len + len < KMSG_HISTORY_BUF_SIZE) {
                  memcpy(kmsg_history_buf + kmsg_history_len, temp_line_buf, len);
                  kmsg_history_len += len;
              } else {
                  /* Buffer full */
                  pr_warn("qrcon: kmsg history buffer full during panic, discarding remaining logs\n");
                  break; 
              }
         }
    }
    
    /* Process all accumulated kernel messages uniformly as QR codes */
    qrcon_process_history();
    pr_info("qrcon: Processed all dumped kernel messages as QR codes\n");

    panic_rendering_complete = true;
    return NOTIFY_DONE;
}

static struct notifier_block panic_nb = {
    .notifier_call = qrcon_panic_notifier,
    .priority = INT_MAX, /* Try to run early */
};

/* Module initialization */
static int __init qrcon_init(void)
{
    int ret;

    /* Initialize compression */
    ret = qrcon_init_compression();
    if (ret < 0) {
        pr_err("qrcon: Failed to initialize compression\n");
        return ret;
    }
    
    /* Initialize buffer */
    qr_payload_len = 0;

    /* Open framebuffer */
    ret = qrcon_open_fb();
    if (ret < 0) {
        qrcon_cleanup_compression();
        return ret;
    }

    /* Register panic notifier */
    ret = atomic_notifier_chain_register(&panic_notifier_list, &panic_nb);
    if (ret) {
        pr_err("qrcon: Failed to register panic notifier\n");
        qrcon_cleanup_compression();
        return ret;
    }

    qrcon_initialized = true;

    pr_info("qrcon: Module loaded, panic notifier registered successfully\n");
    return 0;
}

static void __exit qrcon_exit(void)
{
    qrcon_initialized = false;
    atomic_notifier_chain_unregister(&panic_notifier_list, &panic_nb);
    qrcon_cleanup_compression();
    pr_info("qrcon: Module exit\n");
}

module_init(qrcon_init);
module_exit(qrcon_exit);