// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../common/s7d-codec-session.h"
#include "s7d-av1-dma-context.h"

#define AV1_INPUT_PADDING SZ_16K
#define DOS_REG(word) ((word) << 2)
#define AV1_MAX_STAGING_BYTES (S7D_AV1_INPUT_MAX_BYTES * 11U)

struct s7d_av1_dma_context {
	struct s7d_codec_session *session;
	struct s7d_codec_dma *memory;
	struct regmap *map;
	struct s7d_av1_workspace workspace;
	struct s7d_av1_dma_sizes sizes;
	struct s7d_av1_input_plan input;
	u8 *cpu;
	u32 base;
	u32 staging_capacity;
	u64 lease_epoch;
	u64 last_token;
	bool idle;
	bool staged;
	bool begun;
	bool attempted;
	bool programmed;
	bool failed;
};

int s7d_av1_dma_sizes(u32 width, u32 height, size_t staging_capacity,
		      struct s7d_av1_dma_sizes *sizes)
{
	struct s7d_av1_dma_sizes next = { 0 };
	u32 padded;
	int ret;

	if (!sizes || !staging_capacity)
		return -EINVAL;
	if (staging_capacity > AV1_MAX_STAGING_BYTES)
		return -E2BIG;
	ret = s7d_av1_workspace_size(width, height, &next.workspace_bytes);
	if (ret)
		return ret;
	next.rpm_offset = next.workspace_bytes;
	next.lmem_offset = next.rpm_offset + SZ_4K;
	next.mmu_map_offset = next.lmem_offset + SZ_4K;
	next.input_offset = next.mmu_map_offset +
		ALIGN(S7D_AV1_MMU_MAP_BYTES, SZ_4K);
	if (check_add_overflow((u32)staging_capacity, AV1_INPUT_PADDING + 8,
			       &padded) ||
	    check_add_overflow(padded, SZ_4K - 1U, &padded))
		return -EOVERFLOW;
	next.ring_bytes = round_down(padded, SZ_4K);
	if (check_add_overflow(next.input_offset, next.ring_bytes, &next.total_bytes) ||
	    check_add_overflow(next.total_bytes, SZ_64K - 1U, &next.allocation_bytes))
		return -EOVERFLOW;
	*sizes = next;
	return 0;
}

struct s7d_av1_dma_context *
s7d_av1_dma_context_create(struct s7d_codec_session *session,
			   u32 width, u32 height, size_t staging_capacity,
			   u64 lease_epoch)
{
	struct s7d_av1_dma_context *ctx;
	struct s7d_av1_dma_sizes sizes;
	struct s7d_av1_region rpm, lmem;
	dma_addr_t dma;
	u64 base;
	int ret;

	if (IS_ERR_OR_NULL(session) || !lease_epoch)
		return ERR_PTR(-EINVAL);
	ret = s7d_av1_dma_sizes(width, height, staging_capacity, &sizes);
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
	if (!base || base > U32_MAX || base + sizes.total_bytes > U32_MAX ||
	    base < dma || base - dma + sizes.total_bytes >
	    s7d_codec_dma_size(ctx->memory)) {
		ret = -ERANGE;
		goto release;
	}
	rpm.address = base + sizes.rpm_offset;
	rpm.bytes = S7D_AV1_RPM_BYTES;
	lmem.address = base + sizes.lmem_offset;
	lmem.bytes = S7D_AV1_LMEM_BYTES;
	ret = s7d_av1_workspace_plan(base, sizes.workspace_bytes, width, height,
				    &rpm, &lmem, &ctx->workspace);
	if (ret)
		goto release;
	ctx->sizes = sizes;
	ctx->base = base;
	ctx->cpu = (u8 *)s7d_codec_dma_cpu(ctx->memory) + (base - dma);
	ctx->staging_capacity = staging_capacity;
	ctx->lease_epoch = lease_epoch;
	memset(ctx->cpu, 0, sizes.total_bytes);
	dma_wmb();
	return ctx;

release:
	/* DMA allocations remain session-owned until the core proves full stop. */
	s7d_codec_session_put(session);
	kfree(ctx);
	return ERR_PTR(ret);
}

int s7d_av1_dma_context_destroy(struct s7d_av1_dma_context *context,
			       u64 lease_epoch)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch)
		return -ESTALE;
	if (!context->idle || context->staged || context->begun)
		return -EBUSY;
	s7d_codec_session_put(context->session);
	kfree(context);
	return 0;
}

const struct s7d_av1_workspace *
s7d_av1_dma_context_workspace(const struct s7d_av1_dma_context *context)
{
	return IS_ERR_OR_NULL(context) ? NULL : &context->workspace;
}

int s7d_av1_dma_context_mmu_region(const struct s7d_av1_dma_context *context,
				  u64 lease_epoch,
				  struct s7d_av1_region *region)
{
	if (IS_ERR_OR_NULL(context) || !region)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch)
		return -ESTALE;
	region->address = context->base + context->sizes.mmu_map_offset;
	region->bytes = S7D_AV1_MMU_MAP_BYTES;
	return 0;
}

int s7d_av1_dma_context_stage(struct s7d_av1_dma_context *context,
			     const u8 *data, size_t bytes,
			     u64 lease_epoch, u64 token,
			     struct s7d_av1_input_plan *plan)
{
	struct s7d_av1_input_plan next;
	struct s7d_av1_transport transport;
	u8 *ring;
	int ret;

	if (IS_ERR_OR_NULL(context) || !plan || !token)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || token <= context->last_token)
		return -ESTALE;
	if (context->failed)
		return -EIO;
	if (!context->idle || context->staged || context->begun || context->attempted)
		return -EBUSY;
	ring = context->cpu + context->sizes.input_offset;
	ret = s7d_av1_transport_write(data, bytes, ring, context->staging_capacity,
				     &transport);
	if (ret)
		return ret;
	memset(ring + transport.staged_bytes, 0,
	       context->sizes.ring_bytes - transport.staged_bytes);
	next.start = context->base + context->sizes.input_offset;
	next.end = next.start + context->sizes.ring_bytes;
	next.read_pointer = next.start;
	next.write_pointer = next.start +
		round_down(transport.staged_bytes + AV1_INPUT_PADDING, 8);
	next.decode_size = transport.staged_bytes;
	next.max_shift_bytes = next.write_pointer - next.start;
	next.token = token;
	dma_wmb();
	context->input = next;
	context->last_token = token;
	context->staged = true;
	*plan = next;
	return 0;
}

int s7d_av1_dma_context_begin(struct s7d_av1_dma_context *context,
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

static int av1_program_addresses(struct regmap *map,
				 const struct s7d_av1_input_plan *input)
{
	const struct reg_sequence regs[] = {
		{ DOS_REG(0x3102), input->start },
		{ DOS_REG(0x3103), input->end },
		{ DOS_REG(0x3105), input->read_pointer },
		{ DOS_REG(0x3104), input->write_pointer },
	};
	u32 actual, i;
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

int s7d_av1_dma_context_program_input(struct s7d_av1_dma_context *context,
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
	if (context->map && context->map != map)
		return -ESTALE;
	context->map = map;
	context->attempted = true;
	dma_wmb();
	ret = av1_program_addresses(map, &context->input);
	if (!ret)
		context->programmed = true;
	return ret;
}

int s7d_av1_dma_context_map_body(struct s7d_av1_dma_context *context,
				const struct s7d_av1_region *body, u32 pages,
				u64 lease_epoch, u64 token)
{
	u64 first, last;
	int ret;

	if (IS_ERR_OR_NULL(context) || !body)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || context->input.token != token)
		return -ESTALE;
	if (!context->begun || !context->programmed)
		return -EBUSY;
	first = s7d_codec_dma_addr(context->memory);
	last = first + s7d_codec_dma_size(context->memory);
	if ((u64)body->address < last && first < (u64)body->address + body->bytes)
		return -EINVAL;
	ret = s7d_av1_mmu_map_contiguous(body, pages,
		context->cpu + context->sizes.mmu_map_offset, S7D_AV1_MMU_MAP_BYTES);
	if (ret)
		return ret;
	dma_wmb();
	return 0;
}

int s7d_av1_dma_context_snapshot_rpm(struct s7d_av1_dma_context *context,
				    u64 lease_epoch, u64 token, bool efficiency,
				    struct s7d_av1_rpm *rpm)
{
	if (IS_ERR_OR_NULL(context) || !rpm)
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || context->input.token != token)
		return -ESTALE;
	if (!context->begun || !context->programmed)
		return -EBUSY;
	dma_rmb();
	return s7d_av1_rpm_snapshot(context->cpu + context->sizes.rpm_offset,
				   S7D_AV1_RPM_BYTES, efficiency, rpm);
}

int s7d_av1_dma_context_mark_idle(struct s7d_av1_dma_context *context,
				 u64 lease_epoch, u64 token, int halt_status,
				 bool joined, bool completed)
{
	if (IS_ERR_OR_NULL(context))
		return -EINVAL;
	if (context->lease_epoch != lease_epoch || context->input.token != token)
		return -ESTALE;
	if (halt_status || !joined)
		return halt_status < 0 ? halt_status : halt_status ? -EIO : -EBUSY;
	if (completed && (!context->begun || !context->programmed))
		return -EINVAL;
	if (context->idle && !context->staged)
		return -EALREADY;
	if (context->begun && !completed)
		context->failed = true;
	context->idle = true;
	context->staged = false;
	context->begun = false;
	context->attempted = false;
	context->programmed = false;
	return 0;
}
