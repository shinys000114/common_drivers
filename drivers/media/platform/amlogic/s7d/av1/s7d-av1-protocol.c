// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/regmap.h>

#include "s7d-av1-protocol.h"

#define HREG(word) ((word) * 4U)
#define SCRATCH(n) HREG(0x30c0 + (n))
#define MAILBOX_CLEAR HREG(0x3071)
#define SHIFT_BYTES HREG(0x310d)
#define MMU_STATUS HREG(0x3639)
#define AV1_DECODE_SLICE 5U
#define AV1_SEARCH_HEAD 6U
#define AV1_ACTION_DONE 0xffU

static bool protocol_overlap(const struct s7d_av1_region *a,
			     const struct s7d_av1_region *b)
{
	return (u64)a->address < (u64)b->address + b->bytes &&
	       (u64)b->address < (u64)a->address + a->bytes;
}

static int protocol_writes(struct regmap *map,
			   const struct reg_sequence *registers, size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = regmap_write(map, registers[i].reg, registers[i].def);
		if (ret)
			return ret;
	}
	return 0;
}

int s7d_av1_protocol_init(struct regmap *map,
			  const struct s7d_av1_workspace *ws,
			  const struct s7d_av1_region *mmu_map,
			  const struct s7d_av1_sequence *sequence)
{
	static const u16 parser_cmd[] = {
		0x0401, 0x8401, 0x0800, 0x0402, 0x9002, 0x1423,
		0x8cc3, 0x1423, 0x8804, 0x9825, 0x0800, 0x04fe,
		0x8406, 0x8411, 0x1800, 0x8408, 0x8409, 0x8c2a,
		0x9c2b, 0x1c00, 0x840f, 0x8407, 0x8000, 0x8408,
		0x2000, 0xa800, 0x8410, 0x04de, 0x840c, 0x840d,
		0xac00, 0xa000, 0x08c0, 0x08e0, 0xa40e, 0xfc00, 0x7c00,
	};
	struct s7d_av1_reference_layout reference;
	u32 i;
	int ret;

	if (!map || !mmu_map)
		return -EINVAL;
	ret = s7d_av1_workspace_validate(ws);
	if (ret)
		return ret;
	ret = s7d_av1_admit_sequence(sequence);
	if (ret)
		return ret;
	if (ws->max_width < sequence->max_frame_width ||
	    ws->max_height < sequence->max_frame_height ||
	    !mmu_map->address || !IS_ALIGNED(mmu_map->address, 4096) ||
	    mmu_map->bytes < S7D_AV1_MMU_MAP_BYTES ||
	    (u64)mmu_map->address + mmu_map->bytes > (1ULL << 32) ||
	    protocol_overlap(mmu_map, &ws->backing) ||
	    protocol_overlap(mmu_map, &ws->rpm) || protocol_overlap(mmu_map, &ws->lmem))
		return -EINVAL;
	ret = s7d_av1_reference_layout(sequence->max_frame_width,
		sequence->max_frame_height, sequence->bit_depth,
		sequence->use_128x128_superblock ? 128 : 64, &reference);
	if (ret)
		return ret;
	{
		const struct reg_sequence registers[] = {
			{ SCRATCH(1), ws->rpm.address },
			{ SCRATCH(5), ws->part[S7D_AV1_DAALA_TOP].address },
			{ SCRATCH(8), ws->part[S7D_AV1_CDF].address },
			{ SCRATCH(9), ws->part[S7D_AV1_CDF].address },
			{ HREG(0x3165), ws->part[S7D_AV1_SEGMENT_MAP].address },
			{ HREG(0x3166), ws->part[S7D_AV1_SEGMENT_MAP].address },
			{ HREG(0x316d), ws->part[S7D_AV1_GMC].address },
			{ HREG(0x3409), ws->part[S7D_AV1_IPP].address },
			{ HREG(0x3504), ws->part[S7D_AV1_DBLK_PARAMETERS].address },
			{ HREG(0x3505), ws->part[S7D_AV1_DBLK_DATA].address },
			{ HREG(0x3503), 0x404010 },
			{ HREG(0x34c2), BIT(4) },
			{ HREG(0x34c3), 0 },
			{ HREG(0x3627), reference.body_bytes },
			{ HREG(0x362b), reference.body_bytes },
			{ HREG(0x3629), reference.header_bytes },
			{ HREG(0x363a), ws->part[S7D_AV1_MMU_VBH].address },
			{ HREG(0x363b), ws->part[S7D_AV1_MMU_VBH].address +
				ws->part[S7D_AV1_MMU_VBH].bytes / 2 },
			{ HREG(0x363e), mmu_map->address },
			{ HREG(0x3108), 0x6e7 },
			{ SCRATCH(0), 0 },
			{ SCRATCH(7), 0 },
			{ SCRATCH(21), 0 },
			{ SCRATCH(19), 0x80000002 },
			{ SCRATCH(23), 0 },
			{ SCRATCH(22), 0 },
			{ HREG(0x3112), BIT(16) },
		};

		ret = protocol_writes(map, registers, ARRAY_SIZE(registers));
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(map, HREG(0x3623), BIT(10) | BIT(15), BIT(10));
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x362d), BIT(10), 0);
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x324c), BIT(1), BIT(1));
	if (!ret)
		ret = regmap_update_bits(map, SCRATCH(16), BIT(0) | BIT(1), BIT(0) | BIT(1));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(parser_cmd); i++) {
		ret = regmap_write(map, HREG(0x3112), parser_cmd[i]);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence registers[] = {
			{ HREG(0x3128), 0x0000090b },
			{ HREG(0x3129), 0x1b14140f },
			{ HREG(0x312a), 0x001b1910 },
			{ HREG(0x34c7), 1U | BIT(2) | BIT(3) | (24 << 4) |
				(32 << 11) | (24 << 18) | (32U << 25) },
		};

		ret = protocol_writes(map, registers, ARRAY_SIZE(registers));
		if (ret)
			return ret;
	}
	for (i = 0; i < 32; i++) {
		u32 low = max(i * 2, 1U);
		u32 high = i * 2 + 1;

		low = (low << 8) | (2 * (i * 2 + 2) + low);
		high = (high << 8) | (2 * (i * 2 + 3) + high);
		ret = regmap_write(map, HREG(0x3509), (high << 16) | low);
		if (ret)
			return ret;
	}
	{
		const struct reg_sequence registers[] = {
			{ HREG(0x350b), 0x5704 },
			{ SCRATCH(14), 1 },
			{ HREG(0x3911), 0 },
			{ SCRATCH(18), 8 },
			{ SCRATCH(20), 0 },
		};

		return protocol_writes(map, registers, ARRAY_SIZE(registers));
	}
}

int s7d_av1_protocol_start(struct regmap *map,
			   const struct s7d_av1_workspace *workspace,
			   const struct s7d_av1_input_plan *input, u32 decode_count)
{
	u32 actual;
	int ret;

	if (!map || !workspace || !input || !input->token || !input->decode_size)
		return -EINVAL;
	ret = s7d_av1_workspace_validate(workspace);
	if (ret)
		return ret;
	{
		const struct reg_sequence registers[] = {
			{ SCRATCH(12), 0 },
			{ SCRATCH(0), AV1_ACTION_DONE },
			{ SHIFT_BYTES, 0 },
			{ SCRATCH(23), input->decode_size },
			{ SCRATCH(22), decode_count },
			{ SCRATCH(18), workspace->lmem.address },
		};

		ret = protocol_writes(map, registers, ARRAY_SIZE(registers));
	}
	if (!ret)
		ret = regmap_read(map, SCRATCH(0), &actual);
	if (!ret && actual != AV1_ACTION_DONE)
		ret = -EIO;
	return ret;
}

int s7d_av1_protocol_fgs_read_ack(struct regmap *map)
{
	u32 request;
	int ret;

	if (!map)
		return -EINVAL;
	ret = regmap_read(map, SCRATCH(11), &request);
	if (ret)
		return ret;
	if (request & BIT(11))
		return -EOPNOTSUPP;
	ret = regmap_write(map, SCRATCH(11), S7D_AV1_FGS_CONTINUE);
	if (!ret)
		ret = regmap_write(map, SCRATCH(0), S7D_AV1_FGS_CONTINUE);
	return ret;
}

int s7d_av1_protocol_irq_snapshot(struct regmap *map,
				  struct s7d_av1_irq_snapshot *snapshot)
{
	struct s7d_av1_irq_snapshot next = { 0 };
	u32 fresh, pages;
	int ret;

	if (!map || !snapshot)
		return -EINVAL;
	ret = regmap_write(map, MAILBOX_CLEAR, 1);
	if (!ret)
		ret = regmap_read(map, SCRATCH(0), &next.status);
	if (!ret)
		ret = regmap_read(map, SHIFT_BYTES, &next.shift_bytes);
	if (!ret)
		ret = regmap_read(map, SCRATCH(2), &next.lcu_count);
	if (!ret)
		ret = regmap_read(map, MMU_STATUS, &pages);
	if (!ret)
		ret = regmap_read(map, SCRATCH(0), &fresh);
	if (!ret && fresh != next.status)
		ret = -ESTALE;
	if (!ret)
		next.mmu_pages = pages >> 16;
	next.error = ret;
	*snapshot = next;
	return ret;
}

static bool same_cookie(const struct s7d_hevc_irq_cookie *a,
			const struct s7d_hevc_irq_cookie *b)
{
	return a->session == b->session && a->binding == b->binding &&
		a->lease_epoch == b->lease_epoch && a->input_token == b->input_token;
}

int s7d_av1_execution_begin(struct s7d_av1_execution *execution,
			    const struct s7d_hevc_irq_cookie *cookie,
			    const struct s7d_av1_input_plan *input,
			    u32 expected_frames)
{
	struct s7d_av1_execution next = { 0 };

	if (!execution || !cookie || !cookie->session || !cookie->binding ||
	    !cookie->lease_epoch || !input || !input->token ||
	    cookie->input_token != input->token || !input->decode_size || !expected_frames ||
	    input->max_shift_bytes < input->decode_size)
		return -EINVAL;
	if (execution->phase != S7D_AV1_EXECUTION_IDLE)
		return -EBUSY;
	next.cookie = *cookie;
	next.input_bytes = input->decode_size;
	next.max_shift_bytes = input->max_shift_bytes;
	next.expected_frames = expected_frames;
	next.phase = S7D_AV1_EXECUTION_SEARCH;
	*execution = next;
	return 0;
}

int s7d_av1_execution_receive(struct s7d_av1_execution *e,
			      const struct s7d_hevc_irq_cookie *cookie,
			      const struct s7d_av1_irq_snapshot *s,
			      enum s7d_av1_execution_event *event)
{
	enum s7d_av1_execution_event next = S7D_AV1_EVENT_NONE;
	int ret = 0;

	if (!e || !cookie || !s || !event)
		return -EINVAL;
	if (!same_cookie(&e->cookie, cookie))
		return -ESTALE;
	if (e->phase != S7D_AV1_EXECUTION_SEARCH &&
	    e->phase != S7D_AV1_EXECUTION_DECODE)
		return -EBUSY;
	if (s->error)
		ret = s->error;
	else if (s->shift_bytes < e->last_shift_bytes ||
		 s->shift_bytes > e->max_shift_bytes)
		ret = -EPROTO;
	if (ret)
		goto fail;
	switch (s->status) {
	case 0:
	case AV1_ACTION_DONE:
	case AV1_DECODE_SLICE:
	case AV1_SEARCH_HEAD:
	case S7D_AV1_FGS_CONTINUE:
		break;
	case S7D_AV1_SEQUENCE_PARSED:
		if (e->phase != S7D_AV1_EXECUTION_SEARCH)
			ret = -EPROTO;
		next = S7D_AV1_EVENT_SEQUENCE;
		break;
	case S7D_AV1_FRAME_HEADER_PARSED:
	case S7D_AV1_FRAME_PARSED:
		if (e->phase != S7D_AV1_EXECUTION_SEARCH || !e->sequence_seen ||
		    e->headers_seen >= e->expected_frames)
			ret = -EPROTO;
		next = S7D_AV1_EVENT_FRAME;
		break;
	case S7D_AV1_REDUNDANT_PARSED:
		if (e->phase != S7D_AV1_EXECUTION_DECODE)
			ret = -EPROTO;
		next = S7D_AV1_EVENT_REDUNDANT;
		break;
	case S7D_AV1_PICTURE_PRE_END:
		if (e->phase != S7D_AV1_EXECUTION_DECODE)
			ret = -EPROTO;
		break;
	case S7D_AV1_PICTURE_END:
		if (e->phase != S7D_AV1_EXECUTION_DECODE || !s->lcu_count ||
		    s->mmu_pages > S7D_AV1_MMU_MAX_PAGES)
			ret = -EPROTO;
		next = S7D_AV1_EVENT_PICTURE;
		break;
	case S7D_AV1_NAL_DONE:
	case S7D_AV1_SEARCH_EMPTY:
	case S7D_AV1_DECODE_EMPTY:
	case S7D_AV1_EOS:
		if (e->phase != S7D_AV1_EXECUTION_SEARCH ||
		    e->headers_seen != e->expected_frames ||
		    e->pictures_done != e->headers_seen || s->shift_bytes < e->input_bytes)
			ret = -EBADMSG;
		next = s->status == S7D_AV1_EOS ? S7D_AV1_EVENT_EOS : S7D_AV1_EVENT_INPUT;
		break;
	case S7D_AV1_FGS_REQUEST:
		if (!e->sequence_seen ||
		    (e->phase != S7D_AV1_EXECUTION_SEARCH &&
		     e->phase != S7D_AV1_EXECUTION_DECODE) ||
		    (e->phase == S7D_AV1_EXECUTION_SEARCH && e->headers_seen) ||
		    (e->phase == S7D_AV1_EXECUTION_DECODE &&
		     (e->headers_seen != 1 || e->pictures_done || e->show_existing)))
			ret = -EOPNOTSUPP;
		next = S7D_AV1_EVENT_FGS_READ;
		break;
	case S7D_AV1_TIMEOUT:
		ret = -ETIMEDOUT;
		break;
	case S7D_AV1_OVERSIZE:
		ret = -EOVERFLOW;
		break;
	default:
		ret = -EPROTO;
	}
	if (ret)
		goto fail;
	e->last_shift_bytes = s->shift_bytes;
	e->paused_status = s->status;
	if (next == S7D_AV1_EVENT_SEQUENCE || next == S7D_AV1_EVENT_FRAME ||
	    next == S7D_AV1_EVENT_REDUNDANT)
		e->phase = S7D_AV1_EXECUTION_HEADER;
	else if (next == S7D_AV1_EVENT_PICTURE)
		e->phase = S7D_AV1_EXECUTION_PICTURE;
	else if (next == S7D_AV1_EVENT_INPUT || next == S7D_AV1_EVENT_EOS) {
		e->phase = S7D_AV1_EXECUTION_INPUT;
		e->input_consumed = true;
	}
	*event = next;
	return 0;
fail:
	e->phase = S7D_AV1_EXECUTION_FAILED;
	return ret;
}

int s7d_av1_execution_resume(struct s7d_av1_execution *e,
			     struct regmap *map, bool picture_configured)
{
	u32 command;
	int ret;

	if (!e || !map)
		return -EINVAL;
	if (e->phase != S7D_AV1_EXECUTION_HEADER || e->show_existing)
		return -EBUSY;
	if (e->paused_status == S7D_AV1_SEQUENCE_PARSED) {
		if (picture_configured)
			return -EINVAL;
		command = AV1_SEARCH_HEAD;
	} else {
		if (!picture_configured)
			return -EINVAL;
		command = AV1_DECODE_SLICE;
	}
	if (command == AV1_DECODE_SLICE && e->paused_status != S7D_AV1_REDUNDANT_PARSED)
		ret = regmap_write(map, SCRATCH(2), 0);
	else
		ret = 0;
	if (!ret)
		ret = regmap_write(map, SCRATCH(0), command);
	if (ret) {
		e->phase = S7D_AV1_EXECUTION_FAILED;
		return ret;
	}
	if (command == AV1_SEARCH_HEAD) {
		e->sequence_seen = true;
		e->phase = S7D_AV1_EXECUTION_SEARCH;
	} else {
		if (e->paused_status != S7D_AV1_REDUNDANT_PARSED)
			e->headers_seen++;
		e->phase = S7D_AV1_EXECUTION_DECODE;
	}
	return 0;
}

int s7d_av1_execution_accept_frame(struct s7d_av1_execution *e,
				   const struct s7d_av1_header *header,
				   enum s7d_av1_execution_event *event)
{
	if (!e || !header || !event)
		return -EINVAL;
	if (e->phase != S7D_AV1_EXECUTION_HEADER ||
	    (e->paused_status != S7D_AV1_FRAME_HEADER_PARSED &&
	     e->paused_status != S7D_AV1_FRAME_PARSED))
		return -EBUSY;
	e->show_existing = header->frame.show_existing;
	if (e->show_existing) {
		e->headers_seen++;
		e->phase = S7D_AV1_EXECUTION_PICTURE;
		*event = S7D_AV1_EVENT_EXISTING;
	} else {
		*event = S7D_AV1_EVENT_FRAME;
	}
	return 0;
}

int s7d_av1_execution_picture_complete(struct s7d_av1_execution *e,
				       bool physical_idle)
{
	if (!e)
		return -EINVAL;
	if (e->phase != S7D_AV1_EXECUTION_PICTURE)
		return -EBUSY;
	if (!physical_idle)
		return -EBUSY;
	e->pictures_done++;
	if (e->pictures_done == e->expected_frames &&
	    e->last_shift_bytes >= e->input_bytes) {
		e->input_consumed = true;
		e->phase = S7D_AV1_EXECUTION_INPUT;
	} else {
		/* The owner must restore the remaining input after its verified halt. */
		e->phase = S7D_AV1_EXECUTION_RESTART;
	}
	return 0;
}

int s7d_av1_execution_drain(struct s7d_av1_execution *e, struct regmap *map)
{
	int ret;

	if (!e || !map)
		return -EINVAL;
	if (e->phase != S7D_AV1_EXECUTION_PICTURE || e->expected_frames != 1 ||
	    e->headers_seen != 1 || e->pictures_done)
		return -EBUSY;
	ret = regmap_write(map, SCRATCH(0), AV1_SEARCH_HEAD);
	if (ret) {
		e->phase = S7D_AV1_EXECUTION_FAILED;
		return ret;
	}
	e->pictures_done++;
	e->phase = S7D_AV1_EXECUTION_SEARCH;
	return 0;
}
