// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/videodev2.h>

#include "s7d-h264-layout.h"

static int s7d_h264_max_dpb_mbs(const struct s7d_h264_sps *sps, u32 *max_mbs)
{
	u32 level = sps->level_idc;

	if ((sps->profile_idc == 66 || sps->profile_idc == 77) &&
	    level == 11 && sps->constraint_set3)
		level = 9;
	if (level == 9 && sps->profile_idc == 100)
		return -EINVAL;

	switch (level) {
	case 9:
	case 10:
		*max_mbs = 396;
		break;
	case 11:
		*max_mbs = 900;
		break;
	case 12:
	case 13:
	case 20:
		*max_mbs = 2376;
		break;
	case 21:
		*max_mbs = 4752;
		break;
	case 22:
	case 30:
		*max_mbs = 8100;
		break;
	case 31:
		*max_mbs = 18000;
		break;
	case 32:
		*max_mbs = 20480;
		break;
	case 40:
	case 41:
		*max_mbs = 32768;
		break;
	case 42:
		*max_mbs = 34816;
		break;
	case 50:
		*max_mbs = 110400;
		break;
	case 51:
		*max_mbs = 184320;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int s7d_h264_geometry(const struct s7d_h264_sps *sps,
			    struct s7d_codec_layout *layout, u32 *mb_count)
{
	u32 pixels, right, bottom, horizontal_crop, vertical_crop;
	u32 aligned;

	if (!sps->mb_width || !sps->mb_height)
		return -EINVAL;
	if (check_mul_overflow(sps->mb_width, 16U, &layout->coded_width) ||
	    check_mul_overflow(sps->mb_height, 16U, &layout->coded_height) ||
	    check_mul_overflow(sps->mb_width, sps->mb_height, mb_count) ||
	    check_mul_overflow(layout->coded_width, layout->coded_height, &pixels))
		return -EOVERFLOW;
	if (layout->coded_width < 64 || layout->coded_height < 64 ||
	    pixels > 4096U * 2304U ||
	    (layout->coded_width > layout->coded_height &&
	     (layout->coded_width > 4096 || layout->coded_height > 2304)) ||
	    (layout->coded_width < layout->coded_height &&
	     (layout->coded_width > 2304 || layout->coded_height > 4096)))
		return -ERANGE;

	if (check_mul_overflow(sps->crop_left, 2U, &layout->visible_left) ||
	    check_mul_overflow(sps->crop_right, 2U, &right) ||
	    check_mul_overflow(sps->crop_top, 2U, &layout->visible_top) ||
	    check_mul_overflow(sps->crop_bottom, 2U, &bottom) ||
	    check_add_overflow(layout->visible_left, right, &horizontal_crop) ||
	    check_add_overflow(layout->visible_top, bottom, &vertical_crop))
		return -EOVERFLOW;
	if (horizontal_crop >= layout->coded_width ||
	    vertical_crop >= layout->coded_height)
		return -EINVAL;
	layout->visible_width = layout->coded_width - horizontal_crop;
	layout->visible_height = layout->coded_height - vertical_crop;

	if (check_add_overflow(layout->coded_width,
			       S7D_CODEC_STORAGE_ALIGNMENT - 1, &aligned))
		return -EOVERFLOW;
	layout->pitch[0] = aligned & ~(S7D_CODEC_STORAGE_ALIGNMENT - 1);
	layout->pitch[1] = layout->pitch[0];
	if (check_add_overflow(layout->coded_height,
			       S7D_CODEC_STORAGE_ALIGNMENT - 1, &aligned))
		return -EOVERFLOW;
	layout->storage_height = aligned & ~(S7D_CODEC_STORAGE_ALIGNMENT - 1);
	if (check_mul_overflow(layout->pitch[0], layout->storage_height,
			       &layout->size[0]))
		return -EOVERFLOW;
	layout->size[1] = layout->size[0] / 2;
	if (check_add_overflow(layout->size[0], layout->size[1], &layout->total_size))
		return -EOVERFLOW;

	return 0;
}

int s7d_h264_build_layout(const struct s7d_h264_sps *sps,
			 enum s7d_codec_capture_format format,
			 u32 extra_surfaces, struct s7d_codec_layout *layout)
{
	struct s7d_codec_layout next = { .format = format, .plane_count = 2 };
	u32 max_mbs, mb_count;
	int ret;

	if (!sps || !layout)
		return -EINVAL;
	if (format != S7D_CODEC_CAPTURE_NV12M &&
	    format != S7D_CODEC_CAPTURE_NV21M)
		return -EOPNOTSUPP;
	if (!sps->frame_mbs_only || sps->separate_colour_plane ||
	    sps->chroma_format_idc != 1 || sps->bit_depth_luma != 8 ||
	    sps->bit_depth_chroma != 8 ||
	    (sps->profile_idc != 66 && sps->profile_idc != 77 &&
	     sps->profile_idc != 100))
		return -EOPNOTSUPP;

	ret = s7d_h264_max_dpb_mbs(sps, &max_mbs);
	if (ret)
		return ret;
	ret = s7d_h264_geometry(sps, &next, &mb_count);
	if (ret)
		return ret;
	ret = s7d_codec_resolve_colour(&sps->colour, next.coded_width,
				     next.coded_height, &next.colourimetry);
	if (ret)
		return ret;
	next.dpb_limit = min(max_mbs / mb_count, 16U);
	if (!next.dpb_limit || sps->max_num_ref_frames > next.dpb_limit)
		return -EINVAL;

	next.dpb_slots = next.dpb_limit;
	if (sps->bitstream_restriction) {
		if (sps->max_dec_frame_buffering > next.dpb_limit ||
		    sps->max_dec_frame_buffering < sps->max_num_ref_frames ||
		    sps->num_reorder_frames > sps->max_dec_frame_buffering)
			return -EINVAL;
		next.dpb_slots = max(sps->max_dec_frame_buffering, 1U);
	}
	next.current_slots = 1;
	next.extra_surfaces = extra_surfaces;
	if (check_add_overflow(next.dpb_slots, next.current_slots,
			       &next.min_capture_buffers) ||
	    check_add_overflow(next.min_capture_buffers, extra_surfaces,
			       &next.min_capture_buffers))
		return -EOVERFLOW;
	if (next.min_capture_buffers > S7D_CODEC_MAX_CAPTURE_BUFFERS)
		return -ERANGE;

	*layout = next;
	return 0;
}
