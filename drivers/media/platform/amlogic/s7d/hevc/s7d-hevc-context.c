// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../common/s7d-codec-session.h"
#include "s7d-hevc-context.h"
#include "s7d-hevc-rpm.h"
#include "s7d-hevc-sequence.h"

#define DOS_REG(word) ((word) << 2)
#define INPUT_PADDING SZ_16K
#define AUX_PART_BYTES (12U * SZ_1K)

struct s7d_hevc_context {
	struct s7d_codec_session *session;
	struct s7d_codec_dma *memory;
	struct regmap *borrowed_map;
	struct s7d_hevc_workspace workspace;
	struct s7d_hevc_sps host;
	struct s7d_hevc_input_plan input;
	u8 *cpu;
	u32 base;
	u32 input_offset;
	u32 input_capacity;
	u32 ring_bytes;
	u32 last_input_bytes;
	u32 slot_count;
	u32 mv_stride;
	u32 mv_used_bytes;
	u64 lease_epoch;
	u64 last_token;
	bool idle;
	bool begun;
	bool staged;
	bool attempted;
	bool programmed;
};

static void hevc_region(u32 *offset, u32 bytes, u32 alignment,
			struct s7d_hevc_dma_region *region)
{
	*offset = ALIGN(*offset, alignment);
	region->address = *offset;
	region->bytes = bytes;
	*offset += bytes;
}

/* Fixed spec[5] capacities/order; MMU regions remain reserved and unused. */
static u32 hevc_workspace_layout(struct s7d_hevc_workspace *w, u32 mv_bytes)
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
	hevc_region(&offset, mv_bytes, SZ_64K, &w->mv);
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

static int hevc_mv_layout(const struct s7d_hevc_sps *sps,
			  u32 *used, u32 *stride)
{
	u32 unit, columns, rows, ctbs, padded;

	unit = sps->log2_ctb_size == 6 ? 512 :
	       sps->log2_ctb_size == 5 ? 128 : 32;
	columns = DIV_ROUND_UP(sps->coded_width, 1U << sps->log2_ctb_size);
	rows = DIV_ROUND_UP(sps->coded_height, 1U << sps->log2_ctb_size);
	if (check_mul_overflow(columns, rows, &ctbs) ||
	    check_mul_overflow(ctbs, unit, used) ||
	    check_add_overflow(*used, SZ_64K - 1U, &padded))
		return -EOVERFLOW;
	*stride = round_down(padded, SZ_64K);
	return *used && *stride ? 0 : -EINVAL;
}

static int hevc_context_ranges(const struct s7d_hevc_context *ctx, u32 total)
{
	const struct s7d_hevc_workspace *w = &ctx->workspace;
	const struct s7d_hevc_dma_region *region[] = {
		&w->ipp, &w->sao_abv, &w->sao_vb, &w->short_term_rps,
		&w->vps, &w->sps, &w->pps, &w->sao_up, &w->swap1,
		&w->swap2, &w->scalelut, &w->dblk_para, &w->dblk_data,
		&w->dblk_data2, &w->mpred_above, &w->rpm, &w->lmem,
		&w->aux, &w->mv,
	};
	u64 input = (u64)ctx->base + ctx->input_offset;
	u64 end = (u64)ctx->base + total;
	u32 i, j;

	if (!ctx->base || end > U32_MAX ||
	    input + ctx->ring_bytes != end || !IS_ALIGNED(input, SZ_4K))
		return -ERANGE;
	for (i = 0; i < ARRAY_SIZE(region); i++) {
		u64 first = region[i]->address;
		u64 last = first + region[i]->bytes;

		if (first < ctx->base || last > input ||
		    !IS_ALIGNED(first, SZ_4K))
			return -ERANGE;
		if (!region[i]->bytes)
			continue;
		for (j = 0; j < i; j++)
			if (region[j]->bytes && first <
			    (u64)region[j]->address + region[j]->bytes &&
			    region[j]->address < last)
				return -EINVAL;
	}
	if (!IS_ALIGNED(w->mv.address, SZ_64K) ||
	    (u64)ctx->slot_count * ctx->mv_stride != w->mv.bytes ||
	    ctx->mv_used_bytes > ctx->mv_stride)
		return -ERANGE;
	return 0;
}

struct s7d_hevc_context *
s7d_hevc_context_create_stream(struct s7d_codec_session *session,
		    const struct s7d_hevc_sps *sps, size_t input_capacity,
		    u32 slot_count, u64 lease_epoch)
{
	struct s7d_codec_layout layout;
	struct s7d_hevc_context *ctx;
	dma_addr_t dma;
	u64 base, last;
	u32 total, mv_bytes, allocation;
	int ret;

	if (IS_ERR_OR_NULL(session) || !sps || !input_capacity)
		return ERR_PTR(-EINVAL);
	if (input_capacity > S7D_HEVC_INPUT_MAX_BYTES)
		return ERR_PTR(-E2BIG);
	ret = s7d_hevc_build_layout(sps, S7D_CODEC_CAPTURE_NV12M, 0, &layout);
	if (ret)
		return ERR_PTR(ret);
	if (sps->max_sub_layers != 1 || sps->log2_ctb_size < 4 ||
	    sps->log2_ctb_size > 6)
		return ERR_PTR(-EOPNOTSUPP);
	if (!lease_epoch || slot_count < layout.min_capture_buffers ||
	    slot_count > S7D_HEVC_MAX_CAPTURE_BUFFERS)
		return ERR_PTR(-EINVAL);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);
	ctx->host = *sps;
	ctx->input_capacity = input_capacity;
	ctx->slot_count = slot_count;
	ctx->lease_epoch = lease_epoch;
	ret = hevc_mv_layout(sps, &ctx->mv_used_bytes, &ctx->mv_stride);
	if (ret)
		goto free;
	if (ctx->mv_used_bytes > ctx->mv_stride ||
	    check_mul_overflow(slot_count, ctx->mv_stride, &mv_bytes)) {
		ret = -EOVERFLOW;
		goto free;
	}
	ctx->input_offset = hevc_workspace_layout(&ctx->workspace, mv_bytes);
	/* One read-ahead element remains before the exclusive ring end. */
	ctx->ring_bytes = ALIGN(input_capacity + INPUT_PADDING + 8, SZ_4K);
	if (check_add_overflow(ctx->input_offset, ctx->ring_bytes, &total) ||
	    check_add_overflow(total, SZ_64K - 1U, &allocation)) {
		ret = -EOVERFLOW;
		goto free;
	}
	s7d_codec_session_get(session);
	ctx->session = session;
	ctx->memory = s7d_codec_dma_alloc(session, allocation);
	if (IS_ERR(ctx->memory)) {
		ret = PTR_ERR(ctx->memory);
		goto release;
	}
	dma = s7d_codec_dma_addr(ctx->memory);
	base = ALIGN((u64)dma, (u64)SZ_64K);
	last = base + total;
	if (base > U32_MAX || last > U32_MAX || base < dma ||
	    base - dma + total > s7d_codec_dma_size(ctx->memory)) {
		ret = -ERANGE;
		goto release;
	}
	ctx->base = base;
	ctx->cpu = (u8 *)s7d_codec_dma_cpu(ctx->memory) + (base - dma);
	hevc_workspace_relocate(&ctx->workspace, ctx->base);
	ret = hevc_context_ranges(ctx, total);
	if (ret)
		goto release;
	memset(ctx->cpu, 0, total);
	dma_wmb();
	return ctx;

release:
	/* Accepted DMA stays with the core until verified session stop. */
	s7d_codec_session_put(session);
free:
	kfree(ctx);
	return ERR_PTR(ret);
}

int s7d_hevc_context_destroy_stream(struct s7d_hevc_context *context,
				  u64 lease_epoch)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch)
		return -ESTALE;
	if (!context->idle || context->begun)
		return -EBUSY;
	s7d_codec_session_put(context->session);
	kfree(context);
	return 0;
}

int s7d_hevc_context_mv_slot(const struct s7d_hevc_context *context,
			   u64 lease_epoch, u32 slot,
			   struct s7d_hevc_dma_region *region)
{
	struct s7d_hevc_dma_region next;
	u64 start, end;

	if (IS_ERR_OR_NULL(context) || !region)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch)
		return -ESTALE;
	if (slot >= context->slot_count)
		return -ERANGE;
	start = (u64)context->workspace.mv.address +
		(u64)slot * context->mv_stride;
	end = start + context->mv_used_bytes;
	if (!IS_ALIGNED(start, SZ_64K) || end > U32_MAX ||
	    end > (u64)context->workspace.mv.address + context->workspace.mv.bytes)
		return -ERANGE;
	next.address = start;
	next.bytes = context->mv_used_bytes;
	*region = next;
	return 0;
}

int s7d_hevc_context_mark_idle(struct s7d_hevc_context *context,
			     u64 lease_epoch, u64 input_token,
			     int halt_status, bool irq_joined)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch ||
	    context->input.token != input_token)
		return -ESTALE;
	if (context->idle)
		return -EALREADY;
	if (halt_status || !irq_joined)
		return halt_status < 0 ? halt_status : halt_status ? -EIO : -EBUSY;
	context->idle = true;
	context->begun = false;
	context->staged = false;
	context->attempted = false;
	context->programmed = false;
	return 0;
}

int s7d_hevc_context_begin(struct s7d_hevc_context *context,
			  u64 lease_epoch, u64 input_token)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch ||
	    context->input.token != input_token)
		return -ESTALE;
	if (!context->idle || !context->staged || context->begun)
		return -EBUSY;
	context->idle = false;
	context->begun = true;
	return 0;
}

const struct s7d_hevc_workspace *
s7d_hevc_context_workspace(const struct s7d_hevc_context *context)
{
	return context ? &context->workspace : NULL;
}

void s7d_hevc_context_report_dma(const struct s7d_hevc_context *context,
				struct device *dev)
{
	const struct s7d_hevc_workspace *w = &context->workspace;
	dma_addr_t allocation = s7d_codec_dma_addr(context->memory);
	u32 input = context->base + context->input_offset;

	dev_err(dev,
		"HEVC context allocation=%pad+%zx workspace=[%#x,%#x) ring=[%#x,%#llx)\n",
		&allocation, s7d_codec_dma_size(context->memory), context->base,
		input, input, (u64)input + context->ring_bytes);
	dev_err(dev,
		"HEVC context RPM=%#x+%#x LMEM=%#x+%#x AUX=%#x+%#x MV=%#x+%#x\n",
		w->rpm.address, w->rpm.bytes, w->lmem.address, w->lmem.bytes,
		w->aux.address, w->aux.bytes, w->mv.address, w->mv.bytes);
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
	       a->max_latency_increase_plus1 == b->max_latency_increase_plus1 &&
	       a->separate_colour_plane == b->separate_colour_plane &&
	       a->interlaced_source == b->interlaced_source &&
	       a->field_seq == b->field_seq &&
	       a->colour.primaries == b->colour.primaries &&
	       a->colour.transfer == b->colour.transfer &&
	       a->colour.matrix == b->colour.matrix &&
	       a->colour.flags == b->colour.flags;
}

int s7d_hevc_context_stage_au(struct s7d_hevc_context *context,
			    const struct s7d_hevc_context_au *au,
			    struct s7d_hevc_input_plan *plan)
{
	struct s7d_hevc_input_plan next;
	u8 *input;

	if (IS_ERR_OR_NULL(context) || !au ||
	    !au->data || !au->sps || !au->bytes || !au->input_token || !plan)
		return -EINVAL;
	if (context->lease_epoch != au->lease_epoch ||
	    au->input_token <= context->last_token)
		return -ESTALE;
	if (!context->idle || context->staged || context->begun ||
	    context->attempted)
		return -EBUSY;
	if (au->bytes > context->input_capacity)
		return -E2BIG;
	if (!hevc_same_sps(au->sps, &context->host))
		return -ESTALE;
	input = context->cpu + context->input_offset;
	memcpy(input, au->data, au->bytes);
	/* Creation and previous staging leave the unused ring tail zero. */
	if (au->bytes < context->last_input_bytes)
		memset(input + au->bytes, 0, context->last_input_bytes - au->bytes);
	context->last_input_bytes = au->bytes;
	next.start = context->base + context->input_offset;
	next.end = next.start + context->ring_bytes;
	next.read_pointer = next.start;
	next.write_pointer = next.start + round_down(au->bytes + INPUT_PADDING, 8);
	next.decode_size = au->bytes;
	next.max_shift_bytes = au->bytes + 8;
	next.token = au->input_token;
	dma_wmb();
	context->input = next;
	context->last_token = au->input_token;
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
	if (context->attempted || !context->begun)
		return -EBUSY;
	if (context->borrowed_map &&
	    context->borrowed_map != map)
		return -ESTALE;
	context->borrowed_map = map;
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
