// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include "s7d-h264-context.h"
#include "s7d-vdec-runtime.h"

#define DOS_REG(word)		((word) * 4)
#define WORKSPACE_BYTES		0x228080U
#define WORKSPACE_ALIGNMENT	SZ_64K
#define WORKSPACE_BIAS		SZ_64K
#define LMEM_BYTES		SZ_4K
#define AUX_PREFIX_BYTES		SZ_16K
#define MV_BYTES_PER_MB		96U
#define H264_SLICE_DONE		1U
#define H264_PICTURE_DONE		2U
#define H264_CONFIG_REQUEST	0x11U
#define H264_ACTION_DECODE_START	0xffU

struct s7d_h264_slice_context {
	u32 iqidct;
	u32 vcop;
	u32 rv_ai;
	u32 vld;
};

struct s7d_h264_saved_context {
	struct s7d_h264_slice_context slice;
	u64 input_token;
	u64 write_token;
	u64 park_epoch;
	u32 scratch_f;
	u32 completed_pictures;
};

struct s7d_h264_context {
	struct s7d_codec_session *session;
	struct s7d_codec_dma *workspace;
	struct s7d_codec_dma *lmem;
	struct s7d_codec_dma *aux;
	struct s7d_codec_dma *mv;
	struct s7d_h264_mv_layout mv_layout;
	struct s7d_codec_layout layout;
	struct s7d_h264_slice_context slice;
	struct s7d_h264_saved_context saved;
	u64 input_token;
	u64 write_token;
	u64 last_park_epoch;
	u32 sequence_info;
	u32 buffer_counts;
	u32 nal_control;
	u32 cache_width;
	u32 completed_pictures;
	u32 workspace_bias;
	u32 lmem_address;
	u32 aux_address;
	bool initialize_attempted;
	bool initialized;
	bool sequence_known;
	bool slice_valid;
	bool completion_pending;
	bool park_confirmed;
	bool faulted;
};

static int s7d_context_region(struct s7d_codec_dma *memory, size_t bytes,
			      size_t alignment, u32 *address)
{
	dma_addr_t dma = s7d_codec_dma_addr(memory);
	dma_addr_t aligned;
	size_t offset;

	if (check_add_overflow(dma, (dma_addr_t)alignment - 1, &aligned))
		return -EOVERFLOW;
	aligned &= ~((dma_addr_t)alignment - 1);
	offset = aligned - dma;
	if (offset > s7d_codec_dma_size(memory) ||
	    bytes > s7d_codec_dma_size(memory) - offset ||
	    aligned > U32_MAX || bytes - 1 > U32_MAX - aligned)
		return -ERANGE;
	*address = aligned;
	return 0;
}

struct s7d_h264_context *
s7d_h264_context_create(struct s7d_codec_session *session)
{
	struct s7d_h264_context *context;
	u32 workspace_address;
	int ret;

	if (!session)
		return ERR_PTR(-EINVAL);
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return ERR_PTR(-ENOMEM);
	s7d_codec_session_get(session);
	context->session = session;
	context->workspace = s7d_codec_dma_alloc(session,
			WORKSPACE_BYTES + WORKSPACE_ALIGNMENT - 1);
	if (IS_ERR(context->workspace)) {
		ret = PTR_ERR(context->workspace);
		goto release;
	}
	ret = s7d_context_region(context->workspace, WORKSPACE_BYTES,
				 WORKSPACE_ALIGNMENT, &workspace_address);
	if (ret)
		goto release;
	if (check_add_overflow(workspace_address, WORKSPACE_BIAS,
			       &context->workspace_bias)) {
		ret = -EOVERFLOW;
		goto release;
	}
	context->lmem = s7d_codec_dma_alloc(session, LMEM_BYTES);
	if (IS_ERR(context->lmem)) {
		ret = PTR_ERR(context->lmem);
		goto release;
	}
	ret = s7d_context_region(context->lmem, LMEM_BYTES, LMEM_BYTES,
				 &context->lmem_address);
	if (ret)
		goto release;
	context->aux = s7d_codec_dma_alloc(session, AUX_PREFIX_BYTES);
	if (IS_ERR(context->aux)) {
		ret = PTR_ERR(context->aux);
		goto release;
	}
	ret = s7d_context_region(context->aux, AUX_PREFIX_BYTES, 16,
				 &context->aux_address);
	if (ret)
		goto release;
	memset(s7d_codec_dma_cpu(context->workspace), 0,
	       s7d_codec_dma_size(context->workspace));
	memset(s7d_codec_dma_cpu(context->lmem), 0, LMEM_BYTES);
	memset(s7d_codec_dma_cpu(context->aux), 0, AUX_PREFIX_BYTES);
	dma_wmb();
	return context;

release:
	s7d_codec_session_put(session);
	kfree(context);
	return ERR_PTR(ret);
}

void s7d_h264_context_release(struct s7d_h264_context *context)
{
	s7d_codec_session_put(context->session);
	kfree(context);
}

struct s7d_codec_dma *s7d_h264_context_lmem(struct s7d_h264_context *context)
{
	return context->lmem;
}

static int s7d_context_write(struct regmap *map, u32 reg, u32 value)
{
	u32 actual;
	int ret;

	ret = regmap_write(map, reg, value);
	if (ret)
		return ret;
	ret = regmap_read(map, reg, &actual);
	if (ret)
		return ret;
	return actual == value ? 0 : -EIO;
}

int s7d_h264_context_initialize(struct s7d_h264_context *context,
				struct regmap *map)
{
	const struct reg_sequence registers[] = {
		{ DOS_REG(0x0911), 0 },
		{ DOS_REG(0x09b8), 0x404038aa },
		{ DOS_REG(0x09c0), 0 },
		{ DOS_REG(0x09d3), 0 },
		{ DOS_REG(0x09d2), 0 },
		{ DOS_REG(0x09c8), context->workspace_bias },
		{ DOS_REG(0x09cf), BIT(4) },
		{ DOS_REG(0x09d5), context->lmem_address },
		{ DOS_REG(0x09d6), 1 },
		{ DOS_REG(0x09d7), 0 },
		{ DOS_REG(0x09cc), context->aux_address },
		{ DOS_REG(0x09d1), (AUX_PREFIX_BYTES >> 4) << 16 },
		{ DOS_REG(0x09c4), 1 },
		{ DOS_REG(0x09c5), 0 },
		{ DOS_REG(0x09c3), 0 },
		{ DOS_REG(0x09c2), 0 },
		{ DOS_REG(0x09c9), BIT(16) | BIT(2) | BIT(0) },
		{ DOS_REG(0x09d4), 0 },
	};
	u32 value;
	unsigned int i;
	int ret;

	if (!map)
		return -EINVAL;
	if (context->initialize_attempted)
		return -EALREADY;
	context->initialize_attempted = true;
	dma_wmb();
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = s7d_context_write(map, registers[i].reg, registers[i].def);
		if (ret)
			goto fault;
	}
	ret = regmap_update_bits(map, DOS_REG(0x09e6), BIT(5), BIT(5));
	if (ret)
		goto fault;
	ret = regmap_read(map, DOS_REG(0x09e6), &value);
	if (ret)
		goto fault;
	if (!(value & BIT(5))) {
		ret = -EIO;
		goto fault;
	}
	context->initialized = true;
	return 0;
fault:
	context->faulted = true;
	return ret;
}

int s7d_h264_build_mv_layout(const struct s7d_codec_layout *layout, u32 slots,
			     struct s7d_h264_mv_layout *mv)
{
	struct s7d_h264_mv_layout candidate;
	u32 mb_total;

	if (!layout || !mv || !slots || slots > S7D_CODEC_MAX_CAPTURE_BUFFERS ||
	    !IS_ALIGNED(layout->coded_width, 16) ||
	    !IS_ALIGNED(layout->coded_height, 16) || layout->coded_width < 64 ||
	    layout->coded_height < 64 || layout->coded_width > 4096 ||
	    layout->coded_height > 4096 ||
	    (u64)layout->coded_width * layout->coded_height > 4096U * 2304U)
		return -EINVAL;
	candidate.mb_width = ALIGN(layout->coded_width / 16, 4);
	candidate.mb_height = ALIGN(layout->coded_height / 16, 4);
	candidate.slots = slots;
	if (check_mul_overflow(candidate.mb_width, candidate.mb_height, &mb_total) ||
	    check_mul_overflow(mb_total, MV_BYTES_PER_MB, &candidate.slot_bytes) ||
	    check_mul_overflow((size_t)candidate.slot_bytes, (size_t)slots,
			       &candidate.bytes))
		return -EOVERFLOW;
	*mv = candidate;
	return 0;
}

int s7d_h264_context_allocate_mv(struct s7d_h264_context *context,
				 const struct s7d_codec_layout *layout, u32 slots,
				 struct s7d_h264_mv_buffer *buffer)
{
	struct s7d_h264_mv_buffer candidate;
	size_t bytes;
	u32 address;
	int ret;

	if (!buffer)
		return -EINVAL;
	if (context->mv)
		return -EALREADY;
	ret = s7d_h264_build_mv_layout(layout, slots, &candidate.layout);
	if (ret)
		return ret;
	if (check_add_overflow(candidate.layout.bytes, (size_t)PAGE_SIZE - 1, &bytes))
		return -EOVERFLOW;
	bytes &= PAGE_MASK;
	candidate.memory = s7d_codec_dma_alloc(context->session, bytes);
	if (IS_ERR(candidate.memory))
		return PTR_ERR(candidate.memory);
	ret = s7d_context_region(candidate.memory, bytes, 64, &address);
	if (ret)
		return ret;
	candidate.dma = address;
	memset(s7d_codec_dma_cpu(candidate.memory), 0, bytes);
	dma_wmb();
	context->mv = candidate.memory;
	context->mv_layout = candidate.layout;
	*buffer = candidate;
	return 0;
}

int s7d_h264_mv_slot(const struct s7d_h264_mv_buffer *buffer, u32 slot,
		     bool direct_8x8, u32 *address, u32 *bytes)
{
	size_t offset, end;
	dma_addr_t dma;
	u32 slot_bytes;

	if (!buffer || !address || !bytes || !buffer->memory ||
	    slot >= buffer->layout.slots || !buffer->layout.slot_bytes)
		return -EINVAL;
	if (check_mul_overflow((size_t)buffer->layout.slot_bytes, (size_t)slot,
			       &offset))
		return -EOVERFLOW;
	if (direct_8x8)
		offset >>= 2;
	slot_bytes = buffer->layout.slot_bytes >> (direct_8x8 ? 2 : 0);
	if (check_add_overflow(offset, (size_t)slot_bytes, &end) ||
	    end > buffer->layout.bytes ||
	    buffer->layout.bytes > s7d_codec_dma_size(buffer->memory) ||
	    check_add_overflow(buffer->dma, (dma_addr_t)offset, &dma) ||
	    dma > U32_MAX || slot_bytes - 1 > U32_MAX - dma || !IS_ALIGNED(dma, 8))
		return -ERANGE;
	*address = dma;
	*bytes = slot_bytes;
	return 0;
}

static int s7d_context_update(struct regmap *map, u32 reg, u32 mask, u32 value)
{
	u32 actual;
	int ret;

	ret = regmap_update_bits(map, reg, mask, value);
	if (ret)
		return ret;
	ret = regmap_read(map, reg, &actual);
	if (ret)
		return ret;
	return (actual & mask) == value ? 0 : -EIO;
}

static int s7d_context_cache(struct regmap *map, u32 width)
{
	bool narrow = width <= 256;
	int ret;

	ret = s7d_context_update(map, DOS_REG(0x0e0e), BIT(16),
				 narrow ? BIT(16) : 0);
	if (ret)
		return ret;
	return s7d_context_update(map, DOS_REG(0x0e1d), GENMASK(3, 0),
				  narrow ? 0xa : 0);
}

static int s7d_context_status(struct regmap *map, u32 expected)
{
	u32 status;
	int ret = regmap_read(map, DOS_REG(0x09d3), &status);

	return ret ? ret : (status == expected ? 0 : -ESTALE);
}

int s7d_h264_context_begin(struct s7d_h264_context *context, u64 input_token)
{
	if (!input_token)
		return -EINVAL;
	if (context->faulted)
		return -EIO;
	if (context->input_token)
		return -EBUSY;
	context->input_token = input_token;
	return 0;
}

int s7d_h264_context_set_sequence(struct s7d_h264_context *context,
				  struct regmap *map,
				  const struct s7d_h264_sps *sps,
				  const struct s7d_codec_layout *layout,
				  u32 config_scratch1, u32 mv_slots,
				  u32 capture_slots)
{
	struct s7d_codec_layout validated;
	struct s7d_h264_mv_layout mv;
	u32 mb_total, width, nal, counts;
	int ret;

	if (!map || !sps || !layout)
		return -EINVAL;
	if (context->faulted)
		return -EIO;
	if (!context->initialized || !context->input_token || !context->mv ||
	    context->completion_pending || context->park_confirmed)
		return -EBUSY;
	ret = s7d_h264_build_layout(sps, layout->format, layout->extra_surfaces,
				    &validated);
	if (ret)
		return ret;
	if (memcmp(&validated, layout, sizeof(validated)) ||
	    capture_slots < validated.min_capture_buffers ||
	    capture_slots > S7D_CODEC_MAX_CAPTURE_BUFFERS ||
	    mv_slots < sps->max_num_ref_frames + 1)
		return -EINVAL;
	ret = s7d_h264_build_mv_layout(&validated, mv_slots, &mv);
	if (ret)
		return ret;
	if (memcmp(&mv, &context->mv_layout, sizeof(mv)))
		return -EINVAL;
	if (check_mul_overflow(sps->mb_width, sps->mb_height, &mb_total) ||
	    check_add_overflow(validated.visible_left, validated.visible_width,
			       &width))
		return -EOVERFLOW;
	if ((config_scratch1 & 0xff) != (sps->mb_width & 0xff) ||
	    ((config_scratch1 >> 8) & 0xffff) != mb_total)
		return -EBADMSG;
	nal = BIT(16) | BIT(2) | BIT(0) | (sps->level_idc << 7) |
	      (sps->bitstream_restriction ? BIT(15) : 0);
	counts = (mv_slots << 24) | (capture_slots << 16) | (capture_slots << 8);
	if (context->sequence_known &&
	    (context->sequence_info != config_scratch1 ||
	     context->buffer_counts != counts || context->nal_control != nal ||
	     memcmp(&context->layout, &validated, sizeof(validated))))
		return -EOPNOTSUPP;
	ret = s7d_context_status(map, H264_CONFIG_REQUEST);
	if (ret)
		return ret;
	ret = s7d_context_cache(map, width);
	if (ret)
		goto fault;
	ret = s7d_context_write(map, DOS_REG(0x09c0), counts);
	if (ret)
		goto fault;
	ret = s7d_context_status(map, H264_CONFIG_REQUEST);
	if (ret)
		goto fault;
	context->layout = validated;
	context->sequence_info = config_scratch1;
	context->buffer_counts = counts;
	context->nal_control = nal;
	context->cache_width = width;
	context->sequence_known = true;
	return 0;
fault:
	context->faulted = true;
	return ret;
}

int s7d_h264_context_save_slice(struct s7d_h264_context *context,
				struct regmap *map, u64 input_token,
				u64 write_token)
{
	struct s7d_h264_slice_context slice;
	int ret;

	if (!map || !input_token || !write_token)
		return -EINVAL;
	if (context->faulted)
		return -EIO;
	if (!context->sequence_known || context->completion_pending ||
	    context->park_confirmed || input_token != context->input_token ||
	    (context->write_token && write_token != context->write_token))
		return -ESTALE;
	ret = s7d_context_status(map, H264_SLICE_DONE);
	if (ret)
		return ret;
	ret = regmap_read(map, DOS_REG(0x0e0e), &slice.iqidct);
	if (ret)
		goto fault;
	ret = regmap_read(map, DOS_REG(0x0e00), &slice.vcop);
	if (ret)
		goto fault;
	ret = regmap_read(map, DOS_REG(0x0e0c), &slice.rv_ai);
	if (ret)
		goto fault;
	ret = regmap_read(map, DOS_REG(0x0c18), &slice.vld);
	if (ret)
		goto fault;
	ret = s7d_context_status(map, H264_SLICE_DONE);
	if (ret)
		goto fault;
	ret = s7d_context_cache(map, context->cache_width);
	if (ret)
		goto fault;
	context->slice = slice;
	context->write_token = write_token;
	context->slice_valid = true;
	return 0;
fault:
	context->faulted = true;
	return ret;
}

int s7d_h264_context_save_completion(struct s7d_h264_context *context,
				     struct regmap *map, u64 input_token,
				     u64 write_token)
{
	struct s7d_h264_saved_context saved = {
		.input_token = input_token,
		.write_token = write_token,
	};
	int ret;

	if (!map || !input_token || !write_token)
		return -EINVAL;
	if (context->faulted)
		return -EIO;
	if (!context->slice_valid || context->completion_pending ||
	    context->park_confirmed || input_token != context->input_token ||
	    write_token != context->write_token)
		return -ESTALE;
	ret = s7d_context_status(map, H264_PICTURE_DONE);
	if (ret)
		return ret;
	ret = regmap_read(map, DOS_REG(0x09cf), &saved.scratch_f);
	if (ret)
		goto fault;
	ret = s7d_context_status(map, H264_PICTURE_DONE);
	if (ret)
		goto fault;
	saved.slice = context->slice;
	context->saved = saved;
	context->completion_pending = true;
	return 0;
fault:
	context->faulted = true;
	return ret;
}

int s7d_h264_context_confirm_park(struct s7d_h264_context *context,
				  struct s7d_vdec_runtime *runtime,
				  u64 input_token, u64 write_token,
				  u64 park_epoch, u32 completed_pictures)
{
	int ret;

	if (!runtime || !park_epoch)
		return -EINVAL;
	if (context->faulted)
		return -EIO;
	if (completed_pictures != context->completed_pictures + 1)
		return -EINVAL;
	if (!context->completion_pending || context->park_confirmed ||
	    context->saved.input_token != input_token ||
	    context->saved.write_token != write_token ||
	    park_epoch <= context->last_park_epoch)
		return -ESTALE;
	ret = s7d_vdec_runtime_check_parked(runtime, context->session, input_token,
					 write_token, park_epoch);
	if (ret)
		return ret;
	context->saved.park_epoch = park_epoch;
	context->saved.completed_pictures = completed_pictures;
	context->last_park_epoch = park_epoch;
	context->completed_pictures = context->saved.completed_pictures;
	context->completion_pending = false;
	context->park_confirmed = true;
	return 0;
}

int s7d_h264_context_restore(struct s7d_h264_context *context,
			     struct regmap *map, u64 previous_input,
			     u64 previous_write, u64 park_epoch,
			     u64 next_input)
{
	const struct reg_sequence registers[] = {
		{ DOS_REG(0x0911), 0 },
		{ DOS_REG(0x09b8), 0x404038aa },
		{ DOS_REG(0x09c7), context->buffer_counts },
		{ DOS_REG(0x09d3), H264_ACTION_DECODE_START },
		{ DOS_REG(0x09d2), context->saved.completed_pictures },
		{ DOS_REG(0x09c8), context->workspace_bias },
		{ DOS_REG(0x09cf), (context->saved.scratch_f & 0xffffff83) | BIT(4) },
		{ DOS_REG(0x09d5), context->lmem_address },
		{ DOS_REG(0x09d6), 1 },
		{ DOS_REG(0x09d7), 0 },
		{ DOS_REG(0x09cc), context->aux_address },
		{ DOS_REG(0x09d1), (AUX_PREFIX_BYTES >> 4) << 16 },
		{ DOS_REG(0x09c4), 1 },
		{ DOS_REG(0x09c5), context->sequence_info },
		{ DOS_REG(0x09c3), 0 },
		{ DOS_REG(0x09c2), 1 },
		{ DOS_REG(0x09c9), context->nal_control },
		{ DOS_REG(0x09d4), 0 },
	};
	unsigned int i;
	int ret;

	if (!map || !next_input || next_input <= previous_input)
		return -EINVAL;
	if (context->faulted)
		return -EIO;
	if (!context->park_confirmed ||
	    previous_input != context->saved.input_token ||
	    previous_write != context->saved.write_token ||
	    park_epoch != context->saved.park_epoch)
		return -ESTALE;
	context->park_confirmed = false;
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = s7d_context_write(map, registers[i].reg, registers[i].def);
		if (ret)
			goto fault;
	}
	if (context->saved.slice.iqidct) {
		ret = s7d_context_write(map, DOS_REG(0x0e0e),
					context->saved.slice.iqidct);
		if (ret)
			goto fault;
	}
	if (context->saved.slice.vcop) {
		ret = s7d_context_write(map, DOS_REG(0x0e00),
					context->saved.slice.vcop);
		if (ret)
			goto fault;
	}
	if (context->saved.slice.vld) {
		ret = s7d_context_write(map, DOS_REG(0x0c18),
					context->saved.slice.vld);
		if (ret)
			goto fault;
	}
	ret = s7d_context_cache(map, context->cache_width);
	if (ret)
		goto fault;
	ret = s7d_context_update(map, DOS_REG(0x09e6), BIT(5), BIT(5));
	if (ret)
		goto fault;
	context->input_token = next_input;
	context->write_token = 0;
	context->slice_valid = false;
	return 0;
fault:
	context->faulted = true;
	return ret;
}
