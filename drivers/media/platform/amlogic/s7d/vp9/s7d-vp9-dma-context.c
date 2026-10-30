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

#include "../common/s7d-codec-session.h"
#include "s7d-vp9-dma-context.h"
#include "s7d-vp9-input.h"

#define DOS_REG(word) ((word) << 2)
#define INPUT_PADDING SZ_16K
#define AMLV_BYTES 16U

struct s7d_vp9_dma_context {
	struct s7d_codec_session *session;
	struct s7d_codec_dma *memory;
	struct regmap *borrowed_map;
	struct s7d_vp9_workspace workspace;
	struct s7d_vp9_dma_sizes sizes;
	struct s7d_vp9_input_plan input;
	u8 *cpu;
	u32 base;
	u32 width;
	u32 height;
	u32 input_capacity;
	u64 lease_epoch;
	u64 last_token;
	bool idle;
	bool staged;
	bool begun;
	bool attempted;
	bool programmed;
	bool failed;
};

int s7d_vp9_dma_sizes(u32 width, u32 height, size_t input_capacity,
		      struct s7d_vp9_dma_sizes *sizes)
{
	struct s7d_vp9_dma_sizes next = { 0 };
	u32 columns, rows, base_bytes, compressed_bytes, padded, mv_bytes;

	if (!sizes || !input_capacity || width < 64 || width > 1920 ||
	    height < 64 || height > 1080 || ((width | height) & 1))
		return -EINVAL;
	if (input_capacity > S7D_VP9_INPUT_MAX_BYTES)
		return -E2BIG;
	columns = DIV_ROUND_UP(width, 64);
	rows = DIV_ROUND_UP(ALIGN(height, 32), 64);
	if (check_mul_overflow(columns, rows, &base_bytes) ||
	    check_mul_overflow(base_bytes, 36U * 16U, &base_bytes) ||
	    check_mul_overflow(columns, 16U * ((rows >> 3) + (rows & 7)),
			       &compressed_bytes) ||
	    check_add_overflow(base_bytes, compressed_bytes, &next.mv_used_bytes) ||
	    check_add_overflow(next.mv_used_bytes, SZ_64K - 1U, &padded))
		return -EOVERFLOW;
	next.mv_stride = round_down(padded, SZ_64K);
	next.rpm_offset = S7D_VP9_WORKSPACE_BYTES;
	next.lmem_offset = next.rpm_offset + ALIGN(S7D_VP9_RPM_BYTES, SZ_4K);
	next.mv_offset = ALIGN(next.lmem_offset + S7D_VP9_LMEM_BYTES, SZ_64K);
	if (check_mul_overflow(S7D_VP9_MV_BANKS, next.mv_stride, &mv_bytes) ||
	    check_add_overflow(next.mv_offset, mv_bytes, &next.input_offset))
		return -EOVERFLOW;
	next.ring_bytes = ALIGN((u32)input_capacity + AMLV_BYTES + INPUT_PADDING + 8,
				SZ_4K);
	if (check_add_overflow(next.input_offset, next.ring_bytes, &next.total_bytes) ||
	    check_add_overflow(next.total_bytes, SZ_64K - 1U, &next.allocation_bytes))
		return -EOVERFLOW;
	*sizes = next;
	return 0;
}

static bool vp9_dma_overlap(u64 first, u64 bytes, u64 other, u64 other_bytes)
{
	return first < other + other_bytes && other < first + bytes;
}

static int vp9_dma_layout(const struct s7d_vp9_dma_context *ctx,
			  const struct s7d_codec_layout *layout)
{
	u32 pitch = ALIGN(ctx->width, 64);
	u32 storage = ALIGN(ctx->height, 64);
	int ret;

	if (!layout)
		return -EINVAL;
	ret = s7d_codec_validate_layout(layout);
	if (ret)
		return ret;
	if ((layout->format != S7D_CODEC_CAPTURE_NV12M &&
	     layout->format != S7D_CODEC_CAPTURE_NV21M) ||
	    layout->coded_width != ctx->width || layout->coded_height != ctx->height ||
	    layout->visible_left || layout->visible_top ||
	    layout->visible_width != ctx->width || layout->visible_height != ctx->height ||
	    layout->plane_count != 2 || layout->pitch[0] != pitch ||
	    layout->pitch[1] != pitch || layout->storage_height != storage ||
	    layout->size[0] != pitch * storage || layout->size[1] != pitch * storage / 2)
		return -EINVAL;
	return 0;
}

struct s7d_vp9_dma_context *
s7d_vp9_dma_context_create_stream(struct s7d_codec_session *session,
				u32 width, u32 height, size_t input_capacity,
				u64 lease_epoch)
{
	struct s7d_vp9_region rpm, lmem;
	struct s7d_vp9_dma_context *ctx;
	struct s7d_vp9_dma_sizes sizes;
	dma_addr_t dma;
	u64 base, last;
	int ret;

	if (IS_ERR_OR_NULL(session) || !lease_epoch)
		return ERR_PTR(-EINVAL);
	ret = s7d_vp9_dma_sizes(width, height, input_capacity, &sizes);
	if (ret)
		return ERR_PTR(ret);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);
	s7d_codec_session_get(session);
	ctx->session = session;
	ctx->memory = s7d_codec_dma_alloc(session, sizes.allocation_bytes);
	if (IS_ERR(ctx->memory)) {
		ret = PTR_ERR(ctx->memory);
		goto release;
	}
	dma = s7d_codec_dma_addr(ctx->memory);
	base = ALIGN((u64)dma, (u64)SZ_64K);
	last = base + sizes.total_bytes;
	if (!base || base > U32_MAX || last > U32_MAX || base < dma ||
	    base - dma + sizes.total_bytes > s7d_codec_dma_size(ctx->memory)) {
		ret = -ERANGE;
		goto release;
	}
	rpm.address = base + sizes.rpm_offset;
	rpm.bytes = S7D_VP9_RPM_BYTES;
	lmem.address = base + sizes.lmem_offset;
	lmem.bytes = S7D_VP9_LMEM_BYTES;
	ret = s7d_vp9_workspace_plan(base, S7D_VP9_WORKSPACE_BYTES,
				     &rpm, &lmem, &ctx->workspace);
	if (ret)
		goto release;
	if (ctx->workspace.used != S7D_VP9_WORKSPACE_BYTES) {
		ret = -EINVAL;
		goto release;
	}
	ctx->sizes = sizes;
	ctx->base = base;
	ctx->cpu = (u8 *)s7d_codec_dma_cpu(ctx->memory) + (base - dma);
	ctx->width = width;
	ctx->height = height;
	ctx->input_capacity = input_capacity;
	ctx->lease_epoch = lease_epoch;
	/* Preserve these workspace contents across every subsequent input. */
	memset(ctx->cpu, 0, sizes.total_bytes);
	dma_wmb();
	return ctx;

release:
	/* Accepted allocations remain core-owned until verified session stop. */
	s7d_codec_session_put(session);
	kfree(ctx);
	return ERR_PTR(ret);
}

int s7d_vp9_dma_context_destroy_stream(struct s7d_vp9_dma_context *context,
				     u64 lease_epoch)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch)
		return -ESTALE;
	if (!context->idle || context->begun || context->staged)
		return -EBUSY;
	s7d_codec_session_put(context->session);
	kfree(context);
	return 0;
}

const struct s7d_vp9_workspace *
s7d_vp9_dma_context_workspace(const struct s7d_vp9_dma_context *context)
{
	return IS_ERR_OR_NULL(context) ? NULL : &context->workspace;
}

int s7d_vp9_dma_context_mv_slot(const struct s7d_vp9_dma_context *context,
			      u64 lease_epoch, u32 bank,
			      struct s7d_vp9_region *region)
{
	struct s7d_vp9_region next;
	u64 first, last;

	if (IS_ERR_OR_NULL(context) || !region)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch)
		return -ESTALE;
	if (bank >= S7D_VP9_MV_BANKS)
		return -ERANGE;
	first = (u64)context->base + context->sizes.mv_offset +
		(u64)bank * context->sizes.mv_stride;
	last = first + context->sizes.mv_stride;
	if (!IS_ALIGNED(first, SZ_64K) || last > U32_MAX ||
	    last > (u64)context->base + context->sizes.input_offset)
		return -ERANGE;
	next.address = first;
	/* The compressed-MV reader's exclusive end uses the allocated stride. */
	next.bytes = context->sizes.mv_stride;
	*region = next;
	return 0;
}

int s7d_vp9_dma_context_bind_surface(const struct s7d_vp9_dma_context *context,
				   u64 lease_epoch,
				   struct s7d_codec_surface *surface,
				   const struct s7d_codec_layout *layout,
				   struct s7d_vp9_region region[2])
{
	struct s7d_vp9_region next[2];
	dma_addr_t address, allocation;
	size_t bytes, allocation_bytes;
	u32 i;
	int ret;

	if (IS_ERR_OR_NULL(context) || IS_ERR_OR_NULL(surface) || !region)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch)
		return -ESTALE;
	ret = vp9_dma_layout(context, layout);
	if (ret)
		return ret;
	allocation = s7d_codec_dma_addr(context->memory);
	allocation_bytes = s7d_codec_dma_size(context->memory);
	for (i = 0; i < 2; i++) {
		ret = s7d_codec_surface_dma(surface, i, &address, &bytes);
		if (ret)
			return ret;
		if (!address || address > U32_MAX || !IS_ALIGNED(address, 32) ||
		    bytes < layout->size[i] ||
		    (u64)address + layout->size[i] > (1ULL << 32) ||
		    vp9_dma_overlap(address, layout->size[i], allocation,
				    allocation_bytes))
			return -ERANGE;
		next[i].address = address;
		next[i].bytes = layout->size[i];
	}
	if (vp9_dma_overlap(next[0].address, next[0].bytes,
			    next[1].address, next[1].bytes))
		return -EINVAL;
	memcpy(region, next, sizeof(next));
	return 0;
}

int s7d_vp9_dma_context_stage(struct s7d_vp9_dma_context *context,
			     const struct s7d_vp9_input_result *input,
			     u64 lease_epoch, u64 token,
			     struct s7d_vp9_input_plan *plan)
{
	struct s7d_vp9_input_plan next;
	u8 *ring;
	int ret;

	if (IS_ERR_OR_NULL(context) || !input || !input->data || !input->staging ||
	    !input->bytes || !token || !plan ||
	    input->bytes > S7D_VP9_INPUT_MAX_BYTES ||
	    input->decode_bytes != input->bytes + AMLV_BYTES)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || token <= context->last_token)
		return -ESTALE;
	if (context->failed)
		return -EIO;
	if (!context->idle || context->staged || context->begun || context->attempted)
		return -EBUSY;
	if (input->bytes > context->input_capacity)
		return -E2BIG;
	ret = vp9_dma_layout(context, &input->layout);
	if (ret)
		return ret;
	if (input->prefix.width != context->width ||
	    input->prefix.height != context->height ||
	    input->prefix.render_width != context->width ||
	    input->prefix.render_height != context->height ||
	    input->prefix.show_existing)
		return -EINVAL;
	ring = context->cpu + context->sizes.input_offset;
	memcpy(ring, input->staging, input->decode_bytes);
	memset(ring + input->decode_bytes, 0,
	       context->sizes.ring_bytes - input->decode_bytes);
	next.start = context->base + context->sizes.input_offset;
	next.end = next.start + context->sizes.ring_bytes;
	next.read_pointer = next.start;
	next.write_pointer = next.start +
		round_down(input->decode_bytes + INPUT_PADDING, 8);
	next.decode_size = input->decode_bytes;
	next.max_shift_bytes = input->decode_bytes + 8;
	next.token = token;
	dma_wmb();
	context->input = next;
	context->last_token = token;
	context->staged = true;
	*plan = next;
	return 0;
}

int s7d_vp9_dma_context_begin(struct s7d_vp9_dma_context *context,
			     u64 lease_epoch, u64 token)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || context->input.token != token)
		return -ESTALE;
	if (context->failed)
		return -EIO;
	if (!context->idle || !context->staged || context->begun)
		return -EBUSY;
	context->idle = false;
	context->begun = true;
	return 0;
}

static int vp9_program_addresses(struct regmap *map,
				 const struct s7d_vp9_input_plan *input)
{
	const struct reg_sequence regs[] = {
		{ DOS_REG(0x3102), input->start },
		{ DOS_REG(0x3103), input->end },
		{ DOS_REG(0x3105), input->read_pointer },
		{ DOS_REG(0x3104), input->write_pointer },
	};
	u32 actual;
	u32 i;
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

int s7d_vp9_dma_context_program_input(struct s7d_vp9_dma_context *context,
				     struct regmap *map,
				     u64 lease_epoch, u64 token)
{
	int ret;

	if (IS_ERR_OR_NULL(context) || IS_ERR_OR_NULL(map))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || context->input.token != token)
		return -ESTALE;
	if (!context->staged || !context->begun || context->attempted)
		return -EBUSY;
	if (context->borrowed_map && context->borrowed_map != map)
		return -ESTALE;
	context->borrowed_map = map;
	context->attempted = true;
	dma_wmb();
	ret = vp9_program_addresses(map, &context->input);
	if (!ret)
		context->programmed = true;
	return ret;
}

int s7d_vp9_dma_context_snapshot_rpm(struct s7d_vp9_dma_context *context,
				    u64 lease_epoch, u64 token,
				    struct s7d_vp9_rpm *rpm)
{
	if (IS_ERR_OR_NULL(context) || !rpm)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || context->input.token != token)
		return -ESTALE;
	if (!context->begun || !context->programmed)
		return -EBUSY;
	dma_rmb();
	return s7d_vp9_rpm_snapshot(context->cpu + context->sizes.rpm_offset,
				   S7D_VP9_RPM_BYTES, true, rpm);
}

int s7d_vp9_dma_context_mark_idle(struct s7d_vp9_dma_context *context,
				 u64 lease_epoch, u64 token, int halt_status,
				 bool irq_joined, bool decoded)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || context->input.token != token)
		return -ESTALE;
	if (halt_status || !irq_joined)
		return halt_status < 0 ? halt_status : halt_status ? -EIO : -EBUSY;
	if (decoded && (!context->begun || !context->programmed))
		return -EINVAL;
	if (context->idle && !context->staged)
		return -EALREADY;
	if (context->begun && !decoded)
		context->failed = true;
	context->idle = true;
	context->staged = false;
	context->begun = false;
	context->attempted = false;
	context->programmed = false;
	return 0;
}
