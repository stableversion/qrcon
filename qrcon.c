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

/* Add extern declaration for registered_fb */
extern struct fb_info *registered_fb[FB_MAX];

#define QR_BUF_SIZE 14096
#define QR_TMP_BUF_SIZE (QR_BUF_SIZE * 3)
#define QR_MAX_MSG_SIZE 1200  /* Target compressed size for QR code */

/* QR code positioning macros */
#define QRPOS_CENTER      0
#define QRPOS_TOP_LEFT    1
#define QRPOS_TOP_RIGHT   2
#define QRPOS_BOTTOM_LEFT 3
#define QRPOS_BOTTOM_RIGHT 4
#define QRPOS_CUSTOM      5

/* Compression related defines */
#define QR_COMPRESSION_MAGIC 0x5A535444  /* "ZSTD" */
#define QR_COMPRESSION_HEADER_SIZE 8     /* 4 bytes magic + 4 bytes uncompressed size */
#define QR_TARGET_SIZE (QR_MAX_MSG_SIZE - 10) /* Target slightly below max */
#define QR_MIN_COMPRESS_SIZE 512  /* Minimum size worth optimizing */
#define QR_SKIP_SIZE 1024  /* Bytes to skip on compression error */

/* Maximum size of kernel message buffer to collect */
#define KMSG_MAX_BUFFER_SIZE (QR_BUF_SIZE * 10)

/* Minimal configurable parameters */
static int qr_position = QRPOS_TOP_RIGHT;
static int qr_x_offset = 10;
static int qr_y_offset = 200;
static int qr_size_percent = 60;
static int qr_border = 5;

/* Compression level (1-22, higher = better compression but slower) */
static int compression_level = 15;
module_param(compression_level, int, 0644);
MODULE_PARM_DESC(compression_level, "ZSTD compression level (1-22, higher = better compression)");

/* Framebuffer globals */
static struct fb_info *fb_info;
static u8 *fb_screen_base;
static u32 fb_screen_size;
static u32 bytes_per_pixel;
static u32 line_length;
static u32 xres, yres;

/* QR code buffers */
static u8 qr_data[QR_BUF_SIZE];
static u8 qr_tmp[QR_TMP_BUF_SIZE];
static size_t qr_data_len;
static u8 qr_width;

/* Compression buffers and context */
static ZSTD_CCtx *cctx;
static void *compression_workspace;
static size_t compression_workspace_size;
static u8 compressed_buf[QR_MAX_MSG_SIZE];

/* kmsg dumper data */
static bool qrcon_initialized = false;
static u8 history_data[KMSG_MAX_BUFFER_SIZE];
static size_t history_data_len = 0;
static size_t history_data_pos = 0;

/* Panic notification handling */
static bool panic_in_progress = false;
static bool panic_rendering_complete = false;

/* Delay between QR code updates in milliseconds */
static int qr_refresh_delay = 700;
module_param(qr_refresh_delay, int, 0644);
MODULE_PARM_DESC(qr_refresh_delay, "Delay between QR code updates (ms)");

/* Function prototypes */
static int qrcon_render_qr(void);
static void qrcon_clear_buffer(void);

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

/* Clear the QR data buffer */
static void qrcon_clear_buffer(void)
{
    memset(qr_data, 0, QR_BUF_SIZE);
    qr_data_len = 0;
}

/* Render QR code on the framebuffer */
static int qrcon_render_qr(void)
{
    int block_size;
    int start_x, start_y;
    int max_size_pixels, qr_render_width;
    int x, y;
    u8 *qr_ptr;
    u32 black = 0x00000000;
    u32 white = 0x00FFFFFF;

    if (!fb_screen_base)
        return -EINVAL;
    if (qr_data_len == 0)
        return 0; // Nothing to encode

    pr_debug("qrcon: Rendering QR code from kernel messages: %zu bytes\n", qr_data_len);

    /* Generate QR code using external library */
    qr_width = qr_generate(NULL, qr_data, qr_data_len, QR_BUF_SIZE, qr_tmp, QR_TMP_BUF_SIZE);
    if (qr_width == 0)
        return -EINVAL;

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

    /* Render QR modules (black squares) */
    qr_ptr = qr_data;
    for (y = 0; y < qr_width; y++) {
        for (x = 0; x < qr_width; x++) {
            u8 byte = qr_ptr[y * ((qr_width + 7) / 8) + (x / 8)];
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
    size_t const cctx_size = ZSTD_estimateCCtxSize(3);
    size_t const workspace_size = cctx_size;

    compression_workspace = kmalloc(workspace_size, GFP_KERNEL);
    if (!compression_workspace)
        return -ENOMEM;

    compression_workspace_size = workspace_size;
    cctx = ZSTD_initStaticCCtx(compression_workspace, cctx_size);

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

/* Compress data optimally to maximize QR code utilization */
static size_t qrcon_compress_data(const void *src, size_t src_size, void *dst, size_t dst_size,
                                 size_t *processed_size)
{
    size_t compressed_size;
    u32 *header = (u32 *)dst;
    size_t low, high, mid, best_size = 0, best_compressed = 0, total_size;
    int level = compression_level;
    
    /* Clamp compression level to valid range */
    if (level < 1)
        level = 1;
    else if (level > 22)
        level = 22;
    
    /* Default to processing all input if successful */
    *processed_size = src_size;
    
    /* If src_size is already small, just compress directly */
    if (src_size < QR_MIN_COMPRESS_SIZE) {
        header[0] = QR_COMPRESSION_MAGIC;
        header[1] = (u32)src_size;
        
        compressed_size = ZSTD_compressCCtx(cctx, dst + QR_COMPRESSION_HEADER_SIZE,
                                         dst_size - QR_COMPRESSION_HEADER_SIZE,
                                         src, src_size, level);
        
        if (ZSTD_isError(compressed_size))
            return 0;
            
        return QR_COMPRESSION_HEADER_SIZE + compressed_size;
    }
    
    /* Binary search for optimal amount of data to compress */
    low = 1;
    high = src_size;
    
    while (low <= high) {
        mid = low + (high - low) / 2;
        
        /* Try compressing this amount of data */
        header[0] = QR_COMPRESSION_MAGIC;
        header[1] = (u32)mid;
        
        compressed_size = ZSTD_compressCCtx(cctx, dst + QR_COMPRESSION_HEADER_SIZE,
                                         dst_size - QR_COMPRESSION_HEADER_SIZE,
                                         src, mid, level);
        
        if (ZSTD_isError(compressed_size)) {
            high = mid - 1;
            continue;
        }
        
        total_size = compressed_size + QR_COMPRESSION_HEADER_SIZE;
        
        if (total_size <= QR_MAX_MSG_SIZE) {
            /* This fits, try to find a larger size that also fits */
            if (mid > best_size) {
                best_size = mid;
                best_compressed = compressed_size;
            }
            
            /* If we're very close to target, stop here */
            if (total_size >= QR_TARGET_SIZE) {
                *processed_size = mid;
                pr_debug("qrcon: Optimal compression found: %zu -> %zu (%u%%) at level %d\n", 
                        mid, total_size, (u32)((total_size * 100) / QR_MAX_MSG_SIZE), level);
                return total_size;
            }
            
            low = mid + 1;
        } else {
            /* Too big, try with less data */
            high = mid - 1;
        }
    }
    
    /* If we found a valid size, use it */
    if (best_size > 0) {
        header[0] = QR_COMPRESSION_MAGIC;
        header[1] = (u32)best_size;
        
        /* Recompress with the best size we found */
        compressed_size = ZSTD_compressCCtx(cctx, dst + QR_COMPRESSION_HEADER_SIZE,
                                         dst_size - QR_COMPRESSION_HEADER_SIZE,
                                         src, best_size, level);
        
        if (!ZSTD_isError(compressed_size)) {
            *processed_size = best_size;
            pr_debug("qrcon: Best compression: %zu -> %zu (%u%%) at level %d\n", 
                    best_size, compressed_size + QR_COMPRESSION_HEADER_SIZE,
                    (u32)(((compressed_size + QR_COMPRESSION_HEADER_SIZE) * 100) / QR_MAX_MSG_SIZE),
                    level);
            return QR_COMPRESSION_HEADER_SIZE + compressed_size;
        }
    }
    
    /* Compression failed */
    *processed_size = 0;
    return 0;
}

/* Process and render a chunk of data */
static size_t qrcon_process_chunk(const void *data, size_t data_size, bool render)
{
    size_t processed = 0;
    size_t compressed_size;
    
    /* Compress the data */
    compressed_size = qrcon_compress_data(data, data_size, compressed_buf, 
                                        QR_MAX_MSG_SIZE, &processed);
    
    if (compressed_size == 0 || processed == 0)
        return 0;
    
    /* Copy to QR buffer and render if requested */
    if (render) {
        memcpy(qr_data, compressed_buf, compressed_size);
        qr_data_len = compressed_size;
        
        pr_debug("qrcon: Compressed %zu -> %zu bytes (%u%% of QR capacity)\n", 
                processed, compressed_size, 
                (u32)((compressed_size * 100) / QR_MAX_MSG_SIZE));
        
        qrcon_render_qr();
        qrcon_clear_buffer();
    }
    
    return processed;
}

static void qrcon_process_history(void)
{
    size_t remaining;
    size_t processed;
    // Initial delay to allow scanner to focus properly
    bool first_delay = true;
    
    if (history_data_len == 0)
        return;
        
    pr_info("qrcon: Processing %zu bytes of historical kernel messages\n", history_data_len);
        
    /* Process the history buffer in optimally compressed chunks */
    while (history_data_pos < history_data_len) {
        remaining = history_data_len - history_data_pos;
        processed = qrcon_process_chunk(history_data + history_data_pos, remaining, true);
        
        if (processed == 0) {
            /* Skip some data on error */
            history_data_pos += (remaining < QR_SKIP_SIZE) ? remaining : QR_SKIP_SIZE;
            pr_err("qrcon: Skipping problematic history data\n");
            continue;
        }
        
        history_data_pos += processed;
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
    
    /* Reset history data buffers after processing */
    history_data_len = 0;
    history_data_pos = 0;
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

    /* Accumulate additional kernel messages from the log to capture panic messages */
    {
         struct kmsg_dump_iter iter;
         char temp_buf[QR_MAX_MSG_SIZE];
         size_t len;
         kmsg_dump_rewind(&iter);
         while (kmsg_dump_get_line(&iter, true, temp_buf, sizeof(temp_buf) - 1, &len)) {
              if (len == 0)
                  break;
              if (history_data_len + len < sizeof(history_data)) {
                  memcpy(history_data + history_data_len, temp_buf, len);
                  history_data_len += len;
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
    qrcon_clear_buffer();

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