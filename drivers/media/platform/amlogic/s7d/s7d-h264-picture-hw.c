// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/regmap.h>

#include "s7d-h264-context.h"
#include "s7d-h264-dpb.h"
#include "s7d-h264-protocol.h"
#include "s7d-h264-picture-hw.h"

#define DOS_REG(word)		((word) * 4U)
#define H264_BUFFER_INFO_INDEX	DOS_REG(0x0c24)
#define H264_BUFFER_INFO_DATA	DOS_REG(0x0c22)
#define H264_POC_INDEX		DOS_REG(0x0c30)
#define H264_CURRENT_POC		DOS_REG(0x0c32)
#define H264_CO_MB_WR_ADDR	DOS_REG(0x0c38)
#define H264_CO_MB_RD_ADDR	DOS_REG(0x0c39)
#define H264_CO_MB_RW_CTL		DOS_REG(0x0c3d)
#define DBKR_CANVAS_ADDR		DOS_REG(0x09b0)
#define DBKW_CANVAS_ADDR		DOS_REG(0x09b1)
#define REC_CANVAS_ADDR		DOS_REG(0x09b2)
#define CURR_CANVAS_CTRL		DOS_REG(0x09b3)

struct s7d_h264_picture_values {
	u32 table[S7D_CODEC_MAX_CAPTURE_BUFFERS][3];
	u32 refs[2][S7D_H264_REF_LIST_SIZE / 4U];
	u32 list1_words;
	u32 mv_write;
	u32 mv_read;
	bool read_mv;
};

static bool s7d_h264_same_ref(const struct s7d_h264_picture_ref *a,
			     const struct s7d_h264_picture_ref *b)
{
	return a->slot == b->slot && a->frame_num == b->frame_num &&
		a->poc == b->poc && a->top_poc == b->top_poc &&
		a->bottom_poc == b->bottom_poc &&
		a->long_term_index == b->long_term_index &&
		a->mv_slot == b->mv_slot && a->direct_8x8 == b->direct_8x8;
}

static int s7d_h264_mv_address(const struct s7d_h264_mv_buffer *mv,
			      s32 slot, bool direct, u32 first_mb,
			      u32 *address)
{
	u32 base, bytes, offset, record_bytes = direct ? 24U : 96U;
	int ret;

	if (slot < 0)
		return -EINVAL;
	ret = s7d_h264_mv_slot(mv, slot, direct, &base, &bytes);
	if (ret)
		return ret;
	if (check_mul_overflow(first_mb, record_bytes, &offset))
		return -EOVERFLOW;
	if (offset > bytes || record_bytes > bytes - offset)
		return -ERANGE;
	if (check_add_overflow(base, offset, address))
		return -EOVERFLOW;
	return (*address & 7U) ? -EINVAL : 0;
}

static int s7d_h264_picture_values(const struct s7d_codec_layout *layout,
				  const struct s7d_h264_picture_plan *picture,
				  const struct s7d_h264_mv_buffer *mv,
				  const struct s7d_h264_capture_descriptor *capture,
				  struct s7d_h264_picture_values *values)
{
	struct s7d_h264_mv_layout expected;
	u32 mb_total, used_mv = 0, i, list, current_count = 0;
	u32 unused_address, unused_bytes;
	int ret;

	if (!layout || !picture || !mv || !capture ||
	    !picture->slot_count || picture->slot_count > S7D_CODEC_MAX_CAPTURE_BUFFERS ||
	    picture->slot >= picture->slot_count || picture->type > S7D_H264_SLICE_B ||
	    !layout->coded_width || !layout->coded_height ||
	    (layout->coded_width & 15U) || (layout->coded_height & 15U) ||
	    capture->slot != picture->slot ||
	    capture->anc_offset != DOS_REG(0x0990 + picture->slot) ||
	    capture->anc_value > 0xffffffU ||
	    capture->canvas[0].index != picture->slot * 2U ||
	    capture->canvas[1].index != picture->slot * 2U + 1U)
		return -EINVAL;
	if (capture->anc_value != ((capture->canvas[1].index << 16) |
		(capture->canvas[1].index << 8) | capture->canvas[0].index))
		return -EINVAL;
	if (picture->direct_8x8 != ((picture->mode_8x8_flags & 6U) == 6U))
		return -EOPNOTSUPP;
	if (check_mul_overflow(layout->coded_width / 16U,
			       layout->coded_height / 16U, &mb_total))
		return -EOVERFLOW;
	if (picture->first_mb >= mb_total ||
	    picture->list_count[0] > S7D_H264_REF_LIST_SIZE ||
	    picture->list_count[1] > S7D_H264_REF_LIST_SIZE)
		return -EINVAL;
	if ((picture->type == S7D_H264_SLICE_I &&
	     (picture->list_count[0] || picture->list_count[1])) ||
	    (picture->type == S7D_H264_SLICE_P &&
	     (!picture->list_count[0] || picture->list_count[1])) ||
	    (picture->type == S7D_H264_SLICE_B &&
	     (!picture->list_count[0] || !picture->list_count[1])))
		return -EBADMSG;
	ret = s7d_h264_build_mv_layout(layout, mv->layout.slots, &expected);
	if (ret)
		return ret;
	if (mv->layout.mb_width != expected.mb_width ||
	    mv->layout.mb_height != expected.mb_height ||
	    mv->layout.slot_bytes != expected.slot_bytes ||
	    mv->layout.bytes != expected.bytes || !mv->memory)
		return -EINVAL;
	if ((picture->reference && picture->mv_slot < 0) ||
	    (!picture->reference && picture->mv_slot != -1))
		return -EBADMSG;
	for (i = 0; i < picture->slot_count; i++) {
		const struct s7d_h264_picture_table *entry = &picture->table[i];
		const struct s7d_h264_picture_ref *p = &entry->picture;
		u32 info = 0xf480U, address;
		bool is_current = i == picture->slot;
		bool needs_mv;

		if (!entry->active) {
			if (entry->is_current || entry->reference)
				return -EBADMSG;
			continue;
		}
		if (p->slot != i || p->long_term_index < -1 ||
		    entry->is_current != is_current)
			return -EBADMSG;
		if (entry->is_current) {
			current_count++;
			if (p->frame_num != picture->frame_num || p->poc != picture->poc ||
			    p->top_poc != picture->top_poc ||
			    p->bottom_poc != picture->bottom_poc ||
			    p->mv_slot != picture->mv_slot ||
			    p->direct_8x8 != picture->direct_8x8 ||
			    entry->reference || p->long_term_index != -1)
				return -EBADMSG;
			info |= 0xfU;
		}
		needs_mv = entry->is_current ? picture->reference : entry->reference;
		if (needs_mv) {
			if (p->direct_8x8 != picture->direct_8x8)
				return -EOPNOTSUPP;
			ret = s7d_h264_mv_address(mv, p->mv_slot, p->direct_8x8, 0, &address);
			if (ret)
				return ret;
			if (used_mv & BIT(p->mv_slot))
				return -EBADMSG;
			used_mv |= BIT(p->mv_slot);
		}
		if (p->bottom_poc < p->top_poc)
			info |= BIT(8);
		if (entry->reference && p->long_term_index >= 0)
			info |= BIT(4) | BIT(5);
		values->table[i][0] = info;
		values->table[i][1] = (u32)p->top_poc;
		values->table[i][2] = (u32)p->bottom_poc;
	}
	if (current_count != 1)
		return -EBADMSG;
	for (list = 0; list < 2; list++) {
		u32 words = list ? DIV_ROUND_UP(picture->list_count[1], 4U) : 8U;
		u32 last_cfg = 0;

		for (i = 0; i < picture->list_count[list]; i++) {
			const struct s7d_h264_picture_ref *ref = &picture->list[list][i];
			const struct s7d_h264_picture_table *entry;

			if (ref->slot >= picture->slot_count || ref->slot == picture->slot)
				return -EBADMSG;
			entry = &picture->table[ref->slot];
			if (!entry->active || !entry->reference || entry->is_current ||
			    !s7d_h264_same_ref(ref, &entry->picture))
				return -EBADMSG;
		}
		for (i = 0; i < words * 4U; i++) {
			if (i < picture->list_count[list])
				last_cfg = picture->list[list][i].slot | 0x60U;
			values->refs[list][i / 4U] <<= 8;
			values->refs[list][i / 4U] |= last_cfg;
		}
		if (list)
			values->list1_words = words;
	}
	values->mv_write = U32_MAX;
	if (picture->reference) {
		ret = s7d_h264_mv_address(mv, picture->mv_slot, picture->direct_8x8,
					 picture->first_mb, &values->mv_write);
		if (ret)
			return ret;
	}
	if (picture->list_count[1]) {
		const struct s7d_h264_picture_ref *colocate = &picture->list[1][0];
		s64 top = (s64)picture->poc - colocate->top_poc;
		s64 bottom = (s64)picture->poc - colocate->bottom_poc;
		u32 address;

		ret = s7d_h264_mv_address(mv, colocate->mv_slot, picture->direct_8x8,
					 picture->first_mb, &address);
		if (ret)
			return ret;
		top = top < 0 ? -top : top;
		bottom = bottom < 0 ? -bottom : bottom;
		values->mv_read = (address >> 3) | BIT(31);
		if (top >= bottom)
			values->mv_read |= BIT(29);
		values->read_mv = true;
	}
	/* Validate even a non-reference current picture against the live MV span. */
	return s7d_h264_mv_slot(mv, 0, picture->direct_8x8, &unused_address, &unused_bytes);
}

int s7d_h264_picture_program(struct regmap *map,
			     const struct s7d_codec_layout *layout,
			     const struct s7d_h264_picture_plan *picture,
			     const struct s7d_h264_mv_buffer *mv,
			     const struct s7d_h264_capture_descriptor *capture)
{
	struct s7d_h264_picture_values values = { 0 };
	u32 actual, i, j;
	int ret;

	if (!map)
		return -EINVAL;
	ret = s7d_h264_picture_values(layout, picture, mv, capture, &values);
	if (ret)
		return ret;
	ret = regmap_write(map, H264_POC_INDEX, 0);
	if (ret)
		return ret;
	ret = regmap_write(map, H264_CURRENT_POC, (u32)picture->poc);
	if (ret)
		return ret;
	ret = regmap_write(map, H264_CURRENT_POC, (u32)picture->top_poc);
	if (ret)
		return ret;
	ret = regmap_write(map, H264_CURRENT_POC, (u32)picture->bottom_poc);
	if (ret)
		return ret;
	ret = regmap_write(map, CURR_CANVAS_CTRL, picture->slot << 24);
	if (ret)
		return ret;
	ret = regmap_read(map, CURR_CANVAS_CTRL, &actual);
	if (ret)
		return ret;
	if ((actual & 0xffffffU) != capture->anc_value)
		return -EIO;
	ret = regmap_write(map, REC_CANVAS_ADDR, capture->anc_value);
	if (ret)
		return ret;
	ret = regmap_write(map, DBKR_CANVAS_ADDR, capture->anc_value);
	if (ret)
		return ret;
	ret = regmap_write(map, DBKW_CANVAS_ADDR, capture->anc_value);
	if (ret)
		return ret;
	ret = regmap_write(map, H264_BUFFER_INFO_INDEX, 16);
	if (ret)
		return ret;
	for (i = 0; i < picture->slot_count; i++) {
		for (j = 0; j < 3; j++) {
			ret = regmap_write(map, H264_BUFFER_INFO_DATA, values.table[i][j]);
			if (ret)
				return ret;
		}
	}
	ret = regmap_write(map, H264_BUFFER_INFO_INDEX, 0);
	if (ret)
		return ret;
	for (i = 0; i < 8; i++) {
		ret = regmap_write(map, H264_BUFFER_INFO_DATA, values.refs[0][i]);
		if (ret)
			return ret;
	}
	ret = regmap_write(map, H264_BUFFER_INFO_INDEX, 8);
	if (ret)
		return ret;
	for (i = 0; i < values.list1_words; i++) {
		ret = regmap_write(map, H264_BUFFER_INFO_DATA, values.refs[1][i]);
		if (ret)
			return ret;
	}
	ret = regmap_read_poll_timeout(map, H264_CO_MB_RW_CTL, actual,
				       !(actual & BIT(11)), 1, 10000);
	if (ret)
		return ret;
	ret = regmap_write(map, H264_CO_MB_WR_ADDR, values.mv_write);
	if (ret)
		return ret;
	if (values.read_mv)
		return regmap_write(map, H264_CO_MB_RD_ADDR, values.mv_read);
	return 0;
}
