// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "s7d-codec-dos.h"
#include "s7d-codec-session.h"
#include "s7d-vdec-runtime.h"
#include "s7d-v4l2.h"
#include "s7d-h264-input.h"
#include "s7d-h264-dpb.h"
#include "s7d-h264-protocol.h"

#define DOS_REG(word)			((word) * 4)
#define VDEC_MPSR			DOS_REG(0x0301)
#define VDEC_CPSR			DOS_REG(0x0321)
#define VDEC_MBOX1_MASK			DOS_REG(0x0076)
#define H264_RESERVED_BITS		DOS_REG(0x09c1)
#define H264_CONFIG_SEQINFO		DOS_REG(0x09c1)
#define H264_DECODE_SIZE			DOS_REG(0x09ce)
#define H264_ERROR			DOS_REG(0x09c9)
#define H264_STATUS			DOS_REG(0x09d3)
#define H264_MBY_MBX			DOS_REG(0x0c07)
#define POWER_CTL_VLD			DOS_REG(0x0c08)
#define VIFF_BIT_CNT			DOS_REG(0x0c1a)
#define H264_DECODE_INFO			DOS_REG(0x0c29)
#define VLD_START_PTR			DOS_REG(0x0c40)
#define VLD_CURR_PTR			DOS_REG(0x0c41)
#define VLD_END_PTR			DOS_REG(0x0c42)
#define VLD_CONTROL			DOS_REG(0x0c44)
#define VLD_WP				DOS_REG(0x0c45)
#define VLD_RP				DOS_REG(0x0c46)
#define VLD_BUF_CNTL			DOS_REG(0x0c48)

#define INPUT_ALIGNMENT			64U
#define FIFO_ALIGNMENT			8U
#define FIFO_PADDING			1024U
#define FIFO_ENABLE			(BIT(1) | BIT(2))
#define FIFO_MANUAL_CONTROL		((0x11U << 16) | BIT(10) | (7U << 3))
#define H264_SLICE_HEAD_DONE		1U
#define H264_PIC_DATA_DONE		2U

#define AU_SPS_COUNT 32
#define AU_PPS_COUNT 256
#define AU_MAX_REFS 32
#define AU_MAX_MMCO (AU_MAX_REFS * 2 + 3)
#define AU_MAX_NALS 512
#define AU_MAX_MB_DIM 1055
#define AU_MAX_MBS 139264
#define AU_U32_MAX ((u32)~0U)
#define AU_S32_MAX 2147483647
#define AU_S32_MIN (-2147483647 - 1)

struct au_sps {
	bool valid;
	u32 profile_idc;
	u32 poc_type;
	u32 log2_frame_num;
	u32 log2_poc_lsb;
	u32 coded_width;
	u32 coded_height;
	u32 mb_count;
	u32 max_references;
	struct s7d_codec_colour colour;
	struct s7d_h264_sps sps;
};

struct au_pps {
	bool valid;
	bool cabac;
	bool bottom_poc_present;
	bool weighted_pred;
	bool deblocking_present;
	u32 sps_id;
	u32 ref_count[2];
	u32 weighted_bipred;
	s32 init_qp_minus26;
};

struct au_cache {
	struct au_sps sps[AU_SPS_COUNT];
	struct au_pps pps[AU_PPS_COUNT];
};

struct au_raw {
	u8 *data;
	u32 bytes;
};

/* No session, DMA, firmware address or one-shot prefix belongs to this bank. */
struct s7d_h264_parameters {
	struct au_cache cache;
	struct au_raw sps[AU_SPS_COUNT];
	struct au_raw pps[AU_PPS_COUNT];
	u32 raw_bytes;
	u32 selected_sps;
	u32 selected_pps;
	bool selected_valid;
};

struct au_pending {
	struct au_cache cache;
	/* A non-NULL slot owns a replacement; unchanged slots borrow the bank. */
	struct au_raw sps[AU_SPS_COUNT];
	struct au_raw pps[AU_PPS_COUNT];
	u32 raw_bytes;
	u32 selected_sps;
	u32 selected_pps;
	bool selected_valid;
	struct s7d_h264_au_info info;
	struct s7d_h264_preflight preflight;
	u8 *prefix;
	u32 prefix_bytes;
	u32 prefix_nals;
	u32 aud_end;
};

struct s7d_h264_au_parser {
	struct s7d_h264_parameters *parameters;
	struct au_pending *pending;
	u8 *prefix;
	u32 prefix_bytes;
	u32 prefix_nals;
	bool hunting;
};

struct au_bits {
	const u8 *data;
	u32 bytes;
	u64 bit;
};

struct au_prefix {
	u32 start;
	u32 header;
};

struct au_slice {
	u32 pps_id;
	u32 sps_id;
	u32 first_mb;
	u32 type;
	u32 frame_num;
	u32 idr_pic_id;
	u32 poc_lsb;
	s32 delta_bottom;
	bool idr;
	bool reference;
	bool all_same_type;
};

/* Every reader checks the original RBSP size; it never reads padding. */
static int au_get(struct au_bits *b, u32 n, u32 *value)
{
	u32 v = 0, i;

	if (n > 32 || b->bit > (u64)b->bytes * 8 ||
	    n > (u64)b->bytes * 8 - b->bit)
		return -EBADMSG;
	for (i = 0; i < n; i++, b->bit++)
		v = (v << 1) | ((b->data[b->bit >> 3] >>
				      (7 - (b->bit & 7))) & 1);
	*value = v;
	return 0;
}

static int au_skip(struct au_bits *b, u64 n)
{
	if (b->bit > (u64)b->bytes * 8 ||
	    n > (u64)b->bytes * 8 - b->bit)
		return -EBADMSG;
	b->bit += n;
	return 0;
}

static int au_ue64(struct au_bits *b, u64 max, u64 *value)
{
	u32 zeros = 0, v, suffix;
	u64 decoded;
	int ret;

	for (;;) {
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
		if (v)
			break;
		if (++zeros > 32)
			return -EBADMSG;
	}
	ret = au_get(b, zeros, &suffix);
	if (ret)
		return ret;
	decoded = ((1ULL << zeros) - 1) + suffix;
	if (decoded > max)
		return -EBADMSG;
	*value = decoded;
	return 0;
}

static int au_ue(struct au_bits *b, u32 max, u32 *value)
{
	u64 v;
	int ret = au_ue64(b, max, &v);

	if (!ret)
		*value = v;
	return ret;
}

static int au_se(struct au_bits *b, s32 min, s32 max, s32 *value)
{
	u64 code;
	s64 v;
	int ret = au_ue64(b, (u64)AU_U32_MAX + 1, &code);

	if (ret)
		return ret;
	if (code & 1)
		v = (code + 1) >> 1;
	else
		v = -(s64)(code >> 1);
	if (v < min || v > max)
		return -EBADMSG;
	*value = v;
	return 0;
}

static bool au_more_rbsp(const struct au_bits *b)
{
	struct au_bits tail = *b;
	u64 left = (u64)b->bytes * 8 - b->bit;
	u32 v;

	if (!left)
		return false;
	if (left > 8)
		return true;
	if (au_get(&tail, left, &v))
		return true;
	return v != (1U << (left - 1));
}

static int au_trailing_bits(struct au_bits *b)
{
	u32 v;
	int ret;

	ret = au_get(b, 1, &v);
	if (ret || v != 1)
		return -EBADMSG;
	while (b->bit & 7) {
		ret = au_get(b, 1, &v);
		if (ret || v)
			return -EBADMSG;
	}
	return b->bit == (u64)b->bytes * 8 ? 0 : -EBADMSG;
}

static int au_filler_parse(struct au_bits *b)
{
	u32 v;
	int ret;

	while (au_more_rbsp(b)) {
		ret = au_get(b, 8, &v);
		if (ret || v != 0xff)
			return -EBADMSG;
	}
	return au_trailing_bits(b);
}

static bool au_find_prefix(const u8 *data, u32 bytes, u32 from,
			   struct au_prefix *prefix)
{
	u32 i, start;

	for (i = from; i < bytes; i++) {
		if (i - from < 2 || data[i] != 1 ||
		    data[i - 1] || data[i - 2])
			continue;
		start = i - 2;
		/* A four-byte code is zero_byte plus this three-byte prefix. */
		prefix->start = start;
		prefix->header = i + 1;
		return true;
	}
	return false;
}

/*
 * Reject forbidden unescaped 00 00 {00,01,02} inside the framed EBSP and
 * incomplete/invalid 00 00 03 escapes. NAL headers are excluded here.
 */
static int au_rbsp(const u8 *data, u32 bytes, u8 *rbsp, u32 *rbsp_bytes)
{
	u32 i, out = 0, zeros = 0;
	u8 v;

	for (i = 0; i < bytes; i++) {
		v = data[i];
		if (zeros == 2) {
			if (v == 3) {
				if (i + 1 == bytes || data[i + 1] > 3)
					return -EBADMSG;
				zeros = 0;
				continue;
			}
			if (v <= 2)
				return -EBADMSG;
		}
		rbsp[out++] = v;
		zeros = v ? 0 : zeros + 1;
	}
	*rbsp_bytes = out;
	return 0;
}

static int au_scaling_list(struct au_bits *b, u32 size)
{
	s32 delta;
	u32 i, v, last = 8, next = 8;
	int ret;

	for (i = 0; i < size; i++) {
		if (next) {
			ret = au_se(b, -128, 127, &delta);
			if (ret)
				return ret;
			v = (last + delta + 256) & 255;
			next = v;
		}
		last = next ? next : last;
	}
	return 0;
}

static int au_scaling_matrices(struct au_bits *b, u32 count)
{
	u32 i, present;
	int ret;

	for (i = 0; i < count; i++) {
		ret = au_get(b, 1, &present);
		if (ret)
			return ret;
		if (present) {
			ret = au_scaling_list(b, i < 6 ? 16 : 64);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int au_hrd(struct au_bits *b)
{
	u32 count, i, v;
	int ret;

	ret = au_ue(b, 31, &count);
	if (ret)
		return ret;
	ret = au_skip(b, 8);
	if (ret)
		return ret;
	for (i = 0; i <= count; i++) {
		ret = au_ue(b, AU_U32_MAX, &v);
		if (ret)
			return ret;
		ret = au_ue(b, AU_U32_MAX, &v);
		if (ret)
			return ret;
		ret = au_skip(b, 1);
		if (ret)
			return ret;
	}
	return au_skip(b, 20);
}

static int au_vui(struct au_bits *b, u32 max_refs,
		  struct s7d_h264_sps *sps)
{
	struct s7d_codec_colour *colour = &sps->colour;
	u32 flag, v, units, scale, nal_hrd, vcl_hrd;
	u32 reorder, buffering;
	int ret;

	ret = au_get(b, 1, &flag);
	if (ret)
		return ret;
	if (flag) {
		ret = au_get(b, 8, &v);
		if (ret)
			return ret;
		if (v == 255) {
			ret = au_get(b, 16, &units);
			if (ret)
				return ret;
			ret = au_get(b, 16, &scale);
			if (ret)
				return ret;
			if (!units || !scale)
				return -EBADMSG;
		} else if (v > 16) {
			return -EOPNOTSUPP;
		}
	}
	ret = au_get(b, 1, &flag);
	if (ret)
		return ret;
	if (flag) {
		ret = au_skip(b, 1);
		if (ret)
			return ret;
	}
	ret = au_get(b, 1, &flag);
	if (ret)
		return ret;
	if (flag) {
		colour->flags |= S7D_CODEC_COLOUR_SIGNAL_PRESENT;
		ret = au_get(b, 3, &v);
		if (ret)
			return ret;
		if (v > 5)
			return -EBADMSG;
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
		if (v)
			colour->flags |= S7D_CODEC_COLOUR_FULL_RANGE;
		ret = au_get(b, 1, &flag);
		if (ret)
			return ret;
		if (flag) {
			colour->flags |= S7D_CODEC_COLOUR_DESCRIPTION_PRESENT;
			ret = au_get(b, 8, &v);
			if (ret)
				return ret;
			colour->primaries = v;
			ret = au_get(b, 8, &v);
			if (ret)
				return ret;
			colour->transfer = v;
			ret = au_get(b, 8, &v);
			if (ret)
				return ret;
			colour->matrix = v;
		}
	}
	ret = au_get(b, 1, &flag);
	if (ret)
		return ret;
	if (flag) {
		ret = au_ue(b, 5, &v);
		if (ret)
			return ret;
		ret = au_ue(b, 5, &v);
		if (ret)
			return ret;
	}
	ret = au_get(b, 1, &flag);
	if (ret)
		return ret;
	if (flag) {
		ret = au_get(b, 32, &units);
		if (ret)
			return ret;
		ret = au_get(b, 32, &scale);
		if (ret)
			return ret;
		if (!units || !scale)
			return -EBADMSG;
		ret = au_skip(b, 1);
		if (ret)
			return ret;
	}
	ret = au_get(b, 1, &nal_hrd);
	if (ret)
		return ret;
	if (nal_hrd) {
		ret = au_hrd(b);
		if (ret)
			return ret;
	}
	ret = au_get(b, 1, &vcl_hrd);
	if (ret)
		return ret;
	if (vcl_hrd) {
		ret = au_hrd(b);
		if (ret)
			return ret;
	}
	if (nal_hrd || vcl_hrd) {
		ret = au_skip(b, 1);
		if (ret)
			return ret;
	}
	ret = au_skip(b, 1);
	if (ret)
		return ret;
	ret = au_get(b, 1, &flag);
	if (ret)
		return ret;
	sps->bitstream_restriction = flag;
	if (flag) {
		ret = au_skip(b, 1);
		if (ret)
			return ret;
		ret = au_ue(b, 16, &v);
		if (ret)
			return ret;
		ret = au_ue(b, 16, &v);
		if (ret)
			return ret;
		ret = au_ue(b, 16, &v);
		if (ret)
			return ret;
		ret = au_ue(b, 16, &v);
		if (ret)
			return ret;
		ret = au_ue(b, 16, &reorder);
		if (ret)
			return ret;
		ret = au_ue(b, 16, &buffering);
		if (ret)
			return ret;
		if (reorder > buffering || max_refs > buffering)
			return -EBADMSG;
		sps->num_reorder_frames = reorder;
		sps->max_dec_frame_buffering = buffering;
	}
	return 0;
}

static int au_sps_parse(struct au_bits *b, struct au_cache *cache, u32 *sps_id)
{
	struct au_sps sps = { 0 };
	struct s7d_codec_layout layout;
	u32 profile, constraints, level, id, v, refs, width, height;
	u32 crop[4], i;
	int ret;

	ret = au_get(b, 8, &profile);
	if (ret)
		return ret;
	ret = au_get(b, 8, &constraints);
	if (ret)
		return ret;
	ret = au_get(b, 8, &level);
	if (ret)
		return ret;
	if ((constraints & 3) || !level)
		return -EBADMSG;
	ret = au_ue(b, AU_SPS_COUNT - 1, &id);
	if (ret)
		return ret;
	sps.profile_idc = profile;
	sps.sps.profile_idc = profile;
	sps.sps.level_idc = level;
	sps.sps.constraint_set3 = !!(constraints & BIT(4));
	sps.sps.chroma_format_idc = 1;
	sps.sps.bit_depth_luma = 8;
	sps.sps.bit_depth_chroma = 8;
	if ((profile == 66 || profile == 77) && level == 11)
		return -EOPNOTSUPP;
	if (profile == 100) {
		ret = au_ue(b, 3, &v);
		if (ret)
			return ret;
		if (v != 1)
			return -EOPNOTSUPP;
		ret = au_ue(b, 6, &v);
		if (ret)
			return ret;
		if (v)
			return -EOPNOTSUPP;
		ret = au_ue(b, 6, &v);
		if (ret)
			return ret;
		if (v)
			return -EOPNOTSUPP;
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
		if (v)
			return -EOPNOTSUPP;
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
		if (v) {
			ret = au_scaling_matrices(b, 8);
			if (ret)
				return ret;
		}
	} else if (profile != 66 && profile != 77) {
		return -EOPNOTSUPP;
	}
	ret = au_ue(b, 12, &v);
	if (ret)
		return ret;
	sps.log2_frame_num = v + 4;
	ret = au_ue(b, 2, &sps.poc_type);
	if (ret)
		return ret;
	if (sps.poc_type == 1)
		return -EOPNOTSUPP;
	if (!sps.poc_type) {
		ret = au_ue(b, 12, &v);
		if (ret)
			return ret;
		sps.log2_poc_lsb = v + 4;
	}
	ret = au_ue(b, 16, &refs);
	if (ret)
		return ret;
	sps.max_references = refs;
	sps.sps.max_num_ref_frames = refs;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	ret = au_ue(b, AU_MAX_MB_DIM - 1, &width);
	if (ret)
		return ret;
	ret = au_ue(b, AU_MAX_MB_DIM - 1, &height);
	if (ret)
		return ret;
	width++;
	height++;
	if ((u64)width * height > AU_MAX_MBS)
		return -EOPNOTSUPP;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	if (!v)
		return -EOPNOTSUPP;
	sps.coded_width = width * 16;
	sps.coded_height = height * 16;
	sps.mb_count = width * height;
	sps.sps.mb_width = width;
	sps.sps.mb_height = height;
	sps.sps.frame_mbs_only = true;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	if (v) {
		for (i = 0; i < 4; i++) {
			ret = au_ue(b, AU_MAX_MB_DIM * 8, &crop[i]);
			if (ret)
				return ret;
		}
		if ((u64)(crop[0] + crop[1]) * 2 >= sps.coded_width ||
		    (u64)(crop[2] + crop[3]) * 2 >= sps.coded_height)
			return -EBADMSG;
		sps.sps.crop_left = crop[0];
		sps.sps.crop_right = crop[1];
		sps.sps.crop_top = crop[2];
		sps.sps.crop_bottom = crop[3];
	}
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	if (v) {
		ret = au_vui(b, refs, &sps.sps);
		if (ret)
			return ret;
	}
	ret = au_trailing_bits(b);
	if (ret)
		return ret;
	ret = s7d_h264_build_layout(&sps.sps, S7D_CODEC_CAPTURE_NV12M, 0, &layout);
	if (ret)
		return ret;
	sps.colour = sps.sps.colour;
	sps.valid = true;
	cache->sps[id] = sps;
	*sps_id = id;
	return 0;
}

static int au_pps_parse(struct au_bits *b, struct au_cache *cache, u32 *pps_id)
{
	struct au_pps pps = { 0 };
	u32 id, v, transform;
	s32 signed_v, chroma_qp;
	int ret;

	ret = au_ue(b, AU_PPS_COUNT - 1, &id);
	if (ret)
		return ret;
	ret = au_ue(b, AU_SPS_COUNT - 1, &pps.sps_id);
	if (ret)
		return ret;
	if (!cache->sps[pps.sps_id].valid)
		return -ENOENT;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	pps.cabac = v;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	pps.bottom_poc_present = v;
	ret = au_ue(b, 7, &v);
	if (ret)
		return ret;
	if (v)
		return -EOPNOTSUPP;
	ret = au_ue(b, AU_MAX_REFS - 1, &v);
	if (ret)
		return ret;
	pps.ref_count[0] = v + 1;
	ret = au_ue(b, AU_MAX_REFS - 1, &v);
	if (ret)
		return ret;
	pps.ref_count[1] = v + 1;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	pps.weighted_pred = v;
	ret = au_get(b, 2, &pps.weighted_bipred);
	if (ret)
		return ret;
	if (pps.weighted_bipred > 2)
		return -EBADMSG;
	if ((cache->sps[pps.sps_id].profile_idc == 66 ||
	     cache->sps[pps.sps_id].profile_idc == 88) && pps.cabac)
		return -EBADMSG;
	if (cache->sps[pps.sps_id].profile_idc == 66 &&
	    (pps.weighted_pred || pps.weighted_bipred))
		return -EBADMSG;
	ret = au_se(b, -26, 25, &pps.init_qp_minus26);
	if (ret)
		return ret;
	ret = au_se(b, -26, 25, &signed_v);
	if (ret)
		return ret;
	ret = au_se(b, -12, 12, &chroma_qp);
	if (ret)
		return ret;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	pps.deblocking_present = v;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	ret = au_get(b, 1, &v);
	if (ret)
		return ret;
	if (v)
		return -EOPNOTSUPP;
	if (au_more_rbsp(b)) {
		ret = au_get(b, 1, &transform);
		if (ret)
			return ret;
		if (transform && cache->sps[pps.sps_id].profile_idc != 100)
			return -EBADMSG;
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
		if (v && cache->sps[pps.sps_id].profile_idc != 100)
			return -EBADMSG;
		if (v) {
			ret = au_scaling_matrices(b, 6 + transform * 2);
			if (ret)
				return ret;
		}
		ret = au_se(b, -12, 12, &signed_v);
		if (ret)
			return ret;
		if (cache->sps[pps.sps_id].profile_idc != 100 &&
		    signed_v != chroma_qp)
			return -EBADMSG;
	}
	ret = au_trailing_bits(b);
	if (ret)
		return ret;
	pps.valid = true;
	cache->pps[id] = pps;
	*pps_id = id;
	return 0;
}

/* Payload content is opaque; only the byte-aligned SEI envelope is checked. */
static int au_sei_parse(struct au_bits *b)
{
	u32 v, payload_type, payload_bytes;
	bool seen = false;
	int ret;

	while (au_more_rbsp(b)) {
		payload_type = 0;
		do {
			ret = au_get(b, 8, &v);
			if (ret || payload_type > AU_U32_MAX - v)
				return -EBADMSG;
			payload_type += v;
		} while (v == 255);
		payload_bytes = 0;
		do {
			ret = au_get(b, 8, &v);
			if (ret || payload_bytes > AU_U32_MAX - v)
				return -EBADMSG;
			payload_bytes += v;
		} while (v == 255);
		ret = au_skip(b, (u64)payload_bytes * 8);
		if (ret)
			return ret;
		seen = true;
	}
	if (!seen)
		return -EBADMSG;
	return au_trailing_bits(b);
}

static int au_ref_modification(struct au_bits *b, u32 count, u32 max_pic_num)
{
	u32 i, op, v;
	int ret;

	for (i = 0; i <= count; i++) {
		ret = au_ue(b, 3, &op);
		if (ret || op == 3)
			return ret;
		if (i == count)
			return -EBADMSG;
		ret = au_ue(b, op < 2 ? max_pic_num - 1 : AU_U32_MAX, &v);
		if (ret)
			return ret;
	}
	return -EBADMSG;
}

static int au_pred_weights(struct au_bits *b, u32 l0, u32 l1)
{
	u32 i, list, count, flag, v, j;
	s32 weight;
	int ret;

	ret = au_ue(b, 7, &v);
	if (ret)
		return ret;
	ret = au_ue(b, 7, &v);
	if (ret)
		return ret;
	for (list = 0; list < 2; list++) {
		count = list ? l1 : l0;
		for (i = 0; i < count; i++) {
			ret = au_get(b, 1, &flag);
			if (ret)
				return ret;
			if (flag) {
				ret = au_se(b, -128, 127, &weight);
				if (ret)
					return ret;
				ret = au_se(b, -128, 127, &weight);
				if (ret)
					return ret;
			}
			ret = au_get(b, 1, &flag);
			if (ret)
				return ret;
			if (!flag)
				continue;
			for (j = 0; j < 2; j++) {
				ret = au_se(b, -128, 127, &weight);
				if (ret)
					return ret;
				ret = au_se(b, -128, 127, &weight);
				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

static int au_marking(struct au_bits *b, bool idr, u32 max_pic_num)
{
	u32 flag, i, op, v;
	int ret;

	if (idr)
		return au_skip(b, 2);
	ret = au_get(b, 1, &flag);
	if (ret || !flag)
		return ret;
	for (i = 0; i < AU_MAX_MMCO; i++) {
		ret = au_ue(b, 6, &op);
		if (ret || !op)
			return ret;
		if (op == 1 || op == 3) {
			ret = au_ue(b, max_pic_num - 1, &v);
			if (ret)
				return ret;
		}
		if (op == 2) {
			ret = au_ue(b, AU_U32_MAX, &v);
			if (ret)
				return ret;
		}
		if (op == 3 || op == 6) {
			ret = au_ue(b, AU_U32_MAX, &v);
			if (ret)
				return ret;
		}
		if (op == 4) {
			ret = au_ue(b, AU_U32_MAX, &v);
			if (ret)
				return ret;
		}
	}
	return -EOPNOTSUPP;
}

static int au_slice_parse(struct au_bits *b, const struct au_cache *cache,
			  u32 nal_type, u32 ref_idc, struct au_slice *slice)
{
	const struct au_sps *sps;
	const struct au_pps *pps;
	u32 v, refs[2], max_pic_num;
	s32 delta;
	int ret;

	ret = au_ue(b, AU_MAX_MBS - 1, &slice->first_mb);
	if (ret)
		return ret;
	ret = au_ue(b, 9, &v);
	if (ret)
		return ret;
	slice->type = v % 5;
	slice->all_same_type = v >= 5;
	if (slice->type > 2)
		return -EOPNOTSUPP;
	ret = au_ue(b, AU_PPS_COUNT - 1, &slice->pps_id);
	if (ret)
		return ret;
	pps = &cache->pps[slice->pps_id];
	if (!pps->valid)
		return -ENOENT;
	slice->sps_id = pps->sps_id;
	sps = &cache->sps[slice->sps_id];
	if (!sps->valid)
		return -ENOENT;
	if (sps->profile_idc == 66 && slice->type == 1)
		return -EBADMSG;
	if (slice->first_mb >= sps->mb_count)
		return -EBADMSG;
	slice->idr = nal_type == 5;
	slice->reference = ref_idc != 0;
	if (slice->idr && (slice->type != 2 || !slice->reference))
		return -EBADMSG;
	ret = au_get(b, sps->log2_frame_num, &slice->frame_num);
	if (ret)
		return ret;
	if (slice->idr) {
		if (slice->frame_num)
			return -EBADMSG;
		ret = au_ue(b, 65535, &slice->idr_pic_id);
		if (ret)
			return ret;
	}
	if (!sps->poc_type) {
		ret = au_get(b, sps->log2_poc_lsb, &slice->poc_lsb);
		if (ret)
			return ret;
		if (pps->bottom_poc_present) {
			ret = au_se(b, AU_S32_MIN, AU_S32_MAX, &slice->delta_bottom);
			if (ret)
				return ret;
		}
	}
	refs[0] = pps->ref_count[0];
	refs[1] = pps->ref_count[1];
	if (slice->type == 1) {
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
	}
	if (slice->type < 2) {
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
		if (v) {
			ret = au_ue(b, AU_MAX_REFS - 1, &v);
			if (ret)
				return ret;
			refs[0] = v + 1;
			if (slice->type == 1) {
				ret = au_ue(b, AU_MAX_REFS - 1, &v);
				if (ret)
					return ret;
				refs[1] = v + 1;
			}
		}
	}
	max_pic_num = 1U << sps->log2_frame_num;
	if (slice->type < 2) {
		ret = au_get(b, 1, &v);
		if (ret)
			return ret;
		if (v) {
			ret = au_ref_modification(b, refs[0], max_pic_num);
			if (ret)
				return ret;
		}
		if (slice->type == 1) {
			ret = au_get(b, 1, &v);
			if (ret)
				return ret;
			if (v) {
				ret = au_ref_modification(b, refs[1], max_pic_num);
				if (ret)
					return ret;
			}
		}
	}
	if ((pps->weighted_pred && slice->type == 0) ||
	    (pps->weighted_bipred == 1 && slice->type == 1)) {
		ret = au_pred_weights(b, refs[0], slice->type == 1 ? refs[1] : 0);
		if (ret)
			return ret;
	}
	if (slice->reference) {
		ret = au_marking(b, slice->idr, max_pic_num);
		if (ret)
			return ret;
	}
	if (pps->cabac && slice->type != 2) {
		ret = au_ue(b, 2, &v);
		if (ret)
			return ret;
	}
	ret = au_se(b, -51, 51, &delta);
	if (ret)
		return ret;
	if (26 + pps->init_qp_minus26 + delta < 0 ||
	    26 + pps->init_qp_minus26 + delta > 51)
		return -EBADMSG;
	if (pps->deblocking_present) {
		ret = au_ue(b, 2, &v);
		if (ret)
			return ret;
		if (v != 1) {
			ret = au_se(b, -6, 6, &delta);
			if (ret)
				return ret;
			ret = au_se(b, -6, 6, &delta);
			if (ret)
				return ret;
		}
	}
	if (pps->cabac) {
		while (b->bit & 7) {
			ret = au_get(b, 1, &v);
			if (ret)
				return ret;
			if (!v)
				return -EBADMSG;
		}
	}
	/* At least one bit remains, but its entropy syntax is not inspected. */
	return b->bit < (u64)b->bytes * 8 ? 0 : -EBADMSG;
}

static bool au_same_picture(const struct s7d_h264_au_info *info,
			    const struct au_slice *slice)
{
	return info->pps_id == slice->pps_id &&
	       info->frame_num == slice->frame_num &&
	       info->idr == slice->idr &&
	       (!slice->idr || info->idr_pic_id == slice->idr_pic_id) &&
	       info->reference == slice->reference &&
	       (info->poc_type == 2 ||
		(info->poc_lsb == slice->poc_lsb &&
		 info->delta_bottom == slice->delta_bottom));
}

static void au_info_first(struct s7d_h264_au_info *info,
			  const struct au_cache *cache,
			  const struct au_slice *slice)
{
	const struct au_sps *sps = &cache->sps[slice->sps_id];

	info->pps_id = slice->pps_id;
	info->sps_id = slice->sps_id;
	info->frame_num = slice->frame_num;
	info->idr_pic_id = slice->idr_pic_id;
	info->poc_type = sps->poc_type;
	info->log2_frame_num = sps->log2_frame_num;
	info->log2_poc_lsb = sps->log2_poc_lsb;
	info->poc_lsb = slice->poc_lsb;
	info->coded_width = sps->coded_width;
	info->coded_height = sps->coded_height;
	info->max_references = sps->max_references;
	info->colour = sps->colour;
	info->sps = sps->sps;
	info->delta_bottom = slice->delta_bottom;
	info->idr = slice->idr;
	info->reference = slice->reference;
}

struct s7d_h264_parameters *s7d_h264_parameters_create(void)
{
	struct s7d_h264_parameters *parameters;

	parameters = kvzalloc(sizeof(*parameters), GFP_KERNEL);
	return parameters ? parameters : ERR_PTR(-ENOMEM);
}

void s7d_h264_parameters_clear(struct s7d_h264_parameters *parameters)
{
	u32 i;

	if (IS_ERR_OR_NULL(parameters))
		return;
	for (i = 0; i < AU_SPS_COUNT; i++)
		kvfree(parameters->sps[i].data);
	for (i = 0; i < AU_PPS_COUNT; i++)
		kvfree(parameters->pps[i].data);
	memset(parameters, 0, sizeof(*parameters));
}

void s7d_h264_parameters_destroy(struct s7d_h264_parameters *parameters)
{
	if (IS_ERR_OR_NULL(parameters))
		return;
	s7d_h264_parameters_clear(parameters);
	kvfree(parameters);
}

static struct s7d_h264_au_parser *
s7d_h264_au_create(struct s7d_h264_parameters *parameters, bool hunting)
{
	struct s7d_h264_au_parser *parser;

	if (IS_ERR_OR_NULL(parameters))
		return ERR_PTR(-EINVAL);
	parser = kzalloc(sizeof(*parser), GFP_KERNEL);
	if (!parser)
		return ERR_PTR(-ENOMEM);
	parser->parameters = parameters;
	parser->hunting = hunting;
	return parser;
}

static void au_pending_free(struct au_pending *pending)
{
	u32 i;

	if (!pending)
		return;
	for (i = 0; i < AU_SPS_COUNT; i++)
		kvfree(pending->sps[i].data);
	for (i = 0; i < AU_PPS_COUNT; i++)
		kvfree(pending->pps[i].data);
	kvfree(pending->prefix);
	kvfree(pending);
}

static void s7d_h264_au_discard(struct s7d_h264_au_parser *parser)
{
	if (IS_ERR_OR_NULL(parser))
		return;
	au_pending_free(parser->pending);
	parser->pending = NULL;
}

static void s7d_h264_au_destroy(struct s7d_h264_au_parser *parser)
{
	if (IS_ERR_OR_NULL(parser))
		return;
	s7d_h264_au_discard(parser);
	kvfree(parser->prefix);
	kfree(parser);
}

static int s7d_h264_au_commit(struct s7d_h264_au_parser *parser)
{
	struct s7d_h264_parameters *parameters;
	struct au_pending *pending;
	u32 i;

	if (IS_ERR_OR_NULL(parser))
		return -EINVAL;
	if (!parser->pending)
		return -ENODATA;
	parameters = parser->parameters;
	pending = parser->pending;
	/* All allocations and validation precede this serialized, infallible swap. */
	for (i = 0; i < AU_SPS_COUNT; i++) {
		if (!pending->sps[i].data)
			continue;
		kvfree(parameters->sps[i].data);
		parameters->sps[i] = pending->sps[i];
		pending->sps[i].data = NULL;
	}
	for (i = 0; i < AU_PPS_COUNT; i++) {
		if (!pending->pps[i].data)
			continue;
		kvfree(parameters->pps[i].data);
		parameters->pps[i] = pending->pps[i];
		pending->pps[i].data = NULL;
	}
	parameters->cache = pending->cache;
	parameters->raw_bytes = pending->raw_bytes;
	parameters->selected_sps = pending->selected_sps;
	parameters->selected_pps = pending->selected_pps;
	parameters->selected_valid = pending->selected_valid;
	kvfree(parser->prefix);
	parser->prefix = parser->pending->prefix;
	parser->prefix_bytes = parser->pending->prefix_bytes;
	parser->prefix_nals = parser->pending->prefix_nals;
	parser->pending->prefix = NULL;
	s7d_h264_au_discard(parser);
	return 0;
}

static void au_pending_init(struct s7d_h264_au_parser *parser,
			    struct au_pending *draft)
{
	const struct s7d_h264_parameters *parameters = parser->parameters;

	draft->cache = parameters->cache;
	draft->raw_bytes = parameters->raw_bytes;
	draft->selected_sps = parameters->selected_sps;
	draft->selected_pps = parameters->selected_pps;
	draft->selected_valid = parameters->selected_valid;
}

static const struct au_raw *au_raw_get(struct s7d_h264_au_parser *parser,
				     struct au_pending *draft,
				     bool sps, u32 id)
{
	const struct au_raw *raw = sps ? &draft->sps[id] : &draft->pps[id];

	if (raw->data)
		return raw;
	return sps ? &parser->parameters->sps[id] : &parser->parameters->pps[id];
}

static int au_raw_replace(struct s7d_h264_au_parser *parser,
			  struct au_pending *draft, bool sps, u32 id,
			  const u8 *nal, u32 nal_bytes)
{
	const struct au_raw *previous = au_raw_get(parser, draft, sps, id);
	struct au_raw *raw = sps ? &draft->sps[id] : &draft->pps[id];
	u32 bytes, total;
	u8 *data;

	if (check_add_overflow(nal_bytes, 4U, &bytes) ||
	    previous->bytes > draft->raw_bytes ||
	    check_add_overflow(draft->raw_bytes - previous->bytes, bytes, &total))
		return -EOVERFLOW;
	if (total > S7D_H264_PARAMETERS_MAX_BYTES)
		return -E2BIG;
	data = kvmalloc(bytes, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	/* Canonical Annex-B framing; the accepted NAL header and EBSP stay exact. */
	data[0] = 0;
	data[1] = 0;
	data[2] = 0;
	data[3] = 1;
	memcpy(data + 4, nal, nal_bytes);
	kvfree(raw->data);
	raw->data = data;
	raw->bytes = bytes;
	draft->raw_bytes = total;
	return 0;
}

/* A retained PPS is interpreted against the latest definition of its SPS. */
static int au_replay_pps_check(struct au_pending *draft, const struct au_raw *raw,
			      u32 pps_id, u32 sps_id)
{
	struct au_pps saved = draft->cache.pps[pps_id];
	struct au_pps *checked;
	struct au_bits bits;
	u32 bytes, id;
	u8 *rbsp;
	int ret;

	if (!raw->data || raw->bytes <= 5 || raw->data[0] || raw->data[1] ||
	    raw->data[2] || raw->data[3] != 1 || (raw->data[4] & 31) != 8)
		return -ENOENT;
	rbsp = kvmalloc(raw->bytes - 5, GFP_KERNEL);
	if (!rbsp)
		return -ENOMEM;
	ret = au_rbsp(raw->data + 5, raw->bytes - 5, rbsp, &bytes);
	if (ret)
		goto out;
	bits.data = rbsp;
	bits.bytes = bytes;
	bits.bit = 0;
	ret = au_pps_parse(&bits, &draft->cache, &id);
	if (ret)
		goto out;
	checked = &draft->cache.pps[pps_id];
	if (id != pps_id || checked->sps_id != sps_id ||
	    checked->cabac != saved.cabac ||
	    checked->bottom_poc_present != saved.bottom_poc_present ||
	    checked->weighted_pred != saved.weighted_pred ||
	    checked->deblocking_present != saved.deblocking_present ||
	    checked->ref_count[0] != saved.ref_count[0] ||
	    checked->ref_count[1] != saved.ref_count[1] ||
	    checked->weighted_bipred != saved.weighted_bipred ||
	    checked->init_qp_minus26 != saved.init_qp_minus26)
		ret = -EBADMSG;
out:
	kvfree(rbsp);
	return ret;
}

static int au_headers_prefix(struct s7d_h264_au_parser *parser,
			     struct au_pending *draft, const u8 *data,
			     u32 raw_bytes, u32 nals)
{
	struct au_prefix prefix, next;
	u32 bytes = raw_bytes - draft->aud_end, end, at;
	bool has_next;

	/* HUNT replays current SPS/PPS from the accepted bank. */
	if (parser->hunting) {
		bytes = 0;
		nals = 0;
		if (!au_find_prefix(data, raw_bytes, 0, &prefix))
			return -EBADMSG;
		for (;;) {
			has_next = au_find_prefix(data, raw_bytes,
						prefix.header + 1, &next);
			end = has_next ? next.start : raw_bytes;
			if ((data[prefix.header] & 31) == 6) {
				if (check_add_overflow(bytes, end - prefix.start, &bytes))
					return -EOVERFLOW;
				nals++;
			}
			if (!has_next)
				break;
			prefix = next;
		}
	}

	if (check_add_overflow(parser->prefix_bytes, bytes, &draft->prefix_bytes) ||
	    draft->prefix_bytes > S7D_H264_PREFIX_MAX_BYTES ||
	    check_add_overflow(parser->prefix_nals, nals, &draft->prefix_nals) ||
	    draft->prefix_nals > AU_MAX_NALS)
		return -E2BIG;
	if (!draft->prefix_bytes)
		return 0;
	draft->prefix = kvmalloc(draft->prefix_bytes, GFP_KERNEL);
	if (!draft->prefix)
		return -ENOMEM;
	if (parser->prefix_bytes)
		memcpy(draft->prefix, parser->prefix, parser->prefix_bytes);
	if (!parser->hunting) {
		if (bytes)
			memcpy(draft->prefix + parser->prefix_bytes,
			       data + draft->aud_end, bytes);
		return 0;
	}
	at = parser->prefix_bytes;
	if (!au_find_prefix(data, raw_bytes, 0, &prefix))
		return -EBADMSG;
	for (;;) {
		has_next = au_find_prefix(data, raw_bytes, prefix.header + 1, &next);
		end = has_next ? next.start : raw_bytes;
		if ((data[prefix.header] & 31) == 6) {
			memcpy(draft->prefix + at, data + prefix.start, end - prefix.start);
			at += end - prefix.start;
		}
		if (!has_next)
			break;
		prefix = next;
	}
	return 0;
}

/* Caller serializes this sleeping operation and owns immutable input bytes. */
static int s7d_h264_au_validate(struct s7d_h264_au_parser *parser,
			      const u8 *data, u32 raw_bytes,
			      struct s7d_h264_au_info *info,
			      struct s7d_h264_preflight *preflight)
{
	struct au_pending *draft;
	struct au_prefix prefix, next;
	struct au_bits bits;
	struct au_slice slice;
	u8 *rbsp;
	u32 raw_end, nonzero_end, payload_end, rbsp_bytes;
	u32 type, ref_idc, v, i, j, sps_id, pps_id, prev_mb = 0, first_type = 0;
	u32 nal_count = 0;
	u32 aud_mask = 7;
	static const u8 aud_masks[8] = { 4, 5, 7, 0, 0, 4, 5, 7 };
	bool has_next, seen_vcl = false, all_same_type = false;
	bool seen_nal = false, mixed_types = false, seen_aud = false;
	bool seen_filler = false;
	int ret;

	if (IS_ERR_OR_NULL(parser) || !data || !info || !preflight)
		return -EINVAL;
	if (parser->pending)
		return -EBUSY;
	if (!raw_bytes)
		return -ENODATA;
	if (!au_find_prefix(data, raw_bytes, 0, &prefix))
		return -EBADMSG;
	for (i = 0; i < prefix.start; i++)
		if (data[i])
			return -EBADMSG;
	draft = kvzalloc(sizeof(*draft), GFP_KERNEL);
	if (!draft)
		return -ENOMEM;
	au_pending_init(parser, draft);
	draft->preflight.picture_bytes = raw_bytes;
	rbsp = kvmalloc(raw_bytes, GFP_KERNEL);
	if (!rbsp) {
		ret = -ENOMEM;
		goto free_draft;
	}
	for (;;) {
		if (++nal_count > AU_MAX_NALS) {
			ret = parser->hunting ? -E2BIG : -EOPNOTSUPP;
			goto free_all;
		}
		if (prefix.header >= raw_bytes) {
			ret = -EBADMSG;
			goto free_all;
		}
		type = data[prefix.header] & 31;
		ref_idc = (data[prefix.header] >> 5) & 3;
		if (data[prefix.header] & 0x80) {
			ret = -EBADMSG;
			goto free_all;
		}
		if ((seen_filler && type != 12) ||
		    (seen_vcl && type != 1 && type != 5 && type != 12)) {
			ret = -EOPNOTSUPP;
			goto free_all;
		}
		if ((type == 5 || type == 7 || type == 8) && !ref_idc) {
			ret = -EBADMSG;
			goto free_all;
		}
		if ((type == 6 || type == 9 || type == 10 ||
		     type == 11 || type == 12) && ref_idc) {
			ret = -EBADMSG;
			goto free_all;
		}
		has_next = au_find_prefix(data, raw_bytes, prefix.header + 1, &next);
		raw_end = has_next ? next.start : raw_bytes;
		nonzero_end = raw_end;
		while (nonzero_end > prefix.header + 1 && !data[nonzero_end - 1])
			nonzero_end--;
		payload_end = nonzero_end;
		/* Preserve the zero byte required after a terminal escape. */
		if (payload_end >= prefix.header + 4 && payload_end < raw_end &&
		    !data[payload_end - 3] && !data[payload_end - 2] &&
		    data[payload_end - 1] == 3)
			payload_end++;
		ret = au_rbsp(data + prefix.header + 1,
			      payload_end - prefix.header - 1, rbsp, &rbsp_bytes);
		if (ret)
			goto free_all;
		bits.data = rbsp;
		bits.bytes = rbsp_bytes;
		bits.bit = 0;
		switch (type) {
		case 7:
			ret = au_sps_parse(&bits, &draft->cache, &sps_id);
			if (!ret)
				ret = au_raw_replace(parser, draft, true, sps_id,
						     data + prefix.header,
						     payload_end - prefix.header);
			if (parser->hunting && (ret == -EINVAL || ret == -ERANGE))
				ret = -EBADMSG;
			if (!ret && !parser->hunting && !draft->preflight.sequence_ready) {
				draft->preflight.sps = draft->cache.sps[sps_id].sps;
				draft->preflight.sequence_ready = true;
			}
			break;
		case 8:
			ret = au_pps_parse(&bits, &draft->cache, &pps_id);
			if (!ret)
				ret = au_raw_replace(parser, draft, false, pps_id,
						     data + prefix.header,
						     payload_end - prefix.header);
			break;
		case 9:
			if (seen_nal) {
				ret = -EBADMSG;
				break;
			}
			ret = au_get(&bits, 3, &v);
			if (!ret) {
				aud_mask = aud_masks[v];
				ret = au_trailing_bits(&bits);
			}
			if (!ret) {
				draft->aud_end = has_next ? nonzero_end : raw_end;
				seen_aud = true;
			}
			break;
		case 6:
			ret = au_sei_parse(&bits);
			break;
		case 12:
			if (!seen_vcl) {
				ret = -EOPNOTSUPP;
				break;
			}
			ret = au_filler_parse(&bits);
			if (!ret && !seen_filler) {
				draft->preflight.picture_bytes = prefix.start;
				seen_filler = true;
			}
			break;
		case 1:
		case 5:
			memset(&slice, 0, sizeof(slice));
			ret = au_slice_parse(&bits, &draft->cache, type, ref_idc, &slice);
			if (ret)
				break;
			if (!(aud_mask & (1U << slice.type))) {
				ret = -EBADMSG;
				break;
			}
			if (!seen_vcl) {
				if (slice.first_mb) {
					ret = -EOPNOTSUPP;
					break;
				}
				au_info_first(&draft->info, &draft->cache, &slice);
				first_type = slice.type;
			} else if (!au_same_picture(&draft->info, &slice)) {
				ret = slice.first_mb || parser->hunting ? -EBADMSG : -E2BIG;
				break;
			} else if (slice.first_mb <= prev_mb) {
				ret = -EOPNOTSUPP;
				break;
			}
			mixed_types |= slice.type != first_type;
			if ((all_same_type || slice.all_same_type) && mixed_types) {
				ret = -EBADMSG;
				break;
			}
			if (draft->info.slice_count == S7D_H264_AU_MAX_SLICES) {
				ret = parser->hunting ? -E2BIG : -EOPNOTSUPP;
				break;
			}
			i = draft->info.slice_count++;
			draft->info.first_mb[i] = slice.first_mb;
			draft->info.slice_type[i] = slice.type;
			draft->info.last_vcl_end = nonzero_end;
			draft->info.cavlc_tail_bits =
				!draft->cache.pps[slice.pps_id].cabac &&
				nonzero_end == payload_end ? __ffs(data[nonzero_end - 1]) + 1 : 0;
			prev_mb = slice.first_mb;
			all_same_type |= slice.all_same_type;
			seen_vcl = true;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		if (ret)
			goto free_all;
		seen_nal = true;
		if (!has_next)
			break;
		prefix = next;
	}
	/* Check final definitions, allowing a same-AU PPS replacement to repair it. */
	for (i = 0; i < AU_SPS_COUNT; i++) {
		if (!draft->sps[i].data)
			continue;
		for (j = 0; j < AU_PPS_COUNT; j++) {
			if (!draft->cache.pps[j].valid || draft->cache.pps[j].sps_id != i)
				continue;
			ret = au_replay_pps_check(draft,
						 au_raw_get(parser, draft, false, j), j, i);
			if (ret)
				goto free_all;
		}
	}
	if (seen_vcl) {
		draft->selected_sps = draft->info.sps_id;
		draft->selected_pps = draft->info.pps_id;
		draft->selected_valid = true;
		if (parser->hunting && !draft->info.idr) {
			draft->preflight.kind = S7D_H264_INPUT_SKIP;
		} else {
			draft->preflight.kind = S7D_H264_INPUT_PICTURE;
			draft->preflight.sequence_ready = true;
			draft->preflight.sps = draft->info.sps;
		}
	} else {
		draft->preflight.kind = S7D_H264_INPUT_HEADERS;
		ret = au_headers_prefix(parser, draft, data, raw_bytes,
					 nal_count - seen_aud);
		if (ret)
			goto free_all;
	}
	/* Success exposes pending only; active remains hardware-uncommitted. */
	*info = draft->info;
	*preflight = draft->preflight;
	parser->pending = draft;
	kvfree(rbsp);
	return 0;

free_all:
	kvfree(rbsp);
free_draft:
	au_pending_free(draft);
	return ret;
}

/* Unknown or malformed source never promotes a partially parsed definition. */
static int s7d_h264_au_skip(struct s7d_h264_au_parser *parser, int status,
			  struct s7d_h264_au_info *info,
			  struct s7d_h264_preflight *preflight)
{
	struct au_pending *draft;

	if (!parser->hunting || parser->pending || status > 0)
		return -EINVAL;
	draft = kvzalloc(sizeof(*draft), GFP_KERNEL);
	if (!draft)
		return -ENOMEM;
	au_pending_init(parser, draft);
	draft->preflight.kind = S7D_H264_INPUT_SKIP;
	draft->preflight.skip_status = status;
	*info = draft->info;
	*preflight = draft->preflight;
	parser->pending = draft;
	return 0;
}

struct s7d_h264_input {
	struct regmap *map;
	struct s7d_codec_session *session;
	struct s7d_v4l2_input buffer;
	struct s7d_h264_input_plan plan;
	struct s7d_h264_au_parser *au_parser;
	struct s7d_h264_au_info au;
	struct s7d_h264_input_observation picture;
	u64 last_token;
	u64 last_consumed_token;
	u64 last_completed_token;
	u64 last_completed_write;
	u64 last_park_epoch;
	u64 write_token;
	struct s7d_v4l2_drain drain;
	u64 last_drain_epoch;
	u32 confirmed_slices;
	u32 sequence_info;
	bool preflight_ready;
	bool hw_prepared;
	bool active;
	bool configured;
	bool enabled;
	bool faulted;
	bool picture_ready;
	bool terminal;
	bool draining;
};

int s7d_h264_input_build_plan(const struct s7d_v4l2_input *input,
			    struct s7d_h264_input_plan *plan)
{
	struct s7d_h264_input_plan next;
	dma_addr_t dma;
	size_t ring_bytes, minimum, write_offset;
	u64 last;

	if (!input || !plan || !input->memory || !input->token || !input->bytes ||
	    input->bytes > U32_MAX / 8)
		return -EINVAL;
	minimum = input->bytes < PAGE_SIZE ?
		PAGE_ALIGN(input->bytes + INPUT_ALIGNMENT) :
		ALIGN(input->bytes, INPUT_ALIGNMENT) + INPUT_ALIGNMENT;
	if (input->padded_bytes < minimum ||
	    !IS_ALIGNED(input->padded_bytes, INPUT_ALIGNMENT) ||
	    check_add_overflow(input->padded_bytes, (size_t)SZ_16K, &ring_bytes) ||
	    ring_bytes > U32_MAX || ring_bytes > s7d_codec_dma_size(input->memory) ||
	    !s7d_codec_dma_cpu(input->memory))
		return -EINVAL;
	dma = s7d_codec_dma_addr(input->memory);
	if (!IS_ALIGNED(dma, INPUT_ALIGNMENT) ||
	    check_add_overflow((u64)dma, (u64)ring_bytes - 1, &last) ||
	    last > U32_MAX)
		return -ERANGE;
	write_offset = round_down(input->bytes + FIFO_PADDING, FIFO_ALIGNMENT);
	if (write_offset <= input->bytes || write_offset >= ring_bytes - FIFO_ALIGNMENT)
		return -EINVAL;
	next.start = dma;
	next.end = dma + ring_bytes - FIFO_ALIGNMENT;
	next.write_pointer = dma + write_offset;
	next.ring_bytes = ring_bytes;
	next.decode_bytes = input->bytes;
	next.decode_bits = input->bytes * 8;
	next.token = input->token;
	next.timestamp_ns = input->timestamp_ns;
	*plan = next;
	return 0;
}

struct s7d_h264_input *
s7d_h264_input_create(struct s7d_codec_dos *dos, struct s7d_codec_session *session,
		      struct s7d_h264_parameters *parameters, bool hunting)
{
	struct s7d_h264_input *input;
	struct regmap *map;

	if (IS_ERR_OR_NULL(dos) || IS_ERR_OR_NULL(session) ||
	    IS_ERR_OR_NULL(parameters))
		return ERR_PTR(-EINVAL);
	map = s7d_codec_dos_regmap(dos, S7D_CODEC_ENGINE_VDEC);
	if (IS_ERR_OR_NULL(map))
		return map ? ERR_CAST(map) : ERR_PTR(-ENODEV);
	input = kzalloc(sizeof(*input), GFP_KERNEL);
	if (!input)
		return ERR_PTR(-ENOMEM);
	input->au_parser = s7d_h264_au_create(parameters, hunting);
	if (IS_ERR(input->au_parser)) {
		void *error = input->au_parser;

		kfree(input);
		return ERR_CAST(error);
	}
	input->map = map;
	input->session = session;
	return input;
}

int s7d_h264_input_destroy(struct s7d_h264_input *input)
{
	if (IS_ERR_OR_NULL(input))
		return -EINVAL;
	if (input->active || input->preflight_ready)
		return -EBUSY;
	s7d_h264_au_destroy(input->au_parser);
	kfree(input);
	return 0;
}

static int input_write(struct s7d_h264_input *input, u32 reg, u32 value)
{
	int ret = regmap_write(input->map, reg, value);

	if (ret)
		input->faulted = true;
	return ret;
}

static int input_check_parked(struct s7d_h264_input *input)
{
	const u32 regs[] = { VDEC_MPSR, VDEC_CPSR, VDEC_MBOX1_MASK, VLD_CONTROL };
	u32 value;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_read(input->map, regs[i], &value);
		if (ret)
			return ret;
		if (i == ARRAY_SIZE(regs) - 1 ? value & FIFO_ENABLE : value)
			return -EBUSY;
	}
	return 0;
}

static void input_clear(struct s7d_h264_input *input);

static int input_hunt_bytes(const u8 *data, u32 bytes, u32 aud_end,
			    u8 *output, u32 *copied)
{
	struct au_prefix prefix, next;
	u32 start, end, at = 0, total;
	bool has_next;

	if (!au_find_prefix(data, bytes, aud_end, &prefix))
		return -EBADMSG;
	for (;;) {
		has_next = au_find_prefix(data, bytes, prefix.header + 1, &next);
		start = prefix.start;
		if (start > aud_end && !data[start - 1])
			start--;
		end = has_next ? next.start : bytes;
		if (has_next && end && !data[end - 1])
			end--;
		if ((data[prefix.header] & 31) != 7 &&
		    (data[prefix.header] & 31) != 8) {
			if (start < aud_end || end < start ||
			    check_add_overflow(at, end - start, &total))
				return -EOVERFLOW;
			if (output)
				memcpy(output + at, data + start, end - start);
			at = total;
		}
		if (!has_next)
			break;
		prefix = next;
	}
	*copied = at;
	return 0;
}

static int input_hunt_validate(struct s7d_h264_au_parser *parser,
			       const u8 *data, u32 bytes,
			       struct s7d_h264_au_info *au,
			       struct s7d_h264_preflight *preflight)
{
	struct s7d_h264_au_parser validator = { .hunting = true };
	struct s7d_h264_parameters *view;
	struct au_pending *original = parser->pending;
	struct s7d_h264_au_info checked;
	u32 i, end;
	int ret;

	view = kvzalloc(sizeof(*view), GFP_KERNEL);
	if (!view)
		return -ENOMEM;
	view->cache = original->cache;
	view->raw_bytes = original->raw_bytes;
	view->selected_sps = original->selected_sps;
	view->selected_pps = original->selected_pps;
	view->selected_valid = original->selected_valid;
	for (i = 0; i < AU_SPS_COUNT; i++)
		view->sps[i] = *au_raw_get(parser, original, true, i);
	for (i = 0; i < AU_PPS_COUNT; i++)
		view->pps[i] = *au_raw_get(parser, original, false, i);
	validator.parameters = view;
	ret = s7d_h264_au_validate(&validator, data, bytes, &checked, preflight);
	if (!ret) {
		end = checked.last_vcl_end;
		checked.last_vcl_end = au->last_vcl_end;
		if (preflight->kind != S7D_H264_INPUT_PICTURE ||
		    memcmp(&checked, au, sizeof(checked)))
			ret = -EBADMSG;
		checked.last_vcl_end = end;
	}
	if (!ret) {
		original->info = checked;
		original->preflight = *preflight;
		*au = checked;
	}
	s7d_h264_au_discard(&validator);
	/* Raw slots are borrowed; only the temporary view itself is owned here. */
	kvfree(view);
	return ret;
}

static int input_assemble(struct s7d_h264_input *input,
			  struct s7d_v4l2_input *buffer,
			  struct s7d_h264_input_plan *plan,
			  struct s7d_h264_au_info *au,
			  struct s7d_h264_preflight *preflight)
{
	struct s7d_h264_au_parser *parser = input->au_parser;
	const struct au_raw *sps = NULL, *pps = NULL;
	u8 *cpu = s7d_codec_dma_cpu(buffer->memory), *assembled;
	u32 aud_end = parser->pending->aud_end;
	u32 replay_bytes = 0, prefix_bytes, at, current_bytes;
	size_t total, padded, original_bytes = buffer->bytes;
	int ret;

	if (parser->hunting) {
		if (!au->idr || au->sps_id >= AU_SPS_COUNT || au->pps_id >= AU_PPS_COUNT)
			return -EBADMSG;
		sps = au_raw_get(parser, parser->pending, true, au->sps_id);
		pps = au_raw_get(parser, parser->pending, false, au->pps_id);
		if (!sps->data || !sps->bytes || !pps->data || !pps->bytes)
			return -ENOENT;
		ret = au_replay_pps_check(parser->pending, pps, au->pps_id, au->sps_id);
		if (ret)
			return ret;
		if (check_add_overflow(sps->bytes, pps->bytes, &replay_bytes) ||
		    replay_bytes > S7D_H264_PARAMETERS_MAX_BYTES)
			return -EOVERFLOW;
	}
	if (check_add_overflow(parser->prefix_bytes, replay_bytes, &prefix_bytes) ||
	    prefix_bytes > S7D_H264_ASSEMBLY_MAX_BYTES || aud_end > original_bytes)
		return -EOVERFLOW;
	if (!prefix_bytes)
		return 0;
	current_bytes = original_bytes - aud_end;
	if (parser->hunting) {
		ret = input_hunt_bytes(cpu, original_bytes, aud_end, NULL, &current_bytes);
		if (ret)
			return ret;
	}
	if (check_add_overflow((size_t)aud_end, (size_t)current_bytes, &total) ||
	    check_add_overflow(total, (size_t)prefix_bytes, &total) ||
	    total > U32_MAX / 8 ||
	    check_add_overflow(total, (size_t)INPUT_ALIGNMENT, &padded))
		return -EOVERFLOW;
	buffer->bytes = total;
	buffer->padded_bytes = total < PAGE_SIZE ?
		PAGE_ALIGN(padded) : ALIGN(total, INPUT_ALIGNMENT) + INPUT_ALIGNMENT;
	ret = s7d_h264_input_build_plan(buffer, plan);
	if (ret)
		return ret;
	assembled = kvmalloc(total, GFP_KERNEL);
	if (!assembled)
		return -ENOMEM;
	if (aud_end)
		memcpy(assembled, cpu, aud_end);
	at = aud_end;
	if (replay_bytes) {
		memcpy(assembled + at, sps->data, sps->bytes);
		at += sps->bytes;
		memcpy(assembled + at, pps->data, pps->bytes);
		at += pps->bytes;
	}
	if (parser->prefix_bytes)
		memcpy(assembled + at, parser->prefix, parser->prefix_bytes);
	if (parser->hunting) {
		ret = input_hunt_bytes(cpu, original_bytes, aud_end,
				       assembled + aud_end + prefix_bytes, &current_bytes);
		if (!ret)
			ret = input_hunt_validate(parser, assembled, total, au, preflight);
	} else {
		memcpy(assembled + aud_end + prefix_bytes, cpu + aud_end,
		       original_bytes - aud_end);
		s7d_h264_au_discard(parser);
		ret = s7d_h264_au_validate(parser, assembled, total, au, preflight);
		if (!ret && preflight->kind != S7D_H264_INPUT_PICTURE)
			ret = -EBADMSG;
	}
	if (!ret) {
		memcpy(cpu, assembled, total);
		memset(cpu + total, 0, plan->ring_bytes - total);
	}
	kvfree(assembled);
	return ret;
}

int s7d_h264_input_preflight(struct s7d_h264_input *input,
			    const struct s7d_v4l2_input *buffer,
			    struct s7d_h264_preflight *preflight)
{
	struct s7d_h264_preflight next;
	struct s7d_h264_input_plan plan;
	struct s7d_v4l2_input prepared;
	struct s7d_h264_au_info au;
	bool trimmed = false;
	int ret;

	if (IS_ERR_OR_NULL(input) || !preflight)
		return -EINVAL;
	if (input->terminal)
		return -ESHUTDOWN;
	if (input->active || input->preflight_ready || input->faulted)
		return -EBUSY;
	ret = s7d_h264_input_build_plan(buffer, &plan);
	if (ret)
		return ret;
	if (buffer->token <= input->last_token)
		return -ESTALE;
	if (input->draining && buffer->token > input->drain.last_input_token)
		return -EPIPE;
	ret = s7d_h264_au_validate(input->au_parser,
				 s7d_codec_dma_cpu(buffer->memory), buffer->bytes,
				 &au, &next);
	if (input->au_parser->hunting &&
	    (ret == -ENOENT || ret == -EBADMSG || ret == -EOPNOTSUPP))
		ret = s7d_h264_au_skip(input->au_parser, ret == -ENOENT ? 0 : ret,
				       &au, &next);
	if (ret)
		return ret;
	prepared = *buffer;
	if (next.kind == S7D_H264_INPUT_PICTURE) {
		trimmed = next.picture_bytes < prepared.bytes;
		if (trimmed) {
			prepared.bytes = next.picture_bytes;
			ret = s7d_h264_input_build_plan(&prepared, &plan);
			if (ret)
				goto discard;
		}
		ret = input_assemble(input, &prepared, &plan, &au, &next);
		if (ret && input->au_parser->hunting &&
		    (ret == -ENOENT || ret == -EBADMSG || ret == -EOPNOTSUPP)) {
			s7d_h264_au_discard(input->au_parser);
			ret = s7d_h264_au_skip(input->au_parser, ret == -ENOENT ? 0 : ret,
					       &au, &next);
			if (ret)
				goto discard;
			prepared = *buffer;
			trimmed = false;
			ret = s7d_h264_input_build_plan(buffer, &plan);
		}
		if (ret)
			goto discard;
		if (trimmed)
			memset(s7d_codec_dma_cpu(prepared.memory) + prepared.bytes, 0,
			       plan.ring_bytes - prepared.bytes);
	}
	input->buffer = prepared;
	input->plan = plan;
	input->au = au;
	input->last_token = buffer->token;
	input->preflight_ready = true;
	*preflight = next;
	return 0;

discard:
	s7d_h264_au_discard(input->au_parser);
	return ret;
}

static int input_preflight_check(struct s7d_h264_input *input, u64 token)
{
	if (IS_ERR_OR_NULL(input) || !token)
		return -EINVAL;
	if (input->active || input->faulted)
		return -EBUSY;
	if (!input->preflight_ready || input->buffer.token != token ||
	    !input->au_parser->pending)
		return -ESTALE;
	return 0;
}

int s7d_h264_input_accept_headers(struct s7d_h264_input *input, u64 token,
				 struct s7d_h264_input_return *returned)
{
	struct s7d_h264_input_return next;
	int ret = input_preflight_check(input, token);

	if (ret || !returned)
		return ret ? ret : -EINVAL;
	if (input->au_parser->pending->preflight.kind != S7D_H264_INPUT_HEADERS)
		return -EINVAL;
	next.token = token;
	next.timestamp_ns = input->buffer.timestamp_ns;
	next.status = 0;
	ret = s7d_h264_au_commit(input->au_parser);
	if (ret)
		return ret;
	input->last_consumed_token = token;
	input_clear(input);
	*returned = next;
	return 0;
}

int s7d_h264_input_accept_skip(struct s7d_h264_input *input, u64 token,
			     struct s7d_h264_input_return *returned)
{
	struct s7d_h264_input_return next;
	int ret = input_preflight_check(input, token);

	if (ret || !returned)
		return ret ? ret : -EINVAL;
	if (!input->au_parser->hunting ||
	    input->au_parser->pending->preflight.kind != S7D_H264_INPUT_SKIP)
		return -EINVAL;
	next.token = token;
	next.timestamp_ns = input->buffer.timestamp_ns;
	next.status = input->au_parser->pending->preflight.skip_status;
	ret = s7d_h264_au_commit(input->au_parser);
	if (ret)
		return ret;
	input->last_consumed_token = token;
	input_clear(input);
	*returned = next;
	return 0;
}

int s7d_h264_input_reject_picture(struct s7d_h264_input *input, u64 token,
				int status,
				struct s7d_h264_input_return *returned)
{
	struct s7d_h264_au_parser *parser;
	struct s7d_h264_input_return next;
	int ret = input_preflight_check(input, token);

	if (ret || !returned)
		return ret ? ret : -EINVAL;
	parser = input->au_parser;
	if (!parser->hunting || input->terminal || input->hw_prepared ||
	    input->configured || input->enabled || status >= 0 ||
	    parser->pending->preflight.kind != S7D_H264_INPUT_PICTURE)
		return -EINVAL;
	next.token = token;
	next.timestamp_ns = input->buffer.timestamp_ns;
	next.status = status;
	s7d_h264_au_discard(parser);
	kvfree(parser->prefix);
	parser->prefix = NULL;
	parser->prefix_bytes = 0;
	parser->prefix_nals = 0;
	input->last_consumed_token = token;
	input_clear(input);
	*returned = next;
	return 0;
}

int s7d_h264_input_cancel_preflight(struct s7d_h264_input *input, u64 token,
				   int status,
				   struct s7d_h264_input_return *returned)
{
	struct s7d_h264_input_return next;
	int ret = input_preflight_check(input, token);

	if (ret || !returned)
		return ret ? ret : -EINVAL;
	if (status >= 0)
		return -EINVAL;
	next.token = token;
	next.timestamp_ns = input->buffer.timestamp_ns;
	next.status = status;
	s7d_h264_au_discard(input->au_parser);
	input_clear(input);
	input->terminal = true;
	*returned = next;
	return 0;
}

int s7d_h264_input_prepare(struct s7d_h264_input *input, u64 token)
{
	const struct reg_sequence sequence[] = {
		{ VLD_CONTROL, 0 },
		{ POWER_CTL_VLD, BIT(4) },
	};
	struct s7d_h264_input_plan plan;
	int ret = input_preflight_check(input, token);
	unsigned int i;

	if (ret)
		return ret;
	if (input->au_parser->pending->preflight.kind != S7D_H264_INPUT_PICTURE)
		return -EINVAL;
	ret = input_check_parked(input);
	if (ret)
		return ret;
	plan = input->plan;
	input->active = true;
	input->hw_prepared = true;
	dma_wmb();
	for (i = 0; i < ARRAY_SIZE(sequence); i++) {
		ret = input_write(input, sequence[i].reg, sequence[i].def);
		if (ret)
			return ret;
	}
	ret = input_write(input, VLD_START_PTR, plan.start);
	if (ret)
		return ret;
	ret = input_write(input, VLD_END_PTR, plan.end);
	if (ret)
		return ret;
	ret = input_write(input, VLD_CURR_PTR, plan.start);
	if (ret)
		return ret;
	ret = input_write(input, VLD_CONTROL, 1);
	if (ret)
		return ret;
	ret = input_write(input, VLD_CONTROL, 0);
	if (ret)
		return ret;
	ret = input_write(input, VLD_BUF_CNTL, 2);
	if (ret)
		return ret;
	ret = input_write(input, VLD_RP, plan.start);
	if (ret)
		return ret;
	ret = input_write(input, VLD_WP, plan.write_pointer);
	if (ret)
		return ret;
	ret = input_write(input, VLD_BUF_CNTL, 3);
	if (ret)
		return ret;
	ret = input_write(input, VLD_BUF_CNTL, 2);
	if (ret)
		return ret;
	return input_write(input, VLD_CONTROL, FIFO_MANUAL_CONTROL);
}

static int input_token_check(struct s7d_h264_input *input, u64 token)
{
	if (IS_ERR_OR_NULL(input) || !token)
		return -EINVAL;
	if (!input->active || input->buffer.token != token)
		return -ESTALE;
	return input->faulted ? -EIO : 0;
}

int s7d_h264_input_configure(struct s7d_h264_input *input, u64 token)
{
	const struct reg_sequence sequence[] = {
		{ H264_RESERVED_BITS, 0 },
		{ H264_DECODE_INFO, BIT(13) },
	};
	int ret = input_token_check(input, token);
	unsigned int i;

	if (ret)
		return ret;
	if (input->configured || input->enabled)
		return -EALREADY;
	ret = input_check_parked(input);
	if (ret)
		return ret;
	ret = regmap_update_bits(input->map, POWER_CTL_VLD, BIT(9) | BIT(6),
				 BIT(9) | BIT(6));
	if (ret) {
		input->faulted = true;
		return ret;
	}
	for (i = 0; i < ARRAY_SIZE(sequence); i++) {
		ret = input_write(input, sequence[i].reg, sequence[i].def);
		if (ret)
			return ret;
	}
	ret = input_write(input, H264_DECODE_SIZE, input->plan.decode_bytes);
	if (ret)
		return ret;
	ret = input_write(input, VIFF_BIT_CNT, input->plan.decode_bits);
	if (ret)
		return ret;
	input->configured = true;
	return 0;
}

int s7d_h264_input_enable(struct s7d_h264_input *input, u64 token)
{
	u32 value;
	int ret = input_token_check(input, token);

	if (ret)
		return ret;
	if (!input->configured || input->enabled)
		return -EINVAL;
	dma_wmb();
	ret = regmap_update_bits(input->map, VLD_CONTROL, FIFO_ENABLE, FIFO_ENABLE);
	if (!ret)
		ret = regmap_read(input->map, VLD_CONTROL, &value);
	if (!ret && (value & FIFO_ENABLE) != FIFO_ENABLE)
		ret = -EIO;
	if (ret) {
		input->faulted = true;
		return ret;
	}
	input->enabled = true;
	return 0;
}

int s7d_h264_input_observe(struct s7d_h264_input *input, u64 token,
			 struct s7d_h264_input_observation *observation)
{
	struct s7d_h264_input_observation next;
	u32 status, bits, start, end;
	int ret = input_token_check(input, token);

	if (ret || !observation)
		return ret ? ret : -EINVAL;
	if (!input->enabled)
		return -EINVAL;
	ret = regmap_read(input->map, H264_STATUS, &next.firmware_status);
	if (ret)
		return ret;
	ret = regmap_read(input->map, VIFF_BIT_CNT, &next.remaining_bits);
	if (ret)
		return ret;
	ret = regmap_read(input->map, VLD_RP, &next.read_pointer);
	if (ret)
		return ret;
	ret = regmap_read(input->map, VLD_WP, &next.write_pointer);
	if (ret)
		return ret;
	ret = regmap_read(input->map, VLD_CONTROL, &next.fifo_control);
	if (ret)
		return ret;
	ret = regmap_read(input->map, VLD_START_PTR, &start);
	if (ret)
		return ret;
	ret = regmap_read(input->map, VLD_END_PTR, &end);
	if (ret)
		return ret;
	ret = regmap_read(input->map, VIFF_BIT_CNT, &bits);
	if (ret)
		return ret;
	ret = regmap_read(input->map, H264_STATUS, &status);
	if (ret)
		return ret;
	if (status != next.firmware_status || bits != next.remaining_bits)
		return -EAGAIN;
	if (bits > input->plan.decode_bits ||
	    start != input->plan.start || end != input->plan.end ||
	    next.read_pointer < input->plan.start ||
	    next.read_pointer > input->plan.end ||
	    next.write_pointer != input->plan.write_pointer)
		return -EPROTO;
	next.token = token;
	next.timestamp_ns = input->buffer.timestamp_ns;
	next.whole_bytes_consumed = input->plan.decode_bytes - DIV_ROUND_UP(bits, 8);
	*observation = next;
	return 0;
}

int s7d_h264_input_get_au(struct s7d_h264_input *input, u64 token,
			struct s7d_h264_au_info *au)
{
	if (IS_ERR_OR_NULL(input) || !token || !au)
		return -EINVAL;
	if (!input->preflight_ready || input->buffer.token != token ||
	    !input->au_parser->pending ||
	    input->au_parser->pending->preflight.kind != S7D_H264_INPUT_PICTURE)
		return -ESTALE;
	if (input->faulted)
		return -EIO;
	*au = input->au;
	return 0;
}

int s7d_h264_input_confirm_slice(struct s7d_h264_input *input, u64 token,
				 const struct s7d_h264_slice *slice,
				 u64 write_token)
{
	static const enum s7d_h264_slice_type type[] = {
		S7D_H264_SLICE_P, S7D_H264_SLICE_B, S7D_H264_SLICE_I,
	};
	struct s7d_h264_input_observation observation;
	const struct s7d_h264_au_info *au;
	u32 ordinal;
	int ret = input_token_check(input, token);

	if (ret || !slice || !write_token)
		return ret ? ret : -EINVAL;
	if (input->picture_ready)
		return -EALREADY;
	ret = s7d_h264_input_observe(input, token, &observation);
	if (ret)
		return ret;
	if (observation.firmware_status != H264_SLICE_HEAD_DONE)
		return -EAGAIN;
	au = &input->au;
	ordinal = input->confirmed_slices;
	if (ordinal >= au->slice_count || au->slice_type[ordinal] >= ARRAY_SIZE(type) ||
	    slice->first_mb != au->first_mb[ordinal] ||
	    slice->type != type[au->slice_type[ordinal]] ||
	    slice->frame_num != au->frame_num || slice->idr != au->idr ||
	    slice->reference != au->reference || slice->poc_type != au->poc_type ||
	    slice->log2_frame_num != au->log2_frame_num ||
	    slice->max_references != au->max_references ||
	    (au->poc_type == 0 && (slice->log2_poc_lsb != au->log2_poc_lsb ||
				 slice->poc_lsb != au->poc_lsb ||
				 slice->delta_bottom != au->delta_bottom)) ||
	    (ordinal && input->write_token != write_token)) {
		input->faulted = true;
		return -EBADMSG;
	}
	input->write_token = write_token;
	input->confirmed_slices++;
	return 0;
}

static int input_tail_check(struct s7d_h264_input *input,
			    const struct s7d_h264_input_observation *observation)
{
	const u8 *data = s7d_codec_dma_cpu(input->buffer.memory);
	u32 offset = observation->whole_bytes_consumed;
	u64 tail_bits;

	if (offset < input->au.last_vcl_end) {
		tail_bits = (u64)(input->buffer.bytes - input->au.last_vcl_end) * 8;
		tail_bits += input->au.cavlc_tail_bits;
		if (!input->au.cavlc_tail_bits || observation->remaining_bits > tail_bits)
			return -EOPNOTSUPP;
		offset = input->au.last_vcl_end;
	}
	for (; offset < input->buffer.bytes; offset++)
		if (data[offset])
			return -EBADMSG;
	return 0;
}

int s7d_h264_input_confirm_config(struct s7d_h264_input *input, u64 token,
				  u32 snapshot_sequence)
{
	u32 status, sequence, mb_width, mb_total;
	u32 width, height;
	int ret = input_token_check(input, token);

	if (ret)
		return ret;
	if (!input->configured || !input->enabled || input->confirmed_slices ||
	    input->picture_ready)
		return -ESTALE;
	ret = regmap_read(input->map, H264_STATUS, &status);
	if (ret)
		return ret;
	if (status != S7D_H264_CONFIG_REQUEST)
		return -ESTALE;
	ret = regmap_read(input->map, H264_CONFIG_SEQINFO, &sequence);
	if (ret)
		return ret;
	ret = regmap_read(input->map, H264_STATUS, &status);
	if (ret)
		return ret;
	if (status != S7D_H264_CONFIG_REQUEST || sequence != snapshot_sequence)
		return -ESTALE;
	width = input->au.coded_width / 16U;
	height = input->au.coded_height / 16U;
	mb_total = (sequence >> 8) & 0xffff;
	mb_width = sequence & 0xff;
	if (!mb_width && mb_total)
		mb_width = 256;
	if (!width || !height || width > 256 || height > 256 ||
	    (input->au.coded_width & 15U) || (input->au.coded_height & 15U) ||
	    mb_width != width || mb_total != width * height)
		return -EBADMSG;
	if (input->sequence_info &&
	    ((input->sequence_info ^ sequence) & 0xffffffU))
		return -EOPNOTSUPP;
	input->sequence_info = sequence;
	return 0;
}

static int input_picture_readback(struct s7d_h264_input *input, u64 token,
				  struct s7d_h264_input_observation *picture)
{
	struct s7d_h264_input_observation first, second;
	u32 sequence, position, error, mb_width, mb_total;
	u32 expected_width = input->au.coded_width / 16U;
	u32 expected_height = input->au.coded_height / 16U;
	int ret;

	ret = s7d_h264_input_observe(input, token, &first);
	if (ret)
		return ret;
	if (first.firmware_status != H264_PIC_DATA_DONE)
		return -EAGAIN;
	if (!input->sequence_info)
		return -ESTALE;
	sequence = input->sequence_info;
	ret = regmap_read(input->map, H264_MBY_MBX, &position);
	if (ret)
		return ret;
	ret = regmap_read(input->map, H264_ERROR, &error);
	if (ret)
		return ret;
	ret = s7d_h264_input_observe(input, token, &second);
	if (ret)
		return ret;
	if (second.firmware_status != first.firmware_status ||
	    second.remaining_bits != first.remaining_bits)
		return -EAGAIN;
	mb_total = (sequence >> 8) & 0xffff;
	mb_width = sequence & 0xff;
	if (!mb_width && mb_total)
		mb_width = 256;
	if (!expected_width || !expected_height || expected_width > 256 ||
	    expected_height > 256 || error || mb_width != expected_width ||
	    mb_total != expected_width * expected_height ||
	    (position & 0xff) != expected_height - 1 ||
	    ((position >> 8) & 0xff) != expected_width - 1)
		return -EBADMSG;
	ret = input_tail_check(input, &second);
	if (ret)
		return ret;
	*picture = second;
	return 0;
}

int s7d_h264_input_confirm_picture(struct s7d_h264_input *input, u64 token,
				   u64 write_token)
{
	int ret = input_token_check(input, token);

	if (ret || !write_token)
		return ret ? ret : -EINVAL;
	if (input->picture_ready)
		return -EALREADY;
	if (input->confirmed_slices != input->au.slice_count ||
	    input->write_token != write_token)
		return -ESTALE;
	ret = input_picture_readback(input, token, &input->picture);
	if (ret) {
		if (ret != -EAGAIN)
			input->faulted = true;
		return ret;
	}
	input->picture_ready = true;
	return 0;
}

int s7d_h264_input_get_picture(struct s7d_h264_input *input,
			     struct s7d_codec_session *session, u64 token,
			     u64 write_token,
			     struct s7d_h264_input_observation *picture)
{
	struct s7d_h264_input_observation fresh;
	int ret = input_token_check(input, token);

	if (ret || !picture || IS_ERR_OR_NULL(session))
		return ret ? ret : -EINVAL;
	if (input->session != session)
		return -ESTALE;
	if (!input->picture_ready || !write_token || input->write_token != write_token)
		return -ESTALE;
	ret = input_picture_readback(input, token, &fresh);
	if (ret)
		return ret;
	if (fresh.remaining_bits != input->picture.remaining_bits)
		return -ESTALE;
	*picture = fresh;
	return 0;
}

static void input_clear(struct s7d_h264_input *input)
{
	input->active = false;
	input->preflight_ready = false;
	input->configured = false;
	input->enabled = false;
	input->faulted = false;
	input->picture_ready = false;
	input->confirmed_slices = 0;
	input->write_token = 0;
	memset(&input->buffer, 0, sizeof(input->buffer));
	memset(&input->plan, 0, sizeof(input->plan));
	memset(&input->au, 0, sizeof(input->au));
	memset(&input->picture, 0, sizeof(input->picture));
}

int s7d_h264_input_return_after_park(struct s7d_h264_input *input,
				  struct s7d_vdec_runtime *runtime,
				  u64 token, u64 write_token, u64 park_epoch,
				  struct s7d_h264_input_return *returned)
{
	struct s7d_h264_input_return next;
	int ret = input_token_check(input, token);

	if (ret || !returned || !park_epoch || IS_ERR_OR_NULL(runtime))
		return ret ? ret : -EINVAL;
	if (!input->picture_ready || input->write_token != write_token)
		return -ESTALE;
	ret = s7d_vdec_runtime_check_parked(runtime, input->session, token,
					write_token, park_epoch);
	if (ret)
		return ret;
	ret = input_check_parked(input);
	if (ret)
		return ret;
	next.token = token;
	next.timestamp_ns = input->buffer.timestamp_ns;
	next.status = 0;
	ret = s7d_h264_au_commit(input->au_parser);
	if (ret)
		return ret;
	input->au_parser->hunting = false;
	input->last_completed_token = token;
	input->last_consumed_token = token;
	input->last_completed_write = write_token;
	input->last_park_epoch = park_epoch;
	input_clear(input);
	*returned = next;
	return 0;
}

int s7d_h264_input_begin_drain(struct s7d_h264_input *input,
			     const struct s7d_v4l2_drain *request)
{
	if (IS_ERR_OR_NULL(input) || !request || !request->epoch)
		return -EINVAL;
	if (input->terminal)
		return -ESHUTDOWN;
	if (input->faulted)
		return -EIO;
	if (input->draining)
		return -EBUSY;
	if (request->epoch <= input->last_drain_epoch ||
	    request->last_input_token < input->last_token)
		return -ESTALE;
	input->drain = *request;
	input->last_drain_epoch = request->epoch;
	input->draining = true;
	return 0;
}

static int input_drain_match(struct s7d_h264_input *input,
			     const struct s7d_v4l2_drain *request)
{
	if (IS_ERR_OR_NULL(input) || !request || !request->epoch)
		return -EINVAL;
	if (input->terminal)
		return -ESHUTDOWN;
	if (input->faulted)
		return -EIO;
	if (!input->draining || input->drain.epoch != request->epoch ||
	    input->drain.last_input_token != request->last_input_token)
		return -ESTALE;
	if (input->active || input->preflight_ready ||
	    input->last_consumed_token != request->last_input_token)
		return -EAGAIN;
	return 0;
}

int s7d_h264_input_check_header_drain(struct s7d_h264_input *input,
				    const struct s7d_v4l2_drain *request)
{
	int ret = input_drain_match(input, request);

	if (ret)
		return ret;
	if (input->hw_prepared || input->last_completed_token)
		return -EOPNOTSUPP;
	return 0;
}

int s7d_h264_input_check_drain(struct s7d_h264_input *input,
			     struct s7d_vdec_runtime *runtime,
			     const struct s7d_v4l2_drain *request)
{
	int ret = input_drain_match(input, request);

	if (ret)
		return ret;
	if (!input->last_completed_token)
		return s7d_h264_input_check_header_drain(input, request);
	if (IS_ERR_OR_NULL(runtime))
		return -EINVAL;
	return s7d_vdec_runtime_check_parked(runtime, input->session,
					   input->last_completed_token,
					   input->last_completed_write,
					   input->last_park_epoch);
}

int s7d_h264_input_end_drain(struct s7d_h264_input *input,
			   const struct s7d_v4l2_drain *request)
{
	int ret = input_drain_match(input, request);

	if (ret)
		return ret;
	input->draining = false;
	memset(&input->drain, 0, sizeof(input->drain));
	return 0;
}

int s7d_h264_input_return_after_quiesce(struct s7d_h264_input *input,
				      struct s7d_vdec_runtime *runtime,
				      u64 token, int status,
				      struct s7d_h264_input_return *returned)
{
	struct s7d_h264_input_return next;
	int ret;

	if (IS_ERR_OR_NULL(input) || !returned || !token)
		return -EINVAL;
	if (!input->active || input->buffer.token != token)
		return -ESTALE;
	if (status >= 0)
		return -EOPNOTSUPP;
	if (IS_ERR_OR_NULL(runtime))
		return -EINVAL;
	ret = s7d_vdec_runtime_check_quiesced(runtime, input->session);
	if (ret)
		return ret;
	next.token = token;
	next.timestamp_ns = input->buffer.timestamp_ns;
	next.status = status;
	s7d_h264_au_discard(input->au_parser);
	input_clear(input);
	input->terminal = true;
	*returned = next;
	return 0;
}
