// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "s7d-codec-session.h"
#include "s7d-hevc-context.h"
#include "s7d-hevc-rpm.h"
#include "s7d-hevc-sequence.h"

#define DOS_REG(word) ((word) << 2)
#define INPUT_PADDING SZ_16K
#define PREFIX_BYTES 16U
#define AUX_PART_BYTES (12U * SZ_1K)

struct s7d_hevc_context {
	struct s7d_codec_session *session;
	struct s7d_codec_dma *memory;
	struct s7d_hevc_workspace workspace;
	struct s7d_hevc_sps host;
	struct s7d_hevc_input_plan input;
	u8 *cpu;
	u32 base;
	u32 input_offset;
	u32 input_capacity;
	u32 ring_bytes;
	bool staged;
	bool attempted;
	bool programmed;
};

static size_t hevc_start_code(const u8 *data, size_t bytes, size_t pos)
{
	if (pos + 3 <= bytes && !data[pos] && !data[pos + 1]) {
		if (data[pos + 2] == 1)
			return 3;
		if (pos + 4 <= bytes && !data[pos + 2] && data[pos + 3] == 1)
			return 4;
	}
	return 0;
}

/* Validate escaped bytes, keeping enough initial RBSP bits for IDs only. */
static int hevc_prefix(const u8 *data, size_t bytes, u8 *prefix, u32 *count)
{
	u32 zeros = 0, kept = 0;
	size_t i;

	for (i = 0; i < bytes; i++) {
		if (zeros >= 2 && data[i] <= 3) {
			if (data[i] != 3 || i + 1 == bytes || data[i + 1] > 3)
				return -EBADMSG;
			zeros = 0;
			continue;
		}
		if (kept < PREFIX_BYTES)
			prefix[kept++] = data[i];
		zeros = data[i] ? 0 : zeros + 1;
	}
	*count = kept;
	return kept ? 0 : -EBADMSG;
}

static int hevc_ue(const u8 *data, u32 bytes, u32 *bit, u32 *value)
{
	u32 zeros = 0, result = 1;
	bool one = false;

	while (*bit < bytes * 8) {
		one = data[*bit / 8] & (0x80 >> (*bit & 7));
		(*bit)++;
		if (one)
			break;
		if (++zeros > 31)
			return -EBADMSG;
	}
	if (!one || zeros > bytes * 8 - *bit)
		return -EBADMSG;
	while (zeros--) {
		result = (result << 1) | !!(data[*bit / 8] &
						  (0x80 >> (*bit & 7)));
		(*bit)++;
	}
	*value = result - 1;
	return 0;
}

int s7d_hevc_input_validate_annexb(const u8 *data, size_t bytes,
				   struct s7d_hevc_sps *sps)
{
	struct s7d_hevc_sps next;
	u32 nals = 0, vps_id = 0, pps_id = 0;
	size_t pos = 0;
	int ret;

	if (!data || !sps || !bytes)
		return -EINVAL;
	if (bytes > S7D_HEVC_INPUT_MAX_BYTES)
		return -E2BIG;
	/* Annex B permits leading_zero_8bits before the first start code. */
	while (pos < bytes && !hevc_start_code(data, bytes, pos) && !data[pos])
		pos++;
	while (pos < bytes) {
		u8 prefix[PREFIX_BYTES];
		u32 prefix_bytes, type, id, bit = 0;
		size_t code = hevc_start_code(data, bytes, pos);
		size_t start = pos + code, end;

		if (!code || nals >= 4)
			return -EBADMSG;
		for (pos = start; pos < bytes; pos++)
			if (hevc_start_code(data, bytes, pos))
				break;
		end = pos;
		/* Annex B trailing zeros are outside the NAL's escaped RBSP. */
		while (end > start && !data[end - 1])
			end--;
		if (end - start < 3)
			return -EBADMSG;
		if ((data[start] & 0x81) || data[start + 1] != 1)
			return -EOPNOTSUPP;
		type = (data[start] >> 1) & 0x3f;
		if ((nals < 3 && type != 32 + nals) ||
		    (nals == 3 && type != 19 && type != 20))
			return -EOPNOTSUPP;
		ret = hevc_prefix(data + start + 2, end - start - 2,
				  prefix, &prefix_bytes);
		if (ret)
			return ret;
		if (!nals) {
			vps_id = prefix[0] >> 4;
		} else if (nals == 1) {
			ret = s7d_hevc_parse_sps(data + start, end - start, &next);
			if (ret)
				return ret;
			if (next.vps_id != vps_id)
				return -EBADMSG;
			/* Selected workspace reserves eight 256-byte SPS slots. */
			if (next.sps_id >= 8)
				return -EOPNOTSUPP;
		} else if (nals == 2) {
			ret = hevc_ue(prefix, prefix_bytes, &bit, &pps_id);
			if (ret)
				return ret;
			ret = hevc_ue(prefix, prefix_bytes, &bit, &id);
			if (ret)
				return ret;
			if (pps_id > 63 || id != next.sps_id)
				return -EBADMSG;
		} else {
			if (!(prefix[0] & 0x80))
				return -EOPNOTSUPP;
			/* first_slice_segment flag and no_output_of_prior_pics flag */
			bit = 2;
			ret = hevc_ue(prefix, prefix_bytes, &bit, &id);
			if (ret)
				return ret;
			if (id != pps_id)
				return -EBADMSG;
		}
		nals++;
	}
	if (nals != 4)
		return -EBADMSG;
	*sps = next;
	return 0;
}

static void hevc_region(u32 *offset, u32 bytes, u32 alignment,
			struct s7d_hevc_dma_region *region)
{
	*offset = ALIGN(*offset, alignment);
	region->address = *offset;
	region->bytes = bytes;
	*offset += bytes;
}

/* Fixed spec[5] capacities/order; MMU regions remain reserved and unused. */
static u32 hevc_workspace_layout(struct s7d_hevc_workspace *w)
{
	struct s7d_hevc_dma_region reserved;
	u32 offset = 0;

	hevc_region(&offset, 0x8000, SZ_4K, &w->ipp);
	hevc_region(&offset, 0, SZ_4K, &w->sao_abv);
	hevc_region(&offset, 0, SZ_4K, &w->sao_vb);
	hevc_region(&offset, 0x800, SZ_4K, &w->short_term_rps);
	hevc_region(&offset, 0x800, SZ_4K, &w->vps);
	hevc_region(&offset, 0x800, SZ_4K, &w->sps);
	hevc_region(&offset, 0x2000, SZ_4K, &w->pps);
	hevc_region(&offset, 0, SZ_4K, &w->sao_up);
	hevc_region(&offset, 0x800, SZ_4K, &w->swap1);
	hevc_region(&offset, 0x800, SZ_4K, &w->swap2);
	hevc_region(&offset, 0x8000, SZ_4K, &w->scalelut);
	hevc_region(&offset, 0x40100, SZ_4K, &w->dblk_para);
	hevc_region(&offset, 0x140800, SZ_4K, &w->dblk_data);
	hevc_region(&offset, 0x100800, SZ_4K, &w->dblk_data2);
	hevc_region(&offset, 0xa000, SZ_4K, &reserved);
	hevc_region(&offset, 0x14000, SZ_4K, &reserved);
	hevc_region(&offset, 0x8000, SZ_4K, &w->mpred_above);
	/* Preserve unused workspace RPM/LMEM slots before real coherent buffers. */
	hevc_region(&offset, 0x100, SZ_4K, &reserved);
	hevc_region(&offset, 0xa00, SZ_4K, &reserved);
	hevc_region(&offset, S7D_HEVC_RPM_BYTES, SZ_4K, &w->rpm);
	hevc_region(&offset, 0xc00, SZ_4K, &w->lmem);
	hevc_region(&offset, 2 * AUX_PART_BYTES, SZ_4K, &w->aux);
	w->aux_prefix_bytes = AUX_PART_BYTES;
	w->aux_suffix_bytes = AUX_PART_BYTES;
	hevc_region(&offset, 0x40000, SZ_64K, &w->mv);
	return ALIGN(offset, SZ_4K);
}

static void hevc_workspace_relocate(struct s7d_hevc_workspace *w, u32 base)
{
	w->ipp.address += base;
	w->sao_abv.address += base;
	w->sao_vb.address += base;
	w->short_term_rps.address += base;
	w->vps.address += base;
	w->sps.address += base;
	w->pps.address += base;
	w->sao_up.address += base;
	w->swap1.address += base;
	w->swap2.address += base;
	w->scalelut.address += base;
	w->dblk_para.address += base;
	w->dblk_data.address += base;
	w->dblk_data2.address += base;
	w->mpred_above.address += base;
	w->rpm.address += base;
	w->lmem.address += base;
	w->aux.address += base;
	w->mv.address += base;
}

struct s7d_hevc_context *
s7d_hevc_context_create(struct s7d_codec_session *session,
			const struct s7d_hevc_sps *sps, size_t input_capacity)
{
	struct s7d_codec_layout layout;
	struct s7d_hevc_context *ctx;
	dma_addr_t dma;
	u64 base, last;
	u32 total;
	int ret;

	if (IS_ERR_OR_NULL(session) || !sps || !input_capacity)
		return ERR_PTR(-EINVAL);
	if (input_capacity > S7D_HEVC_INPUT_MAX_BYTES)
		return ERR_PTR(-E2BIG);
	ret = s7d_hevc_build_layout(sps, S7D_CODEC_CAPTURE_NV12M, 0, &layout);
	if (ret)
		return ERR_PTR(ret);
	if (sps->coded_width < 64 || sps->coded_height < 64 ||
	    sps->max_sub_layers != 1 || sps->log2_ctb_size < 4 ||
	    sps->log2_ctb_size > 6)
		return ERR_PTR(-EOPNOTSUPP);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);
	ctx->host = *sps;
	ctx->input_capacity = input_capacity;
	ctx->input_offset = hevc_workspace_layout(&ctx->workspace);
	/* WR cannot wrap; at least one 8-byte element remains after read-ahead. */
	ctx->ring_bytes = ALIGN(input_capacity + INPUT_PADDING + 8, SZ_4K);
	total = ctx->input_offset + ctx->ring_bytes;
	s7d_codec_session_get(session);
	ctx->session = session;
	ctx->memory = s7d_codec_dma_alloc(session, total + SZ_64K - 1);
	if (IS_ERR(ctx->memory)) {
		ret = PTR_ERR(ctx->memory);
		goto release;
	}
	dma = s7d_codec_dma_addr(ctx->memory);
	base = ALIGN((u64)dma, (u64)SZ_64K);
	last = base + total;
	if (last > U32_MAX || base < dma ||
	    base - dma + total > s7d_codec_dma_size(ctx->memory)) {
		ret = -ERANGE;
		goto release;
	}
	ctx->base = base;
	ctx->cpu = (u8 *)s7d_codec_dma_cpu(ctx->memory) + (base - dma);
	hevc_workspace_relocate(&ctx->workspace, ctx->base);
	memset(ctx->cpu, 0, total);
	dma_wmb();
	return ctx;

release:
	/* The core retains accepted DMA even here until verified session stop. */
	s7d_codec_session_put(session);
	kfree(ctx);
	return ERR_PTR(ret);
}

void s7d_hevc_context_release(struct s7d_hevc_context *context)
{
	if (IS_ERR_OR_NULL(context))
		return;
	s7d_codec_session_put(context->session);
	kfree(context);
}

const struct s7d_hevc_workspace *
s7d_hevc_context_workspace(const struct s7d_hevc_context *context)
{
	return context ? &context->workspace : NULL;
}

static bool hevc_same_sps(const struct s7d_hevc_sps *a,
			  const struct s7d_hevc_sps *b)
{
	return a->vps_id == b->vps_id &&
	       a->sps_id == b->sps_id &&
	       a->profile_idc == b->profile_idc &&
	       a->level_idc == b->level_idc &&
	       a->max_sub_layers == b->max_sub_layers &&
	       a->coded_width == b->coded_width &&
	       a->coded_height == b->coded_height &&
	       a->crop_left == b->crop_left &&
	       a->crop_right == b->crop_right &&
	       a->crop_top == b->crop_top &&
	       a->crop_bottom == b->crop_bottom &&
	       a->chroma_format_idc == b->chroma_format_idc &&
	       a->bit_depth_luma == b->bit_depth_luma &&
	       a->bit_depth_chroma == b->bit_depth_chroma &&
	       a->log2_min_cb_size == b->log2_min_cb_size &&
	       a->log2_ctb_size == b->log2_ctb_size &&
	       a->log2_max_poc_lsb == b->log2_max_poc_lsb &&
	       a->max_dec_pic_buffering == b->max_dec_pic_buffering &&
	       a->num_reorder_pics == b->num_reorder_pics &&
	       a->separate_colour_plane == b->separate_colour_plane &&
	       a->interlaced_source == b->interlaced_source &&
	       a->field_seq == b->field_seq &&
	       a->colour.primaries == b->colour.primaries &&
	       a->colour.transfer == b->colour.transfer &&
	       a->colour.matrix == b->colour.matrix &&
	       a->colour.flags == b->colour.flags;
}

int s7d_hevc_context_stage(struct s7d_hevc_context *context,
			   const u8 *data, size_t bytes, u64 token,
			   struct s7d_hevc_input_plan *plan)
{
	struct s7d_hevc_sps staged_sps;
	struct s7d_hevc_input_plan next;
	u8 *input;
	int ret;

	if (!context || !data || !plan || !bytes || !token)
		return -EINVAL;
	if (context->staged || context->attempted)
		return -EBUSY;
	if (bytes > context->input_capacity)
		return -E2BIG;
	input = context->cpu + context->input_offset;
	memcpy(input, data, bytes);
	memset(input + bytes, 0, context->ring_bytes - bytes);
	ret = s7d_hevc_input_validate_annexb(input, bytes, &staged_sps);
	if (ret)
		return ret;
	if (!hevc_same_sps(&staged_sps, &context->host))
		return -ESTALE;
	next.start = context->base + context->input_offset;
	next.end = next.start + context->ring_bytes;
	next.read_pointer = next.start;
	next.write_pointer = next.start + round_down(bytes + INPUT_PADDING, 8);
	next.decode_size = bytes;
	next.max_shift_bytes = bytes + 8;
	next.token = token;
	dma_wmb();
	context->input = next;
	context->staged = true;
	*plan = next;
	return 0;
}

static int hevc_program_addresses(struct regmap *map,
				  const struct s7d_hevc_input_plan *input)
{
	const struct reg_sequence regs[] = {
		{ DOS_REG(0x3102), input->start },
		{ DOS_REG(0x3103), input->end },
		{ DOS_REG(0x3105), input->read_pointer },
		{ DOS_REG(0x3104), input->write_pointer },
	};
	u32 actual;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_write(map, regs[i].reg, regs[i].def);
		if (ret)
			return ret;
		ret = regmap_read(map, regs[i].reg, &actual);
		if (ret)
			return ret;
		if (actual != regs[i].def)
			return -EIO;
	}
	return 0;
}

int s7d_hevc_context_program_input(struct s7d_hevc_context *context,
				   struct regmap *map)
{
	int ret;

	if (!context || IS_ERR_OR_NULL(map) || !context->staged)
		return -EINVAL;
	if (context->attempted)
		return -EBUSY;
	/* A partial failure requires the parent's full halt, never a blind retry. */
	context->attempted = true;
	dma_wmb();
	ret = hevc_program_addresses(map, &context->input);
	if (!ret)
		context->programmed = true;
	return ret;
}

int s7d_hevc_context_snapshot_rpm(struct s7d_hevc_context *context,
				  void *snapshot, size_t bytes)
{
	if (!context || !snapshot || bytes != S7D_HEVC_RPM_BYTES ||
	    !context->programmed)
		return -EINVAL;
	dma_rmb();
	memcpy(snapshot, context->cpu + (context->workspace.rpm.address -
	       context->base), bytes);
	return 0;
}
