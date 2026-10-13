// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "s7d-hevc-rpm.h"
#include "s7d-hevc-sequence.h"

enum s7d_hevc_rpm_word {
	RPM_SLICE_TYPE = 0x12,
	RPM_TEMPORAL_MVP = 0x13,
	RPM_DEPENDENT_SLICE = 0x14,
	RPM_SLICE_ADDRESS = 0x15,
	RPM_WIDTH = 0x17,
	RPM_HEIGHT = 0x18,
	RPM_MIN_CB_MINUS3 = 0x19,
	RPM_CTB_DIFF = 0x1a,
	RPM_POC_LOG2_MINUS4 = 0x1b,
	RPM_REORDER = 0x21,
	RPM_TILES = 0x23,
	RPM_TILE_COLUMNS_MINUS1 = 0x24,
	RPM_TILE_ROWS_MINUS1 = 0x25,
	RPM_MISC_FLAGS = 0x3a,
	RPM_FIRST_SLICE = 0x41,
	RPM_TEMPORAL_ID_PLUS1 = 0x42,
	RPM_NAL_TYPE = 0x43,
	RPM_BIT_DEPTH = 0x48,
	RPM_PROFILE_ETC = 0x49,
	RPM_SEI_FRAME_FIELD = 0x4a,
	RPM_CROP_FLAG = 0x6c,
	RPM_CROP_LEFT = 0x6d,
	RPM_CROP_RIGHT = 0x6e,
	RPM_CROP_TOP = 0x6f,
	RPM_CROP_BOTTOM = 0x70,
	RPM_CHROMA = 0x71,
	RPM_DPB_MINUS1 = 0x76,
};

int s7d_hevc_rpm_parse(const void *snapshot, size_t bytes,
		       bool efficiency_mode, struct s7d_hevc_rpm *rpm)
{
	struct s7d_hevc_rpm next;
	const u8 *data = snapshot;
	u32 i;

	if (!snapshot || !rpm || bytes != S7D_HEVC_RPM_BYTES)
		return -EINVAL;
	if (!efficiency_mode)
		return -EOPNOTSUPP;
	for (i = 0; i < S7D_HEVC_RPM_VALID_WORDS; i++)
		next.word[i] = get_unaligned_le16(data + i * sizeof(u16));
	*rpm = next;
	return 0;
}

static int s7d_hevc_host_check(const struct s7d_hevc_sps *host)
{
	u64 crop_x = (u64)host->crop_left + host->crop_right;
	u64 crop_y = (u64)host->crop_top + host->crop_bottom;

	if (!host->coded_width || !host->coded_height ||
	    crop_x * 2 >= host->coded_width || crop_y * 2 >= host->coded_height ||
	    host->log2_min_cb_size < 3 || host->log2_min_cb_size > 6 ||
	    host->log2_ctb_size < host->log2_min_cb_size ||
	    host->log2_ctb_size < 4 || host->log2_ctb_size > 6 ||
	    host->log2_max_poc_lsb < 4 || host->log2_max_poc_lsb > 16 ||
	    !host->max_dec_pic_buffering || host->max_dec_pic_buffering > 16 ||
	    host->num_reorder_pics >= host->max_dec_pic_buffering)
		return -EINVAL;
	if (host->profile_idc != 1 || host->max_sub_layers != 1 ||
	    host->chroma_format_idc != 1 || host->separate_colour_plane ||
	    host->bit_depth_luma != 8 || host->bit_depth_chroma != 8 ||
	    host->interlaced_source || host->field_seq ||
	    host->coded_width < 64 || host->coded_height < 64 ||
	    host->coded_width > 1920 || host->coded_height > 1088 ||
	    host->coded_height - crop_y * 2 > 1080)
		return -EOPNOTSUPP;
	if ((host->coded_width | host->coded_height) &
	    ((1U << host->log2_min_cb_size) - 1))
		return -EINVAL;
	return 0;
}

int s7d_hevc_rpm_plan_idr(const struct s7d_hevc_rpm *rpm,
			  const struct s7d_hevc_sps *host,
			  struct s7d_hevc_idr_plan *plan)
{
	struct s7d_hevc_idr_plan next = { };
	const u16 *word;
	u32 crop[4] = { };
	u32 min_cb, ctb, poc_log2;
	u32 i;
	int ret;

	if (!rpm || !host || !plan)
		return -EINVAL;
	ret = s7d_hevc_host_check(host);
	if (ret)
		return ret;
	word = rpm->word;
	if (word[RPM_FIRST_SLICE] > 1 || word[RPM_DEPENDENT_SLICE] > 1 ||
	    word[RPM_TEMPORAL_MVP] > 1 || word[RPM_TILES] > 1 ||
	    word[RPM_CROP_FLAG] > 1 || word[RPM_MIN_CB_MINUS3] > 3 ||
	    word[RPM_CTB_DIFF] > 3 || word[RPM_POC_LOG2_MINUS4] > 12)
		return -EPROTO;
	if (word[RPM_SLICE_TYPE] != 2 || word[RPM_FIRST_SLICE] != 1 ||
	    word[RPM_DEPENDENT_SLICE] || word[RPM_SLICE_ADDRESS] ||
	    word[RPM_TEMPORAL_MVP] || word[RPM_TEMPORAL_ID_PLUS1] != 1 ||
	    (word[RPM_NAL_TYPE] != 19 && word[RPM_NAL_TYPE] != 20) ||
	    word[RPM_TILES] || word[RPM_TILE_COLUMNS_MINUS1] ||
	    word[RPM_TILE_ROWS_MINUS1] || (word[RPM_BIT_DEPTH] & 0xff) ||
	    (word[RPM_PROFILE_ETC] & BIT(2)) ||
	    ((word[RPM_SEI_FRAME_FIELD] & BIT(8)) &&
	     (word[RPM_SEI_FRAME_FIELD] & GENMASK(6, 3))))
		return -EOPNOTSUPP;
	min_cb = word[RPM_MIN_CB_MINUS3] + 3;
	ctb = min_cb + word[RPM_CTB_DIFF];
	poc_log2 = word[RPM_POC_LOG2_MINUS4] + 4;
	if (word[RPM_CROP_FLAG])
		for (i = 0; i < ARRAY_SIZE(crop); i++)
			crop[i] = word[RPM_CROP_LEFT + i];
	if (word[RPM_WIDTH] != host->coded_width ||
	    word[RPM_HEIGHT] != host->coded_height ||
	    word[RPM_CHROMA] != host->chroma_format_idc ||
	    min_cb != host->log2_min_cb_size || ctb != host->log2_ctb_size ||
	    poc_log2 != host->log2_max_poc_lsb ||
	    word[RPM_DPB_MINUS1] + 1U != host->max_dec_pic_buffering ||
	    word[RPM_REORDER] != host->num_reorder_pics ||
	    crop[0] != host->crop_left || crop[1] != host->crop_right ||
	    crop[2] != host->crop_top || crop[3] != host->crop_bottom)
		return -ESTALE;
	next.coded_width = host->coded_width;
	next.coded_height = host->coded_height;
	next.ctb_size = 1U << ctb;
	next.ctb_columns = DIV_ROUND_UP(next.coded_width, next.ctb_size);
	next.ctb_rows = DIV_ROUND_UP(next.coded_height, next.ctb_size);
	next.ctb_count = next.ctb_columns * next.ctb_rows;
	next.nal_unit_type = word[RPM_NAL_TYPE];
	next.misc_flags = word[RPM_MISC_FLAGS];
	*plan = next;
	return 0;
}

int s7d_hevc_classify_status(u32 status, enum s7d_hevc_event *event)
{
	enum s7d_hevc_event next;

	if (!event)
		return -EINVAL;
	switch (status) {
	case S7D_HEVC_SLICE_HEADER_DONE:
		next = S7D_HEVC_EVENT_SLICE_HEADER;
		break;
	case 0x09:
		next = S7D_HEVC_EVENT_NAL_SEARCH;
		break;
	case S7D_HEVC_PICTURE_DATA_DONE:
		next = S7D_HEVC_EVENT_PICTURE_DATA;
		break;
	case 0x0c:
		next = S7D_HEVC_EVENT_SEI;
		break;
	case 0x0e:
		next = S7D_HEVC_EVENT_NAL_DONE;
		break;
	case 0x12:
		next = S7D_HEVC_EVENT_NEED_DATA;
		break;
	case 0x20:
	case 0x22:
	case 0x24:
		next = S7D_HEVC_EVENT_INPUT_EMPTY;
		break;
	case 0x21:
		return -ETIMEDOUT;
	case 0x23:
		return -E2BIG;
	case 0x25:
		return -EBADMSG;
	case 0x80:
		return -EOPNOTSUPP;
	case 0x0b:
	case 0x0f:
	case 0xfe:
		return -EIO;
	default:
		return -EPROTO;
	}
	*event = next;
	return 0;
}

int s7d_hevc_check_picture_done(const struct s7d_hevc_idr_plan *plan,
			       const struct s7d_hevc_picture_done *done)
{
	enum s7d_hevc_event event;
	int ret;

	if (!plan || !done || !plan->ctb_count || !done->max_shift_bytes)
		return -EINVAL;
	ret = s7d_hevc_classify_status(done->status, &event);
	if (ret)
		return ret;
	if (event != S7D_HEVC_EVENT_PICTURE_DATA)
		return -EAGAIN;
	if (!done->header_armed || done->accepted_headers != 1)
		return -EPROTO;
	if (done->error_latched || (done->shift_status & BIT(15)) ||
	    (done->parser_lcu_start & GENMASK(23, 0)) != plan->ctb_count - 1 ||
	    !done->shift_byte_count ||
	    done->shift_byte_count > done->max_shift_bytes)
		return -EBADMSG;
	return 0;
}
