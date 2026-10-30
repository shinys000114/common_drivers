// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/unaligned.h>

#include "s7d-h264-protocol.h"

#define RPM_DEPTH_LUMA		0x40U
#define RPM_DEPTH_CHROMA		0x41U
#define RPM_CROP_LEFT		0x6aU
#define RPM_CROP_RIGHT		0x6bU
#define RPM_SPS_FLAGS2		0x6cU
#define RPM_REORDER_FRAMES	0x6dU
#define RPM_BUFFER_FRAMES	0x6eU
#define RPM_CROP_TOP		0xc0U
#define RPM_CROP_BOTTOM		0xc6U
#define RPM_PROFILE		0xe7U

static u16 s7d_h264_rpm_word(const void *rpm, u32 index)
{
	return get_unaligned_le16((const u8 *)rpm + (index ^ 3U) * sizeof(u16));
}

int s7d_h264_parse_config(const struct s7d_h264_config_snapshot *snapshot,
			  enum s7d_codec_capture_format format, u32 extra_surfaces,
			  struct s7d_h264_sequence *sequence)
{
	struct s7d_h264_sequence next = { };
	struct s7d_h264_sps *sps = &next.sps;
	u32 mb_total;
	int ret;

	if (!snapshot || !sequence || !snapshot->rpm ||
	    snapshot->rpm_bytes < S7D_H264_RPM_WORDS * sizeof(u16) ||
	    snapshot->status != S7D_H264_CONFIG_REQUEST)
		return -EINVAL;

	sps->mb_width = snapshot->scratch1 & 0xff;
	mb_total = (snapshot->scratch1 >> 8) & 0xffff;
	if (!sps->mb_width && mb_total)
		sps->mb_width = 256;
	if (!sps->mb_width || !mb_total || mb_total % sps->mb_width)
		return -EINVAL;
	sps->mb_height = mb_total / sps->mb_width;
	sps->frame_mbs_only = !!(snapshot->scratch2 & (1U << 15));
	sps->chroma_format_idc = (snapshot->scratch2 >> 13) & 3;
	sps->profile_idc = s7d_h264_rpm_word(snapshot->rpm, RPM_PROFILE) >> 8;
	if (sps->profile_idc == 66 || sps->profile_idc == 77)
		sps->chroma_format_idc = 1;
	sps->level_idc = snapshot->scratchb & 0xff;
	sps->max_num_ref_frames = (snapshot->scratchb >> 8) & 0xff;
	/* The selected RPM mapping does not supply constraint_set3. */
	if ((sps->profile_idc == 66 || sps->profile_idc == 77) &&
	    sps->level_idc == 11)
		return -EOPNOTSUPP;

	sps->crop_left = s7d_h264_rpm_word(snapshot->rpm, RPM_CROP_LEFT);
	sps->crop_right = s7d_h264_rpm_word(snapshot->rpm, RPM_CROP_RIGHT);
	sps->crop_top = s7d_h264_rpm_word(snapshot->rpm, RPM_CROP_TOP);
	sps->crop_bottom = s7d_h264_rpm_word(snapshot->rpm, RPM_CROP_BOTTOM);
	sps->bit_depth_luma = s7d_h264_rpm_word(snapshot->rpm, RPM_DEPTH_LUMA) + 8;
	sps->bit_depth_chroma = s7d_h264_rpm_word(snapshot->rpm, RPM_DEPTH_CHROMA) + 8;
	sps->bitstream_restriction =
		!!(s7d_h264_rpm_word(snapshot->rpm, RPM_SPS_FLAGS2) & (1U << 3));
	sps->num_reorder_frames = s7d_h264_rpm_word(snapshot->rpm, RPM_REORDER_FRAMES);
	sps->max_dec_frame_buffering = s7d_h264_rpm_word(snapshot->rpm, RPM_BUFFER_FRAMES);

	ret = s7d_h264_build_layout(sps, format, extra_surfaces, &next.layout);
	if (ret)
		return ret;
	*sequence = next;
	return 0;
}

static int s7d_h264_descriptor_layout(const struct s7d_codec_layout *layout)
{
	u64 y_size;

	if (layout->format != S7D_CODEC_CAPTURE_NV12M &&
	    layout->format != S7D_CODEC_CAPTURE_NV21M)
		return -EOPNOTSUPP;
	if (layout->plane_count != 2 || !layout->coded_width ||
	    !layout->coded_height || layout->pitch[0] != layout->pitch[1] ||
	    layout->pitch[0] < layout->coded_width ||
	    layout->storage_height < layout->coded_height ||
	    (layout->pitch[0] & (S7D_CODEC_STORAGE_ALIGNMENT - 1)) ||
	    (layout->storage_height & (S7D_CODEC_STORAGE_ALIGNMENT - 1)))
		return -EINVAL;
	if ((layout->pitch[0] >> 3) > 0xfff || layout->storage_height > 0x1fff)
		return -ERANGE;
	y_size = (u64)layout->pitch[0] * layout->storage_height;
	if (y_size != layout->size[0] || y_size / 2 != layout->size[1] ||
	    y_size + y_size / 2 != layout->total_size)
		return -EINVAL;
	return 0;
}

static void s7d_h264_canvas_plan(struct s7d_h264_canvas_plan *canvas,
				u32 index, dma_addr_t dma, u32 pitch, u32 height)
{
	u32 width = pitch >> 3;

	canvas->index = index;
	canvas->data_low = (u32)(dma >> 3) | ((width & 7) << 29);
	canvas->data_high = (width >> 3) | (height << 9) | (7U << 26);
	canvas->block32 = (1U << 11) | (1U << 8) | index;
}

int s7d_h264_build_capture_descriptor(const struct s7d_codec_layout *layout,
				      u32 slot,
				      const struct s7d_h264_capture_plane planes[2],
				      struct s7d_h264_capture_descriptor *descriptor)
{
	struct s7d_h264_capture_descriptor next = { .slot = slot };
	u64 end[2];
	u32 y_index = slot * 2;
	u32 uv_index = y_index + 1;
	unsigned int plane;
	int ret;

	if (!layout || !planes || !descriptor || slot >= S7D_CODEC_MAX_CAPTURE_BUFFERS)
		return -EINVAL;
	ret = s7d_h264_descriptor_layout(layout);
	if (ret)
		return ret;
	for (plane = 0; plane < 2; plane++) {
		if ((planes[plane].dma & (S7D_CODEC_STORAGE_ALIGNMENT - 1)) ||
		    planes[plane].bytes < layout->size[plane])
			return -EINVAL;
		if (check_add_overflow((u64)planes[plane].dma,
				       (u64)planes[plane].bytes - 1, &end[plane]))
			return -EOVERFLOW;
		if (end[plane] > U32_MAX)
			return -ERANGE;
	}
	if ((u64)planes[0].dma <= end[1] && (u64)planes[1].dma <= end[0])
		return -EINVAL;

	next.canvas_cfg0 = 0x1ffU << 17;
	next.anc_offset = (0x990U + slot) * 4;
	next.anc_value = (uv_index << 16) | (uv_index << 8) | y_index;
	s7d_h264_canvas_plan(&next.canvas[0], y_index, planes[0].dma,
			     layout->pitch[0], layout->storage_height);
	s7d_h264_canvas_plan(&next.canvas[1], uv_index, planes[1].dma,
			     layout->pitch[1], layout->storage_height / 2);
	*descriptor = next;
	return 0;
}
