/* SPDX-License-Identifier: GPL-2.0 */
/*
 * QR Code Generator Library Header
 *
 * This is a simple QR encoder that doesn't allocate memory and does all the work
 * on the stack or on the provided buffers. For simplification, it only supports
 * low error correction, and applies the first mask (checkerboard).
 *
 * Copyright (C) 2023 Linux Kernel Contributors
 */

#ifndef _QR_GENERATOR_H
#define _QR_GENERATOR_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * qr_generate() - Generate a QR code from the provided data
 * @url: The base URL of the QR code. It will be encoded as Binary segment.
 *       If NULL, data will be encoded as binary segment.
 * @data: A pointer to the binary data to be encoded. If URL is not NULL, it
 *        will be encoded efficiently as a numeric segment and appended to the URL.
 * @data_len: Length of the data that needs to be encoded, must be less than data_size.
 * @data_size: Size of data buffer, at least 4071 bytes to hold a V40 QR code.
 *             It will be overwritten with the QR code image.
 * @tmp: A temporary buffer that the QR code encoder will use to write the
 *       segments and ECC.
 * @tmp_size: Size of the temporary buffer, must be at least 3706 bytes for V40.
 * @qr_version: The specific QR version to use (1-40)
 *
 * This function generates a QR code containing the provided data. If a URL is 
 * provided, it is encoded as a Binary segment, and the data is encoded as a 
 * Numeric segment and appended to the URL. If no URL is provided, the data 
 * is encoded directly as a Binary segment.
 *
 * The QR code image is written as a 1-bit-per-pixel binary framebuffer to the data
 * buffer, with each new line starting at the next byte boundary. The data buffer
 * must be large enough to hold the QR code (at least 4071 bytes for a V40 code).
 *
 * The temporary buffer is used for internal operations and must be at least 3706
 * bytes for a V40 QR code.
 *
 * Return: Width of the QR code (each side in pixels) or 0 if encoding failed.
 */
u8 qr_generate(const char *url,
               u8 *data,
               size_t data_len,
               u8 qr_version,
               size_t data_size,
               u8 *tmp,
               size_t tmp_size);

/**
 * qr_max_data_size() - Calculate the maximum data size for a QR version
 * @version: QR code version (1-40)
 * @url_len: Length of the URL (0 if not using URL)
 *
 * This function calculates the maximum number of bytes that can be encoded in a QR
 * code of the specified version, taking into account the overhead of encoding the
 * URL (if provided) and the segment headers.
 *
 * If url_len > 0, the function accounts for both Binary segment (URL) and Numeric
 * segment (data) headers. If url_len = 0, it only accounts for one Binary segment
 * header.
 *
 * Return: Maximum number of data bytes that can be encoded, or 0 if version is invalid.
 */
size_t qr_max_data_size(u8 version, size_t url_len);

#ifdef __cplusplus
}
#endif

#endif /* _QR_GENERATOR_H */ 