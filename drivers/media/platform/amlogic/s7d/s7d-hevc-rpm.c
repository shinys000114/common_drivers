// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "s7d-hevc-rpm.h"
#include "s7d-hevc-input.h"
#include "s7d-hevc-sequence.h"

enum s7d_hevc_rpm_word {
	RPM_ACTIVE_L0 = 0x10,
	RPM_ACTIVE_L1 = 0x11,
	RPM_SLICE_TYPE = 0x12,
	RPM_TEMPORAL_MVP = 0x13,
	RPM_DEPENDENT_SLICE = 0x14,
	RPM_SLICE_ADDRESS = 0x15,
	RPM_WIDTH = 0x17,
	RPM_HEIGHT = 0x18,
	RPM_MIN_CB_MINUS3 = 0x19,
	RPM_CTB_DIFF = 0x1a,
	RPM_POC_LOG2_MINUS4 = 0x1b,
	RPM_POC_LSB = 0x1c,
	RPM_COL_FROM_L0 = 0x1d,
	RPM_COL_INDEX = 0x1e,
	RPM_REORDER = 0x21,
	RPM_MODIFICATION = 0x22,
	RPM_TILES = 0x23,
	RPM_TILE_COLUMNS_MINUS1 = 0x24,
	RPM_TILE_ROWS_MINUS1 = 0x25,
	RPM_FIRST_SLICE = 0x41,
	RPM_TEMPORAL_ID_PLUS1 = 0x42,
	RPM_NAL_TYPE = 0x43,
	RPM_BIT_DEPTH = 0x48,
	RPM_PROFILE_ETC = 0x49,
	RPM_SEI_FRAME_FIELD = 0x4a,
	RPM_MODIFICATION_LIST = 0x4c,
	RPM_CROP_FLAG = 0x6c,
	RPM_CROP_LEFT = 0x6d,
	RPM_CROP_RIGHT = 0x6e,
	RPM_CROP_TOP = 0x6f,
	RPM_CROP_BOTTOM = 0x70,
	RPM_CHROMA = 0x71,
	RPM_DPB_MINUS1 = 0x76,
};

static bool hevc_slice_same(const struct s7d_hevc_slice_info *a,
			    const struct s7d_hevc_slice_info *b)
{
	u32 i, list;

	if (a->nal_type != b->nal_type || a->temporal_id_plus1 != b->temporal_id_plus1 ||
	    a->slice_type != b->slice_type || a->poc_lsb != b->poc_lsb ||
	    a->slice_address != b->slice_address || a->rps_count != b->rps_count ||
	    a->first_slice != b->first_slice || a->dependent_slice != b->dependent_slice ||
	    a->temporal_mvp != b->temporal_mvp ||
	    a->collocated_from_l0 != b->collocated_from_l0 ||
	    a->collocated_ref_idx != b->collocated_ref_idx)
		return false;
	for (i = 0; i < a->rps_count; i++)
		if (a->rps[i].delta_poc != b->rps[i].delta_poc ||
		    a->rps[i].used != b->rps[i].used ||
		    a->rps[i].long_term != b->rps[i].long_term)
			return false;
	for (list = 0; list < 2; list++) {
		if (a->active[list] != b->active[list] ||
		    a->modification[list] != b->modification[list])
			return false;
		if (a->modification[list])
			for (i = 0; i < a->active[list]; i++)
				if (a->modification_index[list][i] != b->modification_index[list][i])
					return false;
	}
	return true;
}

int s7d_hevc_rpm_decode_slice(const struct s7d_hevc_rpm *rpm,
			     const struct s7d_hevc_sps *host,
			     const struct s7d_hevc_slice_info *cpu,
			     struct s7d_hevc_slice_info *slice)
{
	struct s7d_hevc_slice_info next = { };
	struct s7d_hevc_geometry geometry;
	u32 i, list, offset = 0, used = 0;
	const u16 *word;
	int ret;

	if (!rpm || !host || !cpu || !slice)
		return -EINVAL;
	ret = s7d_hevc_rpm_check_sequence(rpm, host, &geometry);
	if (ret)
		return ret;
	if (cpu->rps_count > S7D_HEVC_RPS_ENTRIES || cpu->active[0] > 16 || cpu->active[1] > 16)
		return -EINVAL;
	word = rpm->word;
	next.nal_type = word[RPM_NAL_TYPE];
	next.temporal_id_plus1 = word[RPM_TEMPORAL_ID_PLUS1];
	next.slice_type = word[RPM_SLICE_TYPE];
	next.first_slice = word[RPM_FIRST_SLICE];
	next.dependent_slice = word[RPM_DEPENDENT_SLICE];
	next.slice_address = word[RPM_SLICE_ADDRESS];
	next.temporal_mvp = word[RPM_TEMPORAL_MVP];
	if (next.slice_type > 2 ||
	    (next.nal_type != 0 && next.nal_type != 1 &&
	     !(next.nal_type >= 6 && next.nal_type <= 9) &&
	     !(next.nal_type >= 16 && next.nal_type <= 21)))
		return -EOPNOTSUPP;
	if (next.nal_type >= 16 && next.slice_type != 2)
		return -EPROTO;
	if (next.nal_type != 19 && next.nal_type != 20) {
		next.poc_lsb = word[RPM_POC_LSB];
		if (next.poc_lsb >= (1U << host->log2_max_poc_lsb))
			return -EPROTO;
		for (i = 0; i < S7D_HEVC_RPS_ENTRIES; i++) {
			u16 entry = word[i];
			s32 delta;

			if (entry == 0x8000)
				break;
			if (entry & BIT(15))
				return -EPROTO;
			if (entry & BIT(14))
				return -EOPNOTSUPP;
			delta = entry & 0xfff;
			if (entry & BIT(12))
				delta -= 4096;
			if (!delta)
				return -EPROTO;
			next.rps[i].delta_poc = delta;
			next.rps[i].used = !!(entry & BIT(13));
			used += next.rps[i].used;
			next.rps_count++;
		}
		if (next.rps_count >= host->max_dec_pic_buffering)
			return -EPROTO;
	}
	if (next.slice_type != 2) {
		if (!used || word[RPM_MODIFICATION] & ~3U)
			return -EPROTO;
		for (list = 0; list < (next.slice_type == 0 ? 2U : 1U); list++) {
			next.active[list] = word[RPM_ACTIVE_L0 + list];
			if (!next.active[list] || next.active[list] > S7D_HEVC_RPS_ENTRIES)
				return -EPROTO;
			next.modification[list] = word[RPM_MODIFICATION] & BIT(list);
			if (next.modification[list]) {
				for (i = 0; i < next.active[list]; i++) {
					u32 index = word[RPM_MODIFICATION_LIST + offset++];

					if (index >= used)
						return -EPROTO;
					next.modification_index[list][i] = index;
				}
			}
		}
		next.collocated_from_l0 = next.slice_type == 1;
		if (next.temporal_mvp) {
			if (next.slice_type == 0) {
				if (word[RPM_COL_FROM_L0] > 1)
					return -EPROTO;
				next.collocated_from_l0 = word[RPM_COL_FROM_L0];
			}
			if (next.active[!next.collocated_from_l0] > 1)
				next.collocated_ref_idx = word[RPM_COL_INDEX];
			if (next.collocated_ref_idx >= next.active[!next.collocated_from_l0])
				return -EPROTO;
		}
	}
	next.no_output_of_prior_pics = cpu->no_output_of_prior_pics;
	next.pic_output_flag = cpu->pic_output_flag;
	next.parameter_epoch = cpu->parameter_epoch;
	if (!hevc_slice_same(&next, cpu))
		return -ESTALE;
	*slice = next;
	return 0;
}

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

int s7d_hevc_rpm_check_sequence(const struct s7d_hevc_rpm *rpm,
			       const struct s7d_hevc_sps *host,
			       struct s7d_hevc_geometry *geometry)
{
	struct s7d_hevc_geometry next;
	const u16 *word;
	u32 crop[4] = { };
	u32 min_cb, ctb, poc_log2, i;
	int ret;

	if (!rpm || !host || !geometry)
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
	if (word[RPM_FIRST_SLICE] != 1 || word[RPM_DEPENDENT_SLICE] ||
	    word[RPM_SLICE_ADDRESS] || word[RPM_TEMPORAL_ID_PLUS1] != 1 ||
	    word[RPM_TILES] || word[RPM_TILE_COLUMNS_MINUS1] || word[RPM_TILE_ROWS_MINUS1] ||
	    (word[RPM_BIT_DEPTH] & 0xff) || (word[RPM_PROFILE_ETC] & BIT(2)) ||
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
	*geometry = next;
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
