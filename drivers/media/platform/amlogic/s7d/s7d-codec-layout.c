// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/videodev2.h>

#include "s7d-codec-layout.h"

int s7d_codec_resolve_colour(const struct s7d_codec_colour *colour,
			    u32 width, u32 height,
			    struct s7d_codec_colourimetry *colourimetry)
{
	struct s7d_codec_colourimetry next = { 0 };
	u32 flags;

	if (!colour || !colourimetry || !width || !height)
		return -EINVAL;
	flags = colour->flags;
	if (flags & ~(S7D_CODEC_COLOUR_SIGNAL_PRESENT |
		      S7D_CODEC_COLOUR_DESCRIPTION_PRESENT |
		      S7D_CODEC_COLOUR_FULL_RANGE))
		return -EINVAL;
	if (!(flags & S7D_CODEC_COLOUR_SIGNAL_PRESENT) && flags)
		return -EINVAL;

	next.colorspace = V4L2_MAP_COLORSPACE_DEFAULT(width <= 720 && height <= 576,
						    true);
	if (flags & S7D_CODEC_COLOUR_DESCRIPTION_PRESENT) {
		switch (colour->primaries) {
		case 1:
			next.colorspace = V4L2_COLORSPACE_REC709;
			break;
		case 5:
			next.colorspace = V4L2_COLORSPACE_470_SYSTEM_BG;
			break;
		case 6:
			next.colorspace = V4L2_COLORSPACE_SMPTE170M;
			break;
		case 4:
		case 7 ... 12:
		case 22:
			return -EOPNOTSUPP;
		}
	}
	next.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(next.colorspace);
	next.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(next.colorspace);
	next.quantization = flags & S7D_CODEC_COLOUR_FULL_RANGE ?
		V4L2_QUANTIZATION_FULL_RANGE :
		V4L2_MAP_QUANTIZATION_DEFAULT(false, next.colorspace, next.ycbcr_enc);
	if (flags & S7D_CODEC_COLOUR_DESCRIPTION_PRESENT) {
		switch (colour->transfer) {
		case 1:
		case 6:
			next.xfer_func = V4L2_XFER_FUNC_709;
			break;
		case 4:
		case 5:
		case 7 ... 18:
			return -EOPNOTSUPP;
		}
		switch (colour->matrix) {
		case 1:
			next.ycbcr_enc = V4L2_YCBCR_ENC_709;
			break;
		case 5:
		case 6:
			next.ycbcr_enc = V4L2_YCBCR_ENC_601;
			break;
		case 0:
		case 4:
		case 7 ... 17:
			return -EOPNOTSUPP;
		}
	}
	*colourimetry = next;
	return 0;
}

int s7d_codec_validate_layout(const struct s7d_codec_layout *layout)
{
	u32 pixels, right, bottom, y_bytes, uv_bytes, total, minimum;

	if (!layout)
		return -EINVAL;
	if (layout->format != S7D_CODEC_CAPTURE_NV12M &&
	    layout->format != S7D_CODEC_CAPTURE_NV21M)
		return -EOPNOTSUPP;
	if (layout->plane_count != 2 || layout->coded_width < 64 ||
	    layout->coded_height < 64 ||
	    ((layout->coded_width | layout->coded_height | layout->visible_left |
	      layout->visible_top | layout->visible_width | layout->visible_height) & 1) ||
	    !layout->visible_width || !layout->visible_height ||
	    layout->pitch[0] != layout->pitch[1] ||
	    layout->pitch[0] < layout->coded_width ||
	    layout->pitch[0] % S7D_CODEC_STORAGE_ALIGNMENT ||
	    layout->storage_height < layout->coded_height ||
	    layout->storage_height % S7D_CODEC_STORAGE_ALIGNMENT ||
	    !layout->dpb_limit || layout->dpb_limit > 16 ||
	    !layout->dpb_slots || layout->dpb_slots > layout->dpb_limit ||
	    !layout->current_slots)
		return -EINVAL;
	if (check_mul_overflow(layout->coded_width, layout->coded_height, &pixels) ||
	    check_add_overflow(layout->visible_left, layout->visible_width, &right) ||
	    check_add_overflow(layout->visible_top, layout->visible_height, &bottom) ||
	    check_mul_overflow(layout->pitch[0], layout->storage_height, &y_bytes) ||
	    check_add_overflow(layout->size[0], layout->size[1], &total) ||
	    check_add_overflow(layout->dpb_slots, layout->current_slots, &minimum) ||
	    check_add_overflow(minimum, layout->extra_surfaces, &minimum))
		return -EOVERFLOW;
	uv_bytes = y_bytes / 2;
	if (pixels > 4096U * 2304U || right > layout->coded_width ||
	    bottom > layout->coded_height || layout->size[0] < y_bytes ||
	    layout->size[1] < uv_bytes || layout->total_size != total ||
	    layout->min_capture_buffers != minimum ||
	    minimum > S7D_CODEC_MAX_CAPTURE_BUFFERS)
		return -ERANGE;
	return 0;
}
