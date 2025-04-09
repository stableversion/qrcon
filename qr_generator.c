// SPDX-License-Identifier: GPL-2.0
/*
 * QR Code Generator Library
 *
 * This is a simple QR encoder that doesn't allocate memory and does all the work
 * on the stack or on the provided buffers. For simplification, it only supports
 * low error correction, and applies the first mask (checkerboard).
 *
 * The binary data must be a valid url parameter, so the easiest way is
 * to use base64 encoding. But this waste 25% of data space, so the
 * whole stack trace won't fit in the QR-Code. So instead it encodes
 * every 13bits of input into 4 decimal digits, and then use the
 * efficient numeric encoding, that encode 3 decimal digits into
 * 10bits. This makes 39bits of compressed data into 12 decimal digits,
 * into 40bits in the QR-Code, so wasting only 2.5%. And numbers are
 * valid url parameter, so the website can do the reverse, to get the
 * binary data.
 *
 * Copyright (C) 2023 Linux Kernel Contributors
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>
#include "qr_generator.h"

/* Maximum sizes for buffers */
#define MAX_EC_SIZE 30
#define MAX_BLK_SIZE 123

/* Segment mode bits */
#define MODE_STOP 0
#define MODE_NUMERIC 1
#define MODE_BINARY 4

/* Padding bytes */
static const u8 PADDING[2] = {236, 17};

/**
 * struct qr_version - QR code version information
 * @version: Version number (1-40)
 */
struct qr_version {
	u8 version;
};

/**
 * enum segment_type - Type of QR code segment
 * @SEGMENT_BINARY: Binary segment (8-bit bytes)
 * @SEGMENT_NUMERIC: Numeric segment (decimal digits)
 */
enum segment_type {
	SEGMENT_BINARY,
	SEGMENT_NUMERIC
};

/**
 * struct qr_segment - QR code data segment
 * @type: Type of segment (binary or numeric)
 * @data: Pointer to segment data
 * @length: Length of segment data in bytes
 */
struct qr_segment {
	enum segment_type type;
	const u8 *data;
	size_t length;
};

/**
 * struct encoded_msg - Encoded message with error correction
 * @data: Buffer containing encoded data
 * @ec_size: Error correction code size
 * @g1_blocks: Number of blocks in group 1
 * @g2_blocks: Number of blocks in group 2
 * @g1_blk_size: Block size in group 1
 * @g2_blk_size: Block size in group 2 (always g1_blk_size + 1)
 * @poly: Pointer to generator polynomial
 * @version: QR code version
 */
struct encoded_msg {
	u8 *data;
	u8 ec_size;
	u8 g1_blocks;
	u8 g2_blocks;
	u8 g1_blk_size;
	u8 g2_blk_size;
	const u8 *poly;
	struct qr_version version;
};

/**
 * struct qr_image - QR code image
 * @data: Buffer containing QR code image
 * @width: Width of QR code in modules
 * @stride: Bytes per row in the buffer
 * @version: QR code version
 */
struct qr_image {
	u8 *data;
	u8 width;
	u8 stride;
	struct qr_version version;
};

/* Generator polynomials for ECC, only those needed for low quality */
static const u8 P7[7] = {87, 229, 146, 149, 238, 102, 21};
static const u8 P10[10] = {251, 67, 46, 61, 118, 70, 64, 94, 32, 45};
static const u8 P15[15] = {
	8, 183, 61, 91, 202, 37, 51, 58, 58, 237, 140, 124, 5, 99, 105,
};
static const u8 P18[18] = {
	215, 234, 158, 94, 184, 97, 118, 170, 79, 187, 152, 148, 252, 179, 5, 98, 96, 153,
};
static const u8 P20[20] = {
	17, 60, 79, 50, 61, 163, 26, 187, 202, 180, 221, 225, 83, 239, 156, 164, 212, 212, 188, 190,
};
static const u8 P22[22] = {
	210, 171, 247, 242, 93, 230, 14, 109, 221, 53, 200, 74, 8, 172, 98, 80, 219, 134, 160, 105,
	165, 231,
};
static const u8 P24[24] = {
	229, 121, 135, 48, 211, 117, 251, 126, 159, 180, 169, 152, 192, 226, 228, 218, 111, 0, 117,
	232, 87, 96, 227, 21,
};
static const u8 P26[26] = {
	173, 125, 158, 2, 103, 182, 118, 17, 145, 201, 111, 28, 165, 53, 161, 21, 245, 142, 13, 102,
	48, 227, 153, 145, 218, 70,
};
static const u8 P28[28] = {
	168, 223, 200, 104, 224, 234, 108, 180, 110, 190, 195, 147, 205, 27, 232, 201, 21, 43, 245, 87,
	42, 195, 212, 119, 242, 37, 9, 123,
};
static const u8 P30[30] = {
	41, 173, 145, 152, 216, 31, 179, 182, 50, 48, 110, 86, 239, 96, 222, 125, 42, 173, 226, 193,
	224, 130, 156, 37, 251, 216, 238, 40, 192, 180,
};

/**
 * struct qr_version_param - QR code parameters for Low quality ECC
 * @poly: Error correction polynomial
 * @g1_blocks: Number of blocks in group 1
 * @g2_blocks: Number of blocks in group 2
 * @g1_blk_size: Block size in group 1
 */
struct qr_version_param {
	const u8 *poly;
	u8 g1_blocks;
	u8 g2_blocks;
	u8 g1_blk_size;
};

/* QR version parameters for Low quality ECC */
static const struct qr_version_param VPARAM[40] = {
	{P7, 1, 0, 19},     /* V1 */
	{P10, 1, 0, 34},    /* V2 */
	{P15, 1, 0, 55},    /* V3 */
	{P20, 1, 0, 80},    /* V4 */
	{P26, 1, 0, 108},   /* V5 */
	{P18, 2, 0, 68},    /* V6 */
	{P20, 2, 0, 78},    /* V7 */
	{P24, 2, 0, 97},    /* V8 */
	{P30, 2, 0, 116},   /* V9 */
	{P18, 2, 2, 68},    /* V10 */
	{P20, 4, 0, 81},    /* V11 */
	{P24, 2, 2, 92},    /* V12 */
	{P26, 4, 0, 107},   /* V13 */
	{P30, 3, 1, 115},   /* V14 */
	{P22, 5, 1, 87},    /* V15 */
	{P24, 5, 1, 98},    /* V16 */
	{P28, 1, 5, 107},   /* V17 */
	{P30, 5, 1, 120},   /* V18 */
	{P28, 3, 4, 113},   /* V19 */
	{P28, 3, 5, 107},   /* V20 */
	{P28, 4, 4, 116},   /* V21 */
	{P28, 2, 7, 111},   /* V22 */
	{P30, 4, 5, 121},   /* V23 */
	{P30, 6, 4, 117},   /* V24 */
	{P26, 8, 4, 106},   /* V25 */
	{P28, 10, 2, 114},  /* V26 */
	{P30, 8, 4, 122},   /* V27 */
	{P30, 3, 10, 117},  /* V28 */
	{P30, 7, 7, 116},   /* V29 */
	{P30, 5, 10, 115},  /* V30 */
	{P30, 13, 3, 115},  /* V31 */
	{P30, 17, 0, 115},  /* V32 */
	{P30, 17, 1, 115},  /* V33 */
	{P30, 13, 6, 115},  /* V34 */
	{P30, 12, 7, 121},  /* V35 */
	{P30, 6, 14, 121},  /* V36 */
	{P30, 17, 4, 122},  /* V37 */
	{P30, 4, 18, 122},  /* V38 */
	{P30, 20, 4, 117},  /* V39 */
	{P30, 19, 6, 118},  /* V40 */
};

/* Position of the alignment pattern grid - using a 2D array for cleaner code
 * Each row is zero-terminated to mark end of pattern data
 */
static const u8 ALIGNMENT_PATTERNS[40][8] = {
	{0},                            /* V1: empty array */
	{6, 18, 0},                     /* V2 */
	{6, 22, 0},                     /* V3 */
	{6, 26, 0},                     /* V4 */
	{6, 30, 0},                     /* V5 */
	{6, 34, 0},                     /* V6 */
	{6, 22, 38, 0},                 /* V7 */
	{6, 24, 42, 0},                 /* V8 */
	{6, 26, 46, 0},                 /* V9 */
	{6, 28, 50, 0},                 /* V10 */
	{6, 30, 54, 0},                 /* V11 */
	{6, 32, 58, 0},                 /* V12 */
	{6, 34, 62, 0},                 /* V13 */
	{6, 26, 46, 66, 0},             /* V14 */
	{6, 26, 48, 70, 0},             /* V15 */
	{6, 26, 50, 74, 0},             /* V16 */
	{6, 30, 54, 78, 0},             /* V17 */
	{6, 30, 56, 82, 0},             /* V18 */
	{6, 30, 58, 86, 0},             /* V19 */
	{6, 34, 62, 90, 0},             /* V20 */
	{6, 28, 50, 72, 94, 0},         /* V21 */
	{6, 26, 50, 74, 98, 0},         /* V22 */
	{6, 30, 54, 78, 102, 0},        /* V23 */
	{6, 28, 54, 80, 106, 0},        /* V24 */
	{6, 32, 58, 84, 110, 0},        /* V25 */
	{6, 30, 58, 86, 114, 0},        /* V26 */
	{6, 34, 62, 90, 118, 0},        /* V27 */
	{6, 26, 50, 74, 98, 122, 0},    /* V28 */
	{6, 30, 54, 78, 102, 126, 0},   /* V29 */
	{6, 26, 52, 78, 104, 130, 0},   /* V30 */
	{6, 30, 56, 82, 108, 134, 0},   /* V31 */
	{6, 34, 60, 86, 112, 138, 0},   /* V32 */
	{6, 30, 58, 86, 114, 142, 0},   /* V33 */
	{6, 34, 62, 90, 118, 146, 0},   /* V34 */
	{6, 30, 54, 78, 102, 126, 150, 0}, /* V35 */
	{6, 24, 50, 76, 102, 128, 154, 0}, /* V36 */
	{6, 28, 54, 80, 106, 132, 158, 0}, /* V37 */
	{6, 32, 58, 84, 110, 136, 162, 0}, /* V38 */
	{6, 26, 54, 82, 110, 138, 166, 0}, /* V39 */
	{6, 30, 58, 86, 114, 142, 170, 0}, /* V40 */
};

/* Version information for format V7-V40 */
static const u32 VERSION_INFORMATION[34] = {
	0x07C94,  /* 0b00_0111_1100_1001_0100 */
	0x085BC,  /* 0b00_1000_0101_1011_1100 */
	0x09A99,  /* 0b00_1001_1010_1001_1001 */
	0x0A4D3,  /* 0b00_1010_0100_1101_0011 */
	0x0BBF6,  /* 0b00_1011_1011_1111_0110 */
	0x0C762,  /* 0b00_1100_0111_0110_0010 */
	0x0D847,  /* 0b00_1101_1000_0100_0111 */
	0x0E60D,  /* 0b00_1110_0110_0000_1101 */
	0x0F928,  /* 0b00_1111_1001_0010_1000 */
	0x10B78,  /* 0b01_0000_1011_0111_1000 */
	0x1145D,  /* 0b01_0001_0100_0101_1101 */
	0x12A17,  /* 0b01_0010_1010_0001_0111 */
	0x13532,  /* 0b01_0011_0101_0011_0010 */
	0x149A6,  /* 0b01_0100_1001_1010_0110 */
	0x15683,  /* 0b01_0101_0110_1000_0011 */
	0x168C9,  /* 0b01_0110_1000_1100_1001 */
	0x177EC,  /* 0b01_0111_0111_1110_1100 */
	0x18EC4,  /* 0b01_1000_1110_1100_0100 */
	0x191E1,  /* 0b01_1001_0001_1110_0001 */
	0x1AFAB,  /* 0b01_1010_1111_1010_1011 */
	0x1B08E,  /* 0b01_1011_0000_1000_1110 */
	0x1CC1A,  /* 0b01_1100_1100_0001_1010 */
	0x1D33F,  /* 0b01_1101_0011_0011_1111 */
	0x1ED75,  /* 0b01_1110_1101_0111_0101 */
	0x1F250,  /* 0b01_1111_0010_0101_0000 */
	0x209D5,  /* 0b10_0000_1001_1101_0101 */
	0x216F0,  /* 0b10_0001_0110_1111_0000 */
	0x228BA,  /* 0b10_0010_1000_1011_1010 */
	0x2379F,  /* 0b10_0011_0111_1001_1111 */
	0x24B0B,  /* 0b10_0100_1011_0000_1011 */
	0x2542E,  /* 0b10_0101_0100_0010_1110 */
	0x26A64,  /* 0b10_0110_1010_0110_0100 */
	0x27541,  /* 0b10_0111_0101_0100_0001 */
	0x28C69,  /* 0b10_1000_1100_0110_1001 */
};

/* Format info for low quality ECC */
static const u16 FORMAT_INFOS_QR_L[8] = {
	0x77c4, 0x72f3, 0x7daa, 0x789d, 0x662f, 0x6318, 0x6c41, 0x6976,
};

/* Exponential table for Galois Field GF(256) */
static const u8 EXP_TABLE[256] = {
	1, 2, 4, 8, 16, 32, 64, 128, 29, 58, 116, 232, 205, 135, 19, 38, 76, 152, 45, 90, 180, 117,
	234, 201, 143, 3, 6, 12, 24, 48, 96, 192, 157, 39, 78, 156, 37, 74, 148, 53, 106, 212, 181,
	119, 238, 193, 159, 35, 70, 140, 5, 10, 20, 40, 80, 160, 93, 186, 105, 210, 185, 111, 222, 161,
	95, 190, 97, 194, 153, 47, 94, 188, 101, 202, 137, 15, 30, 60, 120, 240, 253, 231, 211, 187,
	107, 214, 177, 127, 254, 225, 223, 163, 91, 182, 113, 226, 217, 175, 67, 134, 17, 34, 68, 136,
	13, 26, 52, 104, 208, 189, 103, 206, 129, 31, 62, 124, 248, 237, 199, 147, 59, 118, 236, 197,
	151, 51, 102, 204, 133, 23, 46, 92, 184, 109, 218, 169, 79, 158, 33, 66, 132, 21, 42, 84, 168,
	77, 154, 41, 82, 164, 85, 170, 73, 146, 57, 114, 228, 213, 183, 115, 230, 209, 191, 99, 198,
	145, 63, 126, 252, 229, 215, 179, 123, 246, 241, 255, 227, 219, 171, 75, 150, 49, 98, 196, 149,
	55, 110, 220, 165, 87, 174, 65, 130, 25, 50, 100, 200, 141, 7, 14, 28, 56, 112, 224, 221, 167,
	83, 166, 81, 162, 89, 178, 121, 242, 249, 239, 195, 155, 43, 86, 172, 69, 138, 9, 18, 36, 72,
	144, 61, 122, 244, 245, 247, 243, 251, 235, 203, 139, 11, 22, 44, 88, 176, 125, 250, 233, 207,
	131, 27, 54, 108, 216, 173, 71, 142, 1,
};

/* Reverse exponential table for Galois Field GF(256) */
static const u8 LOG_TABLE[256] = {
	175, 0, 1, 25, 2, 50, 26, 198, 3, 223, 51, 238, 27, 104, 199, 75, 4, 100, 224, 14, 52, 141,
	239, 129, 28, 193, 105, 248, 200, 8, 76, 113, 5, 138, 101, 47, 225, 36, 15, 33, 53, 147, 142,
	218, 240, 18, 130, 69, 29, 181, 194, 125, 106, 39, 249, 185, 201, 154, 9, 120, 77, 228, 114,
	166, 6, 191, 139, 98, 102, 221, 48, 253, 226, 152, 37, 179, 16, 145, 34, 136, 54, 208, 148,
	206, 143, 150, 219, 189, 241, 210, 19, 92, 131, 56, 70, 64, 30, 66, 182, 163, 195, 72, 126,
	110, 107, 58, 40, 84, 250, 133, 186, 61, 202, 94, 155, 159, 10, 21, 121, 43, 78, 212, 229, 172,
	115, 243, 167, 87, 7, 112, 192, 247, 140, 128, 99, 13, 103, 74, 222, 237, 49, 197, 254, 24,
	227, 165, 153, 119, 38, 184, 180, 124, 17, 68, 146, 217, 35, 32, 137, 46, 55, 63, 209, 91, 149,
	188, 207, 205, 144, 135, 151, 178, 220, 252, 190, 97, 242, 86, 211, 171, 20, 42, 93, 158, 132,
	60, 57, 83, 71, 109, 65, 162, 31, 45, 67, 216, 183, 123, 164, 118, 196, 23, 73, 236, 127, 12,
	111, 246, 108, 161, 59, 82, 41, 157, 85, 170, 251, 96, 134, 177, 187, 204, 62, 90, 203, 89, 95,
	176, 156, 169, 160, 81, 11, 245, 22, 235, 122, 117, 44, 215, 79, 174, 213, 233, 230, 231, 173,
	232, 116, 214, 244, 234, 168, 80, 88, 175,
};

/* Number of bits to encode characters in numeric mode */
static const size_t NUM_CHARS_BITS[4] = {0, 4, 7, 10};
static const u16 POW10[4] = {1, 10, 100, 1000};

/**
 * struct segment_iterator - Iterator for segment data
 * @segment: Pointer to segment being iterated
 * @offset: Current bit offset in the segment data
 * @carry: Carried digits for numeric mode
 * @carry_len: Number of carried digits
 */
struct segment_iterator {
	const struct qr_segment *segment;
	size_t offset;
	u16 carry;
	size_t carry_len;
};

/**
 * struct encoded_msg_iterator - Iterator for encoded message
 * @em: Pointer to encoded message
 * @offset: Current offset in the interleaved data
 */
struct encoded_msg_iterator {
	const struct encoded_msg *em;
	size_t offset;
};

/* Function prototypes */
static struct qr_version qr_version_from_segments(const struct qr_segment *segments[], size_t count);
static size_t qr_segment_total_size_bits(const struct qr_segment *segment, struct qr_version version);
static u8 qr_version_width(struct qr_version version);
static size_t qr_version_max_data(struct qr_version version);
static size_t qr_version_ec_size(struct qr_version version);
static size_t qr_version_g1_blocks(struct qr_version version);
static size_t qr_version_g2_blocks(struct qr_version version);
static size_t qr_version_g1_blk_size(struct qr_version version);
static const u8 *qr_version_alignment_pattern(struct qr_version version);
static const u8 *qr_version_poly(struct qr_version version);
static u32 qr_version_info(struct qr_version version);

/**
 * qr_version_width() - Get the width of a QR code version
 * @version: The QR code version
 *
 * Return: Width in modules (pixels)
 */
static u8 qr_version_width(struct qr_version version)
{
	return (version.version * 4) + 17;
}

/**
 * qr_version_max_data() - Get maximum data capacity for a QR version
 * @version: The QR code version
 *
 * Return: Maximum data capacity in bytes
 */
static size_t qr_version_max_data(struct qr_version version)
{
	size_t v = version.version - 1;
	if (v >= 40)
		return 0;
	
	return VPARAM[v].g1_blk_size * VPARAM[v].g1_blocks +
	       (VPARAM[v].g1_blk_size + 1) * VPARAM[v].g2_blocks;
}

/**
 * qr_version_ec_size() - Get error correction size for a QR version
 * @version: The QR code version
 *
 * Return: Error correction size in bytes
 */
static size_t qr_version_ec_size(struct qr_version version)
{
	size_t v = version.version - 1;
	if (v >= 40)
		return 0;
	
	/* Length of the polynomial */
	return VPARAM[v].poly == NULL ? 0 : 
	       (VPARAM[v].poly == P7 ? 7 :
	        VPARAM[v].poly == P10 ? 10 :
	        VPARAM[v].poly == P15 ? 15 :
	        VPARAM[v].poly == P18 ? 18 :
	        VPARAM[v].poly == P20 ? 20 :
	        VPARAM[v].poly == P22 ? 22 :
	        VPARAM[v].poly == P24 ? 24 :
	        VPARAM[v].poly == P26 ? 26 :
	        VPARAM[v].poly == P28 ? 28 :
	        VPARAM[v].poly == P30 ? 30 : 0);
}

/**
 * qr_version_g1_blocks() - Get number of group 1 blocks for a QR version
 * @version: The QR code version
 *
 * Return: Number of blocks in group 1
 */
static size_t qr_version_g1_blocks(struct qr_version version)
{
	size_t v = version.version - 1;
	if (v >= 40)
		return 0;
	
	return VPARAM[v].g1_blocks;
}

/**
 * qr_version_g2_blocks() - Get number of group 2 blocks for a QR version
 * @version: The QR code version
 *
 * Return: Number of blocks in group 2
 */
static size_t qr_version_g2_blocks(struct qr_version version)
{
	size_t v = version.version - 1;
	if (v >= 40)
		return 0;
	
	return VPARAM[v].g2_blocks;
}

/**
 * qr_version_g1_blk_size() - Get block size for group 1 in a QR version
 * @version: The QR code version
 *
 * Return: Block size in bytes for group 1
 */
static size_t qr_version_g1_blk_size(struct qr_version version)
{
	size_t v = version.version - 1;
	if (v >= 40)
		return 0;
	
	return VPARAM[v].g1_blk_size;
}

/**
 * qr_version_alignment_pattern() - Get alignment pattern coordinates
 * @version: The QR code version
 *
 * Return: Pointer to array of alignment pattern coordinates (zero-terminated)
 */
static const u8 *qr_version_alignment_pattern(struct qr_version version)
{
	size_t v = version.version - 1;
	if (v >= 40)
		return NULL;
	
	return ALIGNMENT_PATTERNS[v];
}

/**
 * qr_version_poly() - Get error correction polynomial
 * @version: The QR code version
 *
 * Return: Pointer to error correction polynomial
 */
static const u8 *qr_version_poly(struct qr_version version)
{
	size_t v = version.version - 1;
	if (v >= 40)
		return NULL;
	
	return VPARAM[v].poly;
}

/**
 * qr_version_info() - Get version information bits
 * @version: The QR code version
 *
 * Return: Version information bits or 0 for versions 1-6
 */
static u32 qr_version_info(struct qr_version version)
{
	if (version.version >= 7 && version.version <= 40)
		return VERSION_INFORMATION[version.version - 7];
	else
		return 0;
}

/**
 * qr_segment_length_bits_count() - Get number of bits needed for segment length
 * @segment: The segment
 * @version: QR code version
 *
 * Return: Number of bits for segment length field
 */
static size_t qr_segment_length_bits_count(const struct qr_segment *segment, 
                                          struct qr_version version)
{
	switch (segment->type) {
	case SEGMENT_BINARY:
		return (version.version <= 9) ? 8 : 16;
	case SEGMENT_NUMERIC:
		if (version.version <= 9)
			return 10;
		else if (version.version <= 26)
			return 12;
		else
			return 14;
	default:
		return 0;
	}
}

/**
 * qr_segment_character_count() - Get number of characters in segment
 * @segment: The segment
 *
 * Return: Number of characters
 */
static size_t qr_segment_character_count(const struct qr_segment *segment)
{
	size_t data_bits, last_chars;
	
	switch (segment->type) {
	case SEGMENT_BINARY:
		return segment->length;
	case SEGMENT_NUMERIC:
		data_bits = segment->length * 8;
		last_chars = (data_bits % 13) ? ((data_bits % 13) + 1) / 3 : 0;
		/* 4 decimal numbers per 13 bits + remainder */
		return 4 * (data_bits / 13) + last_chars;
	default:
		return 0;
	}
}

/**
 * qr_segment_total_size_bits() - Calculate total bits needed for a segment
 * @segment: The segment
 * @version: QR code version
 *
 * Return: Total number of bits required for the segment
 */
static size_t qr_segment_total_size_bits(const struct qr_segment *segment,
                                       struct qr_version version)
{
	size_t data_size;
	size_t digits;
	
	switch (segment->type) {
	case SEGMENT_BINARY:
		data_size = segment->length * 8;
		break;
	case SEGMENT_NUMERIC:
		digits = qr_segment_character_count(segment);
		data_size = 10 * (digits / 3) + NUM_CHARS_BITS[digits % 3];
		break;
	default:
		return 0;
	}
	
	/* header + length + data */
	return 4 + qr_segment_length_bits_count(segment, version) + data_size;
}

/**
 * get_next_13b() - Extract next 13 bits (or less) from data at specified bit offset
 * @data: Data buffer
 * @data_len: Length of data buffer
 * @offset: Bit offset to start from
 * @size: Pointer to store the actual number of bits extracted
 *
 * Return: The extracted bits as u16 value, or 0 if offset is beyond data
 */
static u16 get_next_13b(const u8 *data, size_t data_len, size_t offset, size_t *size)
{
	size_t bit_size;
	size_t byte_off;
	size_t bit_off;
	u16 b;
	u16 first_byte;
	u16 number = 0;
	
	if (offset >= data_len * 8) {
		if (size)
			*size = 0;
		return 0;
	}
	
	bit_size = min_t(size_t, 13, data_len * 8 - offset);
	byte_off = offset / 8;
	bit_off = offset % 8;
	b = (bit_off + bit_size);
	
	/* First byte with bit offset adjustment */
	first_byte = (data[byte_off] & 0xFF);
	
	if (b <= 8) {
		/* All bits within the first byte */
		number = (first_byte << bit_off) >> (8 - bit_size);
	} else if (b <= 16) {
		/* Bits span first and second byte */
		number = (first_byte << bit_off) >> bit_off;  /* Clear left bits */
		number = (number << (b - 8));  /* Shift to proper position */
		
		if (byte_off + 1 < data_len)
			number += (data[byte_off + 1] >> (16 - b));
		else
			bit_size = 8 - bit_off; /* Adjust if we're at the end of data */
	} else {
		/* Bits span three bytes */
		if (byte_off + 2 < data_len) {
			number = ((first_byte << bit_off) >> bit_off) << (b - 8);  /* Clear left bits and shift */
			number += ((data[byte_off + 1] & 0xFF) << (b - 16));
			number += (data[byte_off + 2] >> (24 - b));
		} else if (byte_off + 1 < data_len) {
			/* Only two bytes available */
			number = ((first_byte << bit_off) >> bit_off) << (b - 8);
			number += (data[byte_off + 1] >> (16 - b));
			bit_size = 16 - bit_off; /* Adjust bit_size */
		}
	}
	
	if (size)
		*size = bit_size;
	
	return number;
}

/**
 * qr_version_from_segments() - Find the smallest QR version that can hold these segments
 * @segments: Array of segment pointers
 * @count: Number of segments
 *
 * Return: The appropriate QR version, version.version = 0 if no suitable version found
 */
static struct qr_version __maybe_unused qr_version_from_segments(const struct qr_segment *segments[], size_t count)
{
	size_t v;
	struct qr_version version;
	size_t total_bits;
	
	/* Try versions from 1 to 40 */
	for (v = 1; v <= 40; v++) {
		version.version = v;
		total_bits = 0;
		
		/* Sum up the bits needed for all segments */
		for (size_t i = 0; i < count; i++) {
			total_bits += qr_segment_total_size_bits(segments[i], version);
		}
		
		/* Check if this version can hold all segments plus STOP marker */
		if (qr_version_max_data(version) * 8 >= total_bits + 4) {
			return version;
		}
	}
	
	/* No suitable version found */
	version.version = 0;
	return version;
}

/**
 * qr_segment_get_header() - Get the segment header bits
 * @segment: The segment
 * @size: Pointer to store the bit size of the header
 *
 * Return: Header bits (mode indicator)
 */
static u16 qr_segment_get_header(const struct qr_segment *segment, size_t *size)
{
	if (size)
		*size = 4;  /* All mode indicators are 4 bits */
	
	switch (segment->type) {
	case SEGMENT_BINARY:
		return MODE_BINARY;
	case SEGMENT_NUMERIC:
		return MODE_NUMERIC;
	default:
		return 0;
	}
}

/**
 * qr_segment_get_length_field() - Get the segment length field
 * @segment: The segment
 * @version: QR code version
 * @size: Pointer to store the bit size of the length field
 *
 * Return: Length field bits
 */
static u16 qr_segment_get_length_field(const struct qr_segment *segment, 
                                     struct qr_version version,
                                     size_t *size)
{
	size_t len_bits = qr_segment_length_bits_count(segment, version);
	size_t char_count = qr_segment_character_count(segment);
	
	if (size)
		*size = len_bits;
	
	return char_count & ((1 << len_bits) - 1);
}

/**
 * qr_segment_iterator_init() - Initialize segment iterator
 * @iter: Iterator to initialize
 * @segment: Segment to iterate over
 */
static void qr_segment_iterator_init(struct segment_iterator *iter, 
                                   const struct qr_segment *segment)
{
	iter->segment = segment;
	iter->offset = 0;
	iter->carry = 0;
	iter->carry_len = 0;
}

/**
 * qr_segment_iterator_next() - Get next bits from segment iterator
 * @iter: The segment iterator
 * @bits: Pointer to store the bits
 * @size: Pointer to store the bit size
 *
 * Return: true if more data available, false if end of segment
 */
static bool qr_segment_iterator_next(struct segment_iterator *iter, u16 *bits, size_t *size)
{
	size_t bit_size;
	u16 number;
	size_t new_chars;
	size_t remaining;
	
	if (!iter || !bits || !size)
		return false;
	
	*bits = 0;
	*size = 0;
	
	switch (iter->segment->type) {
	case SEGMENT_BINARY:
		/* For binary segments, simply return the next byte */
		if (iter->offset < iter->segment->length) {
			*bits = iter->segment->data[iter->offset++];
			*size = 8;
			return true;
		}
		return false;
		
	case SEGMENT_NUMERIC:
		if (iter->carry_len == 3) {
			/* We have a full group of 3 digits, output it */
			*bits = iter->carry;
			*size = NUM_CHARS_BITS[iter->carry_len];
			iter->carry = 0;
			iter->carry_len = 0;
			return true;
		}
		
		/* Get next 13 bits from data */
		number = get_next_13b(iter->segment->data, iter->segment->length, 
		                      iter->offset, &bit_size);
		
		if (bit_size == 0) {
			/* End of data, output any remaining digits */
			if (iter->carry_len > 0) {
				*bits = iter->carry;
				*size = NUM_CHARS_BITS[iter->carry_len];
				iter->carry_len = 0;
				return true;
			}
			return false;
		}
		
		iter->offset += bit_size;
		
		/* Calculate how many new characters from these bits */
		new_chars = (bit_size == 1) ? 1 : (bit_size + 1) / 3;
		
		if (iter->carry_len + new_chars > 3) {
			/* We'll have more than 3 digits total, output first 3 */
			remaining = iter->carry_len + new_chars - 3;
			*bits = iter->carry * POW10[new_chars - remaining] + 
			        (number / POW10[remaining]);
			*size = NUM_CHARS_BITS[3];
			iter->carry = number % POW10[remaining];
			iter->carry_len = remaining;
		} else {
			/* Add digits to carry */
			*bits = iter->carry * POW10[new_chars] + number;
			*size = NUM_CHARS_BITS[iter->carry_len + new_chars];
			iter->carry_len = 0;
			iter->carry = 0;
		}
		
		return true;
		
	default:
		return false;
	}
}

/**
 * encoded_msg_init() - Initialize encoded message structure
 * @em: Encoded message to initialize
 * @segments: Array of segment pointers
 * @count: Number of segments
 * @data: Buffer to store encoded data
 * @data_size: Size of data buffer
 *
 * Return: true if initialization succeeded, false otherwise
 */
static bool encoded_msg_init(struct encoded_msg *em, 
                           const struct qr_segment *segments[],
                           size_t count,
                           u8 qr_version, /* The QR version to use */
                           u8 *data,
                           size_t data_size)
{
	struct qr_version version;
	size_t max_data;
	size_t ec_size;
	size_t g1_blocks;
	size_t g2_blocks;
	size_t g1_blk_size;
	size_t required_size;
	size_t total_bits = 0;
	size_t i;
	
	if (!em || !segments || !data || count == 0 || data_size == 0)
		return false;
	
	/* Validate and set the QR version */
	if (qr_version < 1 || qr_version > 40) {
		pr_err("qr_generator: Invalid QR version specified (%u)\\n", qr_version);
		return false;
	}
	version.version = qr_version;
	
	/* Calculate total bits required for segments + terminator */
	for (i = 0; i < count; i++) {
		total_bits += qr_segment_total_size_bits(segments[i], version);
	}
	total_bits += 4; /* Add 4 bits for the terminator */
	
	/* Calculate data sizes based on version */
	max_data = qr_version_max_data(version);
	/* Check if the chosen version can hold the required bits */
	if (total_bits > max_data * 8) {
		pr_err("qr_generator: Data (%zu bits) exceeds capacity (%zu bits) for version %u\\n",
		       total_bits, max_data * 8, version.version);
		return false; /* Version is too small for the data */
	}
	ec_size = qr_version_ec_size(version);
	g1_blocks = qr_version_g1_blocks(version);
	g2_blocks = qr_version_g2_blocks(version);
	g1_blk_size = qr_version_g1_blk_size(version);
	
	/* Ensure data buffer is large enough */
	required_size = max_data + ec_size * (g1_blocks + g2_blocks);
	if (data_size < required_size)
		return false;
	
	/* Initialize em structure */
	em->data = data;
	em->ec_size = ec_size;
	em->g1_blocks = g1_blocks;
	em->g2_blocks = g2_blocks;
	em->g1_blk_size = g1_blk_size;
	em->g2_blk_size = g1_blk_size + 1;
	em->poly = qr_version_poly(version);
	em->version = version;
	
	/* Clear the buffer */
	memset(data, 0, data_size);
	
	return true;
}

/**
 * encoded_msg_push() - Push bits into encoded message at specified offset
 * @em: Encoded message
 * @offset: Pointer to bit offset (will be updated)
 * @number: Bits to push
 * @len_bits: Number of bits in number
 */
static void encoded_msg_push(struct encoded_msg *em, size_t *offset, 
                           u16 number, size_t len_bits)
{
	size_t byte_off = *offset / 8;
	size_t bit_off = *offset % 8;
	size_t b = bit_off + len_bits;
	
	/* Handle different bit alignment cases */
	if (bit_off == 0 && b <= 8) {
		/* Aligned to byte boundary, all bits fit in one byte */
		em->data[byte_off] = (number << (8 - b)) & 0xFF;
	} else if (bit_off == 0) {
		/* Aligned to byte boundary, spans multiple bytes */
		em->data[byte_off] = (number >> (b - 8)) & 0xFF;
		em->data[byte_off + 1] = (number << (16 - b)) & 0xFF;
	} else if (b <= 8) {
		/* Not aligned, all bits fit in current byte */
		em->data[byte_off] |= (number << (8 - b)) & 0xFF;
	} else if (b <= 16) {
		/* Not aligned, spans two bytes */
		em->data[byte_off] |= (number >> (b - 8)) & 0xFF;
		em->data[byte_off + 1] = (number << (16 - b)) & 0xFF;
	} else {
		/* Not aligned, spans three bytes */
		em->data[byte_off] |= (number >> (b - 8)) & 0xFF;
		em->data[byte_off + 1] = (number >> (b - 16)) & 0xFF;
		em->data[byte_off + 2] = (number << (24 - b)) & 0xFF;
	}
	
	*offset += len_bits;
}

/**
 * encoded_msg_add_segments() - Add segments to encoded message
 * @em: Encoded message
 * @segments: Array of segment pointers
 * @count: Number of segments
 */
static void encoded_msg_add_segments(struct encoded_msg *em,
                                   const struct qr_segment *segments[],
                                   size_t count)
{
	size_t offset = 0;
	size_t size;
	u16 bits;
	struct segment_iterator iter;
	size_t i;
	size_t pad_offset;
	
	for (i = 0; i < count; i++) {
		/* Add segment header */
		bits = qr_segment_get_header(segments[i], &size);
		encoded_msg_push(em, &offset, bits, size);
		
		/* Add segment length */
		bits = qr_segment_get_length_field(segments[i], em->version, &size);
		encoded_msg_push(em, &offset, bits, size);
		
		/* Add segment data */
		qr_segment_iterator_init(&iter, segments[i]);
		while (qr_segment_iterator_next(&iter, &bits, &size)) {
			encoded_msg_push(em, &offset, bits, size);
		}
	}
	
	/* Add terminator */
	encoded_msg_push(em, &offset, MODE_STOP, 4);
	
	/* Add padding to byte boundary if needed */
	if (offset % 8 != 0) {
		encoded_msg_push(em, &offset, 0, 8 - (offset % 8));
	}
	
	/* Add padding bytes */
	pad_offset = offset / 8;
	for (i = pad_offset; i < qr_version_max_data(em->version); i++) {
		em->data[i] = PADDING[(i & 1) ^ (pad_offset & 1)];
	}
}

/**
 * encoded_msg_error_code_for_block() - Compute error correction for a block
 * @em: Encoded message
 * @offset: Offset of block in data
 * @size: Size of block
 * @ec_offset: Offset where to store error correction
 */
static void encoded_msg_error_code_for_block(struct encoded_msg *em,
                                          size_t offset,
                                          size_t size,
                                          size_t ec_offset)
{
	u8 tmp[MAX_BLK_SIZE + MAX_EC_SIZE] = {0};
	size_t lead_coeff;
	size_t log_lead_coeff;
	size_t i, j;
	
	/* Copy block data to temporary buffer */
	memcpy(tmp, em->data + offset, size);
	
	/* Reed-Solomon algorithm for generating error correction codewords */
	for (i = 0; i < size; i++) {
		lead_coeff = tmp[i];
		if (lead_coeff == 0)
			continue;
		
		log_lead_coeff = LOG_TABLE[lead_coeff];
		
		for (j = 0; j < em->ec_size; j++) {
			tmp[i + j + 1] ^= EXP_TABLE[(em->poly[j] + log_lead_coeff) % 255];
		}
	}
	
	/* Copy error correction to output */
	memcpy(em->data + ec_offset, tmp + size, em->ec_size);
}

/**
 * encoded_msg_compute_error_code() - Compute error correction for all blocks
 * @em: Encoded message
 */
static void encoded_msg_compute_error_code(struct encoded_msg *em)
{
	size_t offset = 0;
	size_t ec_offset = em->g1_blocks * em->g1_blk_size + 
	                 em->g2_blocks * em->g2_blk_size;
	
	/* Compute error correction for group 1 blocks */
	for (size_t i = 0; i < em->g1_blocks; i++) {
		encoded_msg_error_code_for_block(em, offset, em->g1_blk_size, ec_offset);
		offset += em->g1_blk_size;
		ec_offset += em->ec_size;
	}
	
	/* Compute error correction for group 2 blocks */
	for (size_t i = 0; i < em->g2_blocks; i++) {
		encoded_msg_error_code_for_block(em, offset, em->g2_blk_size, ec_offset);
		offset += em->g2_blk_size;
		ec_offset += em->ec_size;
	}
}

/**
 * encoded_msg_encode() - Encode segments and compute error correction
 * @em: Encoded message
 * @segments: Array of segment pointers
 * @count: Number of segments
 */
static void encoded_msg_encode(struct encoded_msg *em,
                             const struct qr_segment *segments[],
                             size_t count)
{
	encoded_msg_add_segments(em, segments, count);
	encoded_msg_compute_error_code(em);
}

/**
 * encoded_msg_iterator_init() - Initialize encoded message iterator
 * @iter: Iterator to initialize
 * @em: Encoded message to iterate over
 */
static void encoded_msg_iterator_init(struct encoded_msg_iterator *iter,
                                    const struct encoded_msg *em)
{
	iter->em = em;
	iter->offset = 0;
}

/**
 * encoded_msg_iterator_next() - Get next byte from encoded message
 * @iter: The message iterator
 * @byte: Pointer to store the next byte
 *
 * Return: true if byte was retrieved, false if end of data
 */
static bool encoded_msg_iterator_next(struct encoded_msg_iterator *iter, u8 *byte)
{
	const struct encoded_msg *em = iter->em;
	size_t blocks = em->g1_blocks + em->g2_blocks;
	size_t g1_end = em->g1_blocks * em->g1_blk_size;
	size_t g2_end = g1_end + em->g2_blocks * em->g2_blk_size;
	size_t ec_end = g2_end + em->ec_size * blocks;
	size_t offset;
	
	if (!em || !iter || !byte || iter->offset >= ec_end)
		return false;
	
	/* Interleaved output format */
	if (iter->offset < em->g1_blk_size * blocks) {
		/* Interleave group 1 and group 2 blocks */
		size_t blk = iter->offset % blocks;
		size_t blk_off = iter->offset / blocks;
		
		if (blk < em->g1_blocks)
			offset = blk * em->g1_blk_size + blk_off;
		else
			offset = g1_end + (blk - em->g1_blocks) * em->g2_blk_size + blk_off;
	} else if (iter->offset < g2_end) {
		/* Last byte of group 2 blocks */
		size_t blk2 = iter->offset - blocks * em->g1_blk_size;
		offset = g1_end + blk2 / em->g2_blk_size * em->g2_blk_size + em->g2_blk_size - 1;
	} else {
		/* EC blocks */
		size_t ec_offset = iter->offset - g2_end;
		size_t blk = ec_offset % blocks;
		size_t blk_off = ec_offset / blocks;
		offset = g2_end + blk * em->ec_size + blk_off;
	}
	
	*byte = em->data[offset];
	iter->offset++;
	return true;
}

/**
 * qr_image_init() - Initialize QR image
 * @qr: QR image to initialize
 * @em: Encoded message
 * @data: Buffer to store QR image
 * @data_size: Size of data buffer
 *
 * Return: true if initialization succeeded, false otherwise
 */
static bool qr_image_init(struct qr_image *qr,
                        const struct encoded_msg *em,
                        u8 *data,
                        size_t data_size)
{
	u8 width = qr_version_width(em->version);
	u8 stride = DIV_ROUND_UP(width, 8);
	size_t buffer_size = stride * width;
	
	/* Check if buffer is large enough */
	if (data_size < buffer_size)
		return false;
	
	qr->data = data;
	qr->width = width;
	qr->stride = stride;
	qr->version = em->version;
	
	/* Clear the buffer */
	memset(data, 0, buffer_size);
	
	return true;
}

/**
 * qr_image_set() - Set a module (pixel) to dark
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 */
static void qr_image_set(struct qr_image *qr, u8 x, u8 y)
{
	size_t offset = y * qr->stride + x / 8;
	u8 mask = 0x80 >> (x % 8);
	
	if (x < qr->width && y < qr->width)
		qr->data[offset] |= mask;
}

/**
 * qr_image_xor() - Invert a module (pixel)
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 */
static void qr_image_xor(struct qr_image *qr, u8 x, u8 y)
{
	size_t offset = y * qr->stride + x / 8;
	u8 mask = 0x80 >> (x % 8);
	
	if (x < qr->width && y < qr->width)
		qr->data[offset] ^= mask;
}

/**
 * qr_image_draw_square() - Draw a square with top-left corner at (x,y)
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 * @size: Size of square
 */
static void qr_image_draw_square(struct qr_image *qr, u8 x, u8 y, u8 size)
{
	u8 k;
	
	/* Draw top and bottom horizontal lines */
	for (k = 0; k < size + 1; k++) {
		qr_image_set(qr, x + k, y);
		qr_image_set(qr, x + k, y + size);
	}
	
	/* Draw left and right vertical lines */
	for (k = 1; k < size; k++) {
		qr_image_set(qr, x, y + k);
		qr_image_set(qr, x + size, y + k);
	}
}

/**
 * qr_image_is_finder() - Check if coordinates are in a finder pattern
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 *
 * Return: true if coordinates are in a finder pattern
 */
static bool qr_image_is_finder(const struct qr_image *qr, u8 x, u8 y)
{
	u8 end = qr->width - 8;
	
	return (x < 8 && y < 8) || 
	       (x < 8 && y >= end) || 
	       (x >= end && y < 8);
}

/**
 * qr_image_draw_finders() - Draw the three finder patterns
 * @qr: QR image
 */
static void qr_image_draw_finders(struct qr_image *qr)
{
	u8 k;
	
	/* Draw the three finder patterns */
	qr_image_draw_square(qr, 1, 1, 4);
	qr_image_draw_square(qr, qr->width - 6, 1, 4);
	qr_image_draw_square(qr, 1, qr->width - 6, 4);
	
	/* Draw horizontal separator lines */
	for (k = 0; k < 8; k++) {
		qr_image_set(qr, k, 7);
		qr_image_set(qr, qr->width - k - 1, 7);
		qr_image_set(qr, k, qr->width - 8);
	}
	
	/* Draw vertical separator lines */
	for (k = 0; k < 7; k++) {
		qr_image_set(qr, 7, k);
		qr_image_set(qr, qr->width - 8, k);
		qr_image_set(qr, 7, qr->width - 1 - k);
	}
}

/**
 * qr_image_is_alignment() - Check if coordinates are in an alignment pattern
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 *
 * Return: true if coordinates are in an alignment pattern
 */
static bool qr_image_is_alignment(const struct qr_image *qr, u8 x, u8 y)
{
	const u8 *positions = qr_version_alignment_pattern(qr->version);
	u8 ax, ay;
	
	for (int i = 0; positions[i] != 0; i++) {
		ax = positions[i];
		for (int j = 0; positions[j] != 0; j++) {
			ay = positions[j];
			
			/* Skip if alignment pattern overlaps a finder pattern */
			if (qr_image_is_finder(qr, ax, ay))
				continue;
			
			/* Check if coordinates are within this alignment pattern */
			if (x >= ax - 2 && x <= ax + 2 && y >= ay - 2 && y <= ay + 2)
				return true;
		}
	}
	
	return false;
}

/**
 * qr_image_draw_alignments() - Draw alignment patterns
 * @qr: QR image
 */
static void qr_image_draw_alignments(struct qr_image *qr)
{
	const u8 *positions = qr_version_alignment_pattern(qr->version);
	u8 x, y;
	size_t len = 0;
	size_t i, j;
	
	/* Get the length of the alignment pattern array */
	if (qr->version.version == 1) {
		/* Version 1 has no alignment patterns */
		return;
	} else if (qr->version.version <= 6) {
		len = 2; /* Versions 2-6 have 2 alignment patterns */
	} else if (qr->version.version <= 13) {
		len = 3; /* Versions 7-13 have 3 alignment patterns */
	} else if (qr->version.version <= 20) {
		len = 4; /* Versions 14-20 have 4 alignment patterns */
	} else if (qr->version.version <= 27) {
		len = 5; /* Versions 21-27 have 5 alignment patterns */
	} else if (qr->version.version <= 34) {
		len = 6; /* Versions 28-34 have 6 alignment patterns */
	} else {
		len = 7; /* Versions 35-40 have 7 alignment patterns */
	}
	
	for (i = 0; i < len; i++) {
		x = positions[i];
		for (j = 0; j < len; j++) {
			y = positions[j];
			
			/* Skip if alignment pattern overlaps a finder pattern */
			if (qr_image_is_finder(qr, x, y))
				continue;
			
			qr_image_draw_square(qr, x - 1, y - 1, 2);
		}
	}
}

/**
 * qr_image_is_timing() - Check if coordinates are in a timing pattern
 * @x: X coordinate
 * @y: Y coordinate
 *
 * Return: true if coordinates are in a timing pattern
 */
static bool qr_image_is_timing(u8 x, u8 y)
{
	return x == 6 || y == 6;
}

/**
 * qr_image_draw_timing_patterns() - Draw timing patterns
 * @qr: QR image
 */
static void qr_image_draw_timing_patterns(struct qr_image *qr)
{
	u8 end = qr->width - 8;
	
	for (u8 i = 9; i < end; i += 2) {
		qr_image_set(qr, i, 6);
		qr_image_set(qr, 6, i);
	}
}

/**
 * qr_image_is_maskinfo() - Check if coordinates are in format info area
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 *
 * Return: true if coordinates are in format info area
 */
static bool qr_image_is_maskinfo(const struct qr_image *qr, u8 x, u8 y)
{
	u8 end = qr->width - 8;
	
	/* Format info around the finder patterns */
	return (x <= 8 && y == 8) || 
	       (y <= 8 && x == 8) || 
	       (x == 8 && y >= end) || 
	       (x >= end && y == 8);
}

/**
 * qr_image_draw_maskinfo() - Draw format information
 * @qr: QR image
 */
static void qr_image_draw_maskinfo(struct qr_image *qr)
{
	u16 info = FORMAT_INFOS_QR_L[0];
	u8 k, skip;
	
	/* Draw format info around the top left finder pattern */
	skip = 0;
	for (k = 0; k < 7; k++) {
		if (k == 6)
			skip = 1;
		
		if ((info & (1 << (14 - k))) == 0) {
			qr_image_set(qr, k + skip, 8);
			qr_image_set(qr, 8, qr->width - 1 - k);
		}
	}
	
	/* Draw format info around the bottom left and top right finder patterns */
	skip = 0;
	for (k = 0; k < 8; k++) {
		if (k == 2)
			skip = 1;
		
		if ((info & (1 << (7 - k))) == 0) {
			qr_image_set(qr, 8, 8 - skip - k);
			qr_image_set(qr, qr->width - 8 + k, 8);
		}
	}
}

/**
 * qr_image_is_version_info() - Check if coordinates are in version info area
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 *
 * Return: true if coordinates are in version info area
 */
static bool qr_image_is_version_info(const struct qr_image *qr, u8 x, u8 y)
{
	u32 vinfo = qr_version_info(qr->version);
	u8 pos = qr->width - 11;
	
	if (vinfo == 0)
		return false;
	
	return (x >= pos && x < pos + 3 && y < 6) || 
	       (y >= pos && y < pos + 3 && x < 6);
}

/**
 * qr_image_draw_version_info() - Draw version information
 * @qr: QR image
 */
static void qr_image_draw_version_info(struct qr_image *qr)
{
	u32 vinfo = qr_version_info(qr->version);
	u8 pos = qr->width - 11;
	
	if (vinfo == 0)
		return;
	
	for (u8 x = 0; x < 3; x++) {
		for (u8 y = 0; y < 6; y++) {
			if ((vinfo & (1 << (x + y * 3))) == 0) {
				qr_image_set(qr, x + pos, y);
				qr_image_set(qr, y, x + pos);
			}
		}
	}
}

/**
 * qr_image_is_reserved() - Check if coordinates are in a reserved area
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 *
 * Return: true if coordinates are in a reserved area
 */
static bool qr_image_is_reserved(const struct qr_image *qr, u8 x, u8 y)
{
	return qr_image_is_alignment(qr, x, y) ||
	       qr_image_is_finder(qr, x, y) ||
	       qr_image_is_timing(x, y) ||
	       qr_image_is_maskinfo(qr, x, y) ||
	       qr_image_is_version_info(qr, x, y);
}

/**
 * qr_image_is_last() - Check if coordinates are at the last module
 * @qr: QR image
 * @x: X coordinate
 * @y: Y coordinate
 *
 * Return: true if coordinates are at the last module
 */
static bool qr_image_is_last(const struct qr_image *qr, u8 x, u8 y)
{
	return x == 0 && y == qr->width - 1;
}

/**
 * qr_image_next() - Get coordinates of next module
 * @qr: QR image
 * @x: Pointer to X coordinate (will be updated)
 * @y: Pointer to Y coordinate (will be updated)
 */
static void qr_image_next(const struct qr_image *qr, u8 *x, u8 *y)
{
	u8 x_adj = (*x <= 6) ? *x + 1 : *x;
	u8 column_type = (qr->width - x_adj) % 4;
	
	switch (column_type) {
	case 2:
		if (*y > 0) {
			*x = *x + 1;
			*y = *y - 1;
		} else {
			*x = *x - 1;
		}
		break;
		
	case 0:
		if (*y < qr->width - 1) {
			*x = *x + 1;
			*y = *y + 1;
		} else {
			*x = *x - 1;
		}
		break;
		
	default:
		if (*x == 7) {
			*x = *x - 2;
		} else {
			*x = *x - 1;
		}
		break;
	}
}

/**
 * qr_image_next_available() - Get coordinates of next available module
 * @qr: QR image
 * @x: Pointer to X coordinate (will be updated)
 * @y: Pointer to Y coordinate (will be updated)
 */
static void qr_image_next_available(const struct qr_image *qr, u8 *x, u8 *y)
{
	qr_image_next(qr, x, y);
	
	while (qr_image_is_reserved(qr, *x, *y) && !qr_image_is_last(qr, *x, *y)) {
		qr_image_next(qr, x, y);
	}
}

/**
 * qr_image_draw_data() - Draw data modules
 * @qr: QR image
 * @iter: Iterator for encoded message
 */
static void qr_image_draw_data(struct qr_image *qr, struct encoded_msg_iterator *iter)
{
	u8 x = qr->width - 1;
	u8 y = qr->width - 1;
	u8 byte;
	int bit;
	
	/* Fill in data bits */
	while (encoded_msg_iterator_next(iter, &byte)) {
		for (bit = 7; bit >= 0; bit--) {
			/* If bit is 0, set the module (since our set() makes it light) */
			if (!(byte & (1 << bit))) {
				qr_image_set(qr, x, y);
			}
			
			if (!qr_image_is_last(qr, x, y)) {
				qr_image_next_available(qr, &x, &y);
			} else {
				break;
			}
		}
	}
	
	/* Set the remaining modules (if any) */
	while (!qr_image_is_last(qr, x, y)) {
		if (!qr_image_is_reserved(qr, x, y)) {
			qr_image_set(qr, x, y);
		}
		qr_image_next(qr, &x, &y);
	}
}

/**
 * qr_image_apply_mask() - Apply checkerboard mask (pattern 0)
 * @qr: QR image
 */
static void qr_image_apply_mask(struct qr_image *qr)
{
	for (u8 x = 0; x < qr->width; x++) {
		for (u8 y = 0; y < qr->width; y++) {
			if ((x ^ y) % 2 == 0 && !qr_image_is_reserved(qr, x, y)) {
				qr_image_xor(qr, x, y);
			}
		}
	}
}

/**
 * qr_image_draw() - Draw complete QR code
 * @qr: QR image
 * @em: Encoded message
 */
static void qr_image_draw(struct qr_image *qr, const struct encoded_msg *em)
{
	struct encoded_msg_iterator iter;
	
	/* Clear the image */
	memset(qr->data, 0, qr->stride * qr->width);
	
	/* Draw fixed patterns */
	qr_image_draw_finders(qr);
	qr_image_draw_alignments(qr);
	qr_image_draw_timing_patterns(qr);
	qr_image_draw_version_info(qr);
	
	/* Draw data */
	encoded_msg_iterator_init(&iter, em);
	qr_image_draw_data(qr, &iter);
	
	/* Draw format info and apply mask */
	qr_image_draw_maskinfo(qr);
	qr_image_apply_mask(qr);
}

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
 *
 * Return: Width of the QR code (each side in pixels) or 0 if encoding failed.
 */
u8 qr_generate(const char *url,
              u8 *data,
              size_t data_len,
              u8 qr_version,
              size_t data_size,
              u8 *tmp,
              size_t tmp_size)
{
	struct qr_segment binary_segment, numeric_segment;
	const struct qr_segment *segments[2];
	size_t count = 0;
	struct encoded_msg em;
	struct qr_image qr;
	
	/* Ensure minimum buffer sizes */
	if (data_size < 4071 || tmp_size < 3706)
		return 0;
	
	/* Validate QR version early */
	if (qr_version < 1 || qr_version > 40) {
		pr_err("qr_generator: Invalid QR version %u specified to qr_generate\\n", qr_version);
		return 0;
	}
	
	/* Setup segments according to parameters */
	if (url) {
		/* Create binary segment for URL */
		binary_segment.type = SEGMENT_BINARY;
		binary_segment.data = (const u8 *)url;
		binary_segment.length = strlen(url);
		segments[count++] = &binary_segment;
		
		/* Create numeric segment for data */
		numeric_segment.type = SEGMENT_NUMERIC;
		numeric_segment.data = data;
		numeric_segment.length = data_len;
		segments[count++] = &numeric_segment;
	} else {
		/* Create binary segment for data */
		binary_segment.type = SEGMENT_BINARY;
		binary_segment.data = data;
		binary_segment.length = data_len;
		segments[count++] = &binary_segment;
	}
	
	/* Initialize and encode the message */
	if (!encoded_msg_init(&em, segments, count, qr_version, tmp, tmp_size))
		return 0;
	
	encoded_msg_encode(&em, segments, count);
	
	/* Initialize and draw the QR image */
	if (!qr_image_init(&qr, &em, data, data_size))
		return 0;
	
	qr_image_draw(&qr, &em);
	
	return qr.width;
}
EXPORT_SYMBOL_GPL(qr_generate);

/**
 * qr_max_data_size() - Calculate the maximum data size for a QR version
 * @version: QR code version (1-40)
 * @url_len: Length of the URL (0 if not using URL)
 *
 * Return: Maximum number of bytes that can be encoded, or 0 if version is invalid.
 */
size_t qr_max_data_size(u8 version, size_t url_len)
{
	struct qr_version ver;
	size_t max_data;
	size_t max;
	
	if (version < 1 || version > 40)
		return 0;
	
	ver.version = version;
	max_data = qr_version_max_data(ver);
	
	if (url_len > 0) {
		/* Binary segment (URL) 4 + 16 bits, numeric segment (kmsg) 4 + 12 bits => 5 bytes */
		if (url_len + 5 >= max_data)
			return 0;
		
		/* Include 2.5% overhead for the numeric encoding */
		max = max_data - url_len - 5;
		return (max * 39) / 40;
	} else {
		/* Remove 3 bytes for binary segment (header 4 bits, length 16 bits, stop 4 bits) */
		return max_data - 3;
	}
}
EXPORT_SYMBOL_GPL(qr_max_data_size);

MODULE_AUTHOR("Certainly written by AI");
MODULE_DESCRIPTION("QR Code Generator Library");
MODULE_LICENSE("GPL");
MODULE_ALIAS("qr_generator"); 