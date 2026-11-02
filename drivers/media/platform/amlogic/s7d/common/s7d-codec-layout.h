/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_LAYOUT_H
#define S7D_CODEC_LAYOUT_H

#include <linux/bits.h>
#include <linux/types.h>

#define S7D_CODEC_STORAGE_ALIGNMENT	64U
#define S7D_CODEC_MAX_CAPTURE_BUFFERS	26U

enum s7d_codec_capture_format {
	S7D_CODEC_CAPTURE_NV12M,
	S7D_CODEC_CAPTURE_NV21M,
};

#define S7D_CODEC_COLOUR_SIGNAL_PRESENT		BIT(0)
#define S7D_CODEC_COLOUR_DESCRIPTION_PRESENT	BIT(1)
#define S7D_CODEC_COLOUR_FULL_RANGE			BIT(2)

struct s7d_codec_colour {
	u8 primaries;
	u8 transfer;
	u8 matrix;
	u8 flags;
};

struct s7d_codec_colourimetry {
	u32 colorspace;
	u32 xfer_func;
	u32 ycbcr_enc;
	u32 quantization;
};

struct s7d_codec_layout {
	enum s7d_codec_capture_format format;
	u32 coded_width;
	u32 coded_height;
	u32 visible_left;
	u32 visible_top;
	u32 visible_width;
	u32 visible_height;
	u32 plane_count;
	u32 pitch[2];
	u32 storage_height;
	u32 size[2];
	u32 total_size;
	u32 dpb_limit;
	u32 dpb_slots;
	u32 current_slots;
	u32 extra_surfaces;
	u32 min_capture_buffers;
	/* Bits per coded sample: 8 or 10; 0 before a stream is known reads as 8. */
	u32 bit_depth;
	struct s7d_codec_colourimetry colourimetry;
};

/*
 * Amlogic FBC written through the HEVC core's SAO MMU: a frame map of one
 * little-endian u32 page number per 4 KiB body page. Map a physically
 * contiguous body one to one; the rest of the map is cleared.
 */
int s7d_amfbc_mmu_map_contiguous(u32 body_address, u32 pages, u8 *map, size_t bytes);

int s7d_codec_resolve_colour(const struct s7d_codec_colour *colour,
			    u32 width, u32 height,
			    struct s7d_codec_colourimetry *colourimetry);
int s7d_codec_validate_layout(const struct s7d_codec_layout *layout);

#endif
