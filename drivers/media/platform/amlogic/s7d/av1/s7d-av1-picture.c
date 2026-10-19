// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include "s7d-av1-picture.h"
#include "s7d-av1-reference.h"

#define HREG(word) ((word) * 4U)
#define CDF_BANK_BYTES 0x8000U
#define GRAIN_BANK_BYTES 0x2000U

static bool overlap(const struct s7d_av1_region *a,
		    const struct s7d_av1_region *b)
{
	return a->bytes && b->bytes &&
		(u64)a->address < (u64)b->address + b->bytes &&
		(u64)b->address < (u64)a->address + a->bytes;
}

static bool region_valid(const struct s7d_av1_region *region,
			 u32 alignment, u32 required)
{
	return region->address && IS_ALIGNED(region->address, alignment) &&
		region->bytes >= required &&
		(u64)region->address + region->bytes <= (1ULL << 32);
}

static const struct s7d_av1_region *
slot_region(const struct s7d_av1_reference_backing *backing, u32 index)
{
	switch (index) {
	case 0:
		return &backing->body;
	case 1:
		return &backing->header;
	case 2:
		return &backing->mv;
	default:
		return &backing->linear;
	}
}

static bool id_equal(const struct s7d_av1_picture_id *a,
		     const struct s7d_av1_picture_id *b)
{
	return a->slot == b->slot && a->backing_cookie &&
		a->backing_cookie == b->backing_cookie;
}

static int backing_validate(const struct s7d_av1_picture_input *input)
{
	const struct s7d_av1_workspace *ws = input->workspace;
	struct s7d_av1_reference_layout layout;
	u32 superblock = input->sequence->use_128x128_superblock ? 128 : 64;
	u32 segment_bytes = ws->part[S7D_AV1_SEGMENT_MAP].bytes / 16;
	u32 i, j, a, b, uv_bytes;
	int ret;

	ret = s7d_av1_reference_layout(input->sequence->max_frame_width,
		input->sequence->max_frame_height, 8, superblock, &layout);
	if (ret)
		return ret;
	uv_bytes = ALIGN(DIV_ROUND_UP(input->sequence->max_frame_width, superblock) *
		DIV_ROUND_UP(input->sequence->max_frame_height, superblock) *
		superblock * superblock / 2, SZ_64K);
	if (!region_valid(&input->mmu_map, SZ_4K, S7D_AV1_MMU_MAP_BYTES) ||
	    overlap(&input->mmu_map, &ws->backing) ||
	    overlap(&input->mmu_map, &ws->rpm) || overlap(&input->mmu_map, &ws->lmem))
		return -EINVAL;
	for (i = 0; i < input->backing_count; i++) {
		const struct s7d_av1_reference_backing *slot = &input->backing[i];

		if (!slot->id.backing_cookie)
			continue;
		if (slot->id.slot != i ||
		    !region_valid(&slot->body, SZ_4K, layout.body_allocation) ||
		    !region_valid(&slot->header, 32, layout.header_capacity) ||
		    !region_valid(&slot->mv, SZ_64K, layout.mv_allocation) ||
		    !region_valid(&slot->linear, SZ_64K, uv_bytes * 3) ||
		    slot->y_bytes < uv_bytes * 2 || slot->uv_bytes < uv_bytes ||
		    (u64)slot->y_bytes + slot->uv_bytes > slot->linear.bytes ||
		    slot->cdf.address != ws->part[S7D_AV1_CDF].address + i * CDF_BANK_BYTES ||
		    slot->cdf.bytes != CDF_BANK_BYTES ||
		    slot->segment_map.address != ws->part[S7D_AV1_SEGMENT_MAP].address +
					 i * segment_bytes ||
		    slot->segment_map.bytes != segment_bytes)
			return -EINVAL;
		for (a = 0; a < 4; a++) {
			const struct s7d_av1_region *region = slot_region(slot, a);

			if (overlap(region, &ws->backing) || overlap(region, &ws->rpm) ||
			    overlap(region, &ws->lmem) || overlap(region, &input->mmu_map))
				return -EINVAL;
			for (b = a + 1; b < 4; b++)
				if (overlap(region, slot_region(slot, b)))
					return -EINVAL;
			for (j = 0; j < i; j++) {
				if (!input->backing[j].id.backing_cookie)
					continue;
				for (b = 0; b < 4; b++)
					if (overlap(region, slot_region(&input->backing[j], b)))
						return -EINVAL;
			}
		}
	}
	return 0;
}

static int picture_validate(const struct s7d_av1_picture_input *input)
{
	const struct s7d_av1_dpb_plan *dpb;
	const struct s7d_av1_dpb_picture *picture;
	const struct s7d_av1_dpb_frame *frame;
	u32 i;
	int ret;

	if (!input || !input->workspace || !input->sequence || !input->header ||
	    !input->dpb || !input->backing || !input->state ||
	    !input->backing_count || input->backing_count > S7D_AV1_FRAME_SLOTS)
		return -EINVAL;
	ret = s7d_av1_workspace_validate(input->workspace);
	if (ret)
		return ret;
	ret = s7d_av1_rpm_check_sequence(&input->header->snapshot, input->sequence);
	if (ret)
		return ret;
	if (input->sequence->max_frame_width > input->workspace->max_width ||
	    input->sequence->max_frame_height > input->workspace->max_height)
		return -EINVAL;
	dpb = input->dpb;
	frame = &input->header->frame;
	picture = &dpb->current_picture;
	if (!dpb->generation || !dpb->serial || !dpb->metadata.input_token ||
	    !dpb->decode || frame->show_existing ||
	    picture->id.slot >= input->backing_count ||
	    !id_equal(&picture->id, &input->backing[picture->id.slot].id) ||
	    !frame->width || !frame->height || frame->decode_width != frame->width ||
	    frame->width > input->sequence->max_frame_width ||
	    frame->height > input->sequence->max_frame_height || frame->bit_depth != 8 ||
	    picture->width != frame->width || picture->height != frame->height ||
	    picture->decode_width != frame->decode_width || picture->bit_depth != 8 ||
	    picture->frame_type != frame->frame_type ||
	    picture->order_hint != frame->order_hint ||
	    input->backing[picture->id.slot].pitch != ALIGN(frame->width, 64) ||
	    dpb->primary_ref > S7D_AV1_DPB_PRIMARY_NONE || dpb->sign_bias_mask > 0x7f)
		return -EINVAL;
	if (frame->frame_type > S7D_AV1_SWITCH_FRAME ||
	    (input->sequence->order_hint_bits &&
	     frame->order_hint >= BIT(input->sequence->order_hint_bits)))
		return -EINVAL;
	if ((dpb->primary_ref == S7D_AV1_DPB_PRIMARY_NONE) != !input->primary)
		return -EINVAL;
	if (input->state->segmentation_enabled != !!(input->header->segmentation & 1) ||
	    input->state->segmentation_update_map != !!(input->header->segmentation & 8) ||
	    input->state->intra_only != !!(input->header->segmentation & 4) ||
	    input->state->mi_cols != ALIGN(frame->width, 8) / 4 ||
	    input->state->mi_rows != ALIGN(frame->height, 8) / 4)
		return -EINVAL;
	if (frame->frame_type == S7D_AV1_INTER_FRAME ||
	    frame->frame_type == S7D_AV1_SWITCH_FRAME) {
		for (i = 0; i < 7; i++) {
			const struct s7d_av1_dpb_picture *ref = &dpb->reference[i];
			u32 j;

			if (!ref->decoded || ref->id.slot >= input->backing_count ||
			    ref->id.slot == picture->id.slot ||
			    !id_equal(&ref->id, &input->backing[ref->id.slot].id) ||
			    !ref->width || !ref->height || ref->bit_depth != 8 ||
			    ref->decode_width != ref->width ||
			    ref->width > input->sequence->max_frame_width ||
			    ref->height > input->sequence->max_frame_height ||
			    ref->width > frame->width * 2 || ref->height > frame->height * 2 ||
			    frame->width > ref->width * 16 || frame->height > ref->height * 16)
				return -EINVAL;
			if (input->sequence->order_hint_bits &&
			    ref->order_hint >= BIT(input->sequence->order_hint_bits))
				return -EINVAL;
			for (j = 0; j < 7; j++)
				if (input->sequence->order_hint_bits &&
				    ref->ref_order_hint[j] >= BIT(input->sequence->order_hint_bits))
					return -EINVAL;
		}
	}
	return backing_validate(input);
}

static s32 relative_dist(u32 bits, u32 a, u32 b)
{
	u32 half, difference;

	if (!bits)
		return 0;
	half = BIT(bits - 1);
	difference = (a - b) & (BIT(bits) - 1);
	return (s32)(difference & (half - 1)) - (s32)(difference & half);
}

static void motion_candidate(const struct s7d_av1_picture_input *input,
			     struct s7d_av1_hw_picture_plan *plan, u32 index)
{
	const struct s7d_av1_dpb_picture *ref = &input->dpb->reference[index];
	const struct s7d_av1_dpb_picture *picture = &input->dpb->current_picture;
	u32 i, count = plan->mv_count;

	if (count >= 3 || ref->frame_type == S7D_AV1_KEY_FRAME ||
	    ref->frame_type == S7D_AV1_INTRA_ONLY_FRAME ||
	    ALIGN(ref->width, 8) != ALIGN(picture->width, 8) ||
	    ALIGN(ref->height, 8) != ALIGN(picture->height, 8))
		return;
	plan->mv_read[count] = input->backing[ref->id.slot].mv.address;
	for (i = 0; i < 7; i++) {
		u32 position = count * 7 + i;
		u8 offset = relative_dist(input->sequence->order_hint_bits,
					 ref->order_hint, ref->ref_order_hint[i]);

		plan->mv_offsets[position / 4] |= (u32)offset << (24 - (position & 3) * 8);
	}
	plan->mv_offsets[5] |= (index + 1) << (2 + count * 3);
	plan->mv_count++;
}

static void motion_plan(const struct s7d_av1_picture_input *input,
			struct s7d_av1_hw_picture_plan *plan)
{
	const struct s7d_av1_dpb_plan *dpb = input->dpb;
	u32 hint_bits = input->sequence->order_hint_bits;
	u32 current_hint = dpb->current_picture.order_hint;
	u32 old_count;
	int stamp = 2;
	u32 i;

	plan->mv_order = 0x1010 | (current_hint << 16);
	if (hint_bits)
		plan->mv_order |= BIT(27) | ((hint_bits - 1) << 24);
	if (!plan->inter)
		return;
	for (i = 1; i < 7; i++)
		plan->mv_references[0] |= dpb->reference[i].id.slot << (5 * (i - 1));
	plan->mv_references[1] = dpb->reference[0].id.slot << 24;
	for (i = 4; i < 7; i++)
		plan->mv_references[1] |= dpb->reference[i].order_hint << (8 * (i - 4));
	for (i = 0; i < 4; i++)
		plan->mv_references[2] |= dpb->reference[i].order_hint << (i * 8);
	if (!hint_bits)
		return;
	if (dpb->reference[0].ref_order_hint[6] != dpb->reference[3].order_hint)
		motion_candidate(input, plan, 0);
	stamp--;
	for (i = 4; i < 7; i++) {
		if ((i == 6 && stamp < 0) ||
		    relative_dist(hint_bits, dpb->reference[i].order_hint, current_hint) <= 0)
			continue;
		old_count = plan->mv_count;
		motion_candidate(input, plan, i);
		if (plan->mv_count != old_count)
			stamp--;
	}
	if (stamp >= 0)
		motion_candidate(input, plan, 1);
	plan->mv_offsets[5] |= plan->mv_count;
	plan->read_motion = input->header->allow_ref_frame_mvs;
}

static u32 skip_mode(const struct s7d_av1_picture_input *input)
{
	const struct s7d_av1_dpb_plan *dpb = input->dpb;
	u32 bits = input->sequence->order_hint_bits;
	u32 picture = dpb->current_picture.order_hint;
	s32 forward = -1, backward = -1, second = -1;
	u32 i;

	if (!bits || !input->header->snapshot.word[28])
		return 0xff;
	for (i = 0; i < 7; i++) {
		u32 hint = dpb->reference[i].order_hint;
		s32 distance = relative_dist(bits, hint, picture);

		if (distance < 0 && (forward < 0 ||
		    relative_dist(bits, hint, dpb->reference[forward].order_hint) > 0))
			forward = i;
		if (distance > 0 && (backward < 0 ||
		    relative_dist(bits, hint, dpb->reference[backward].order_hint) < 0))
			backward = i;
	}
	if (forward < 0)
		return 0xff;
	if (backward >= 0)
		return BIT(8) | min(forward, backward) | (max(forward, backward) << 4);
	for (i = 0; i < 7; i++) {
		u32 hint = dpb->reference[i].order_hint;

		if (relative_dist(bits, hint, dpb->reference[forward].order_hint) < 0 &&
		    (second < 0 || relative_dist(bits, hint,
					  dpb->reference[second].order_hint) > 0))
			second = i;
	}
	if (second < 0)
		return 0xff;
	return BIT(8) | min(forward, second) | (max(forward, second) << 4);
}

int s7d_av1_picture_plan(const struct s7d_av1_picture_input *input,
			 struct s7d_av1_hw_picture_plan *plan)
{
	struct s7d_av1_hw_picture_plan next = { 0 };
	const struct s7d_av1_reference_backing *picture, *primary;
	const struct s7d_av1_dpb_plan *dpb;
	struct s7d_av1_reference_layout layout;
	u32 superblock, i, best = 0, second = 0, distance[7];
	int ret;

	if (!plan)
		return -EINVAL;
	ret = picture_validate(input);
	if (ret)
		return ret;
	dpb = input->dpb;
	next.current_slot = dpb->current_picture.id.slot;
	picture = &input->backing[next.current_slot];
	primary = dpb->primary_ref == S7D_AV1_DPB_PRIMARY_NONE ? NULL :
		&input->backing[dpb->reference[dpb->primary_ref].id.slot];
	next.inter = input->header->frame.frame_type == S7D_AV1_INTER_FRAME ||
		input->header->frame.frame_type == S7D_AV1_SWITCH_FRAME;
	next.backing_count = input->backing_count;
	superblock = input->sequence->use_128x128_superblock ? 128 : 64;
	ret = s7d_av1_reference_layout(dpb->current_picture.width,
		dpb->current_picture.height, 8, superblock, &layout);
	if (ret)
		return ret;
	next.body_bytes = layout.body_bytes;
	next.header_bytes = layout.header_bytes;
	next.lcu_total = DIV_ROUND_UP(dpb->current_picture.width, superblock) *
		DIV_ROUND_UP(dpb->current_picture.height, superblock);
	next.uv_bytes = ALIGN(next.lcu_total * superblock * superblock / 2, SZ_64K);
	next.y_bytes = next.uv_bytes * 2;
	next.y_address = picture->linear.address;
	next.uv_address = picture->linear.address + picture->y_bytes;
	next.header_address = picture->header.address;
	next.current_mv = picture->mv.address;
	next.cdf_write = picture->cdf.address;
	next.cdf_read = primary ? primary->cdf.address :
		input->workspace->part[S7D_AV1_CDF].address;
	next.segment_write = picture->segment_map.address;
	next.segment_read = primary ? primary->segment_map.address :
		input->workspace->part[S7D_AV1_SEGMENT_MAP].address;
	next.grain_table = input->workspace->part[S7D_AV1_FILM_GRAIN].address +
		next.current_slot * GRAIN_BANK_BYTES;
	next.skip_mode = next.inter ? skip_mode(input) : 0xff;
	next.skip_mode |= next.current_slot << 16;
	next.skip_mode |= (primary ? primary->id.slot : 0x1f) << 24;
	if (input->primary && input->primary->intra_only)
		next.skip_mode |= BIT(30);
	if (input->state->prev_segmentation_enabled)
		next.skip_mode |= BIT(31);
	next.sign_bias = dpb->sign_bias_mask << 13;
	next.restore_segment_features = primary && input->state->segmentation_enabled &&
		!(input->header->segmentation & 2);
	for (i = 0; i < input->backing_count; i++)
		if (input->backing[i].id.backing_cookie)
			next.anc2axi[i] = input->backing[i].header.address >> 5;
	for (i = 0; i < 8; i++) {
		const struct s7d_av1_dpb_picture *ref = next.inter && i ?
			&dpb->reference[i - 1] : &dpb->current_picture;

		next.canvas[i] = (ref->order_hint << 24) | (ref->id.slot << 16) |
			(ref->id.slot << 8) | ref->id.slot;
		next.reference_info[i][0] = ref->width;
		next.reference_info[i][1] = ref->height;
		next.reference_info[i][2] = ((ref->width << 14) +
			dpb->current_picture.width / 2) / dpb->current_picture.width;
		next.reference_info[i][3] = ((ref->height << 14) +
			dpb->current_picture.height / 2) / dpb->current_picture.height;
		if (i && (next.reference_info[i][2] != BIT(14) ||
			  next.reference_info[i][3] != BIT(14)))
			next.scale_enable |= BIT(i);
	}
	motion_plan(input, &next);
	next.cache_canvas[0] = U32_MAX;
	next.cache_canvas[1] = U32_MAX;
	if (next.inter) {
		for (i = 0; i < 7; i++) {
			distance[i] = abs(relative_dist(input->sequence->order_hint_bits,
				dpb->reference[i].order_hint, dpb->current_picture.order_hint));
			if (distance[i] < distance[best])
				best = i;
		}
		for (i = 0; i < 7; i++)
			if (dpb->reference[i].id.slot != dpb->reference[best].id.slot &&
			    (dpb->reference[second].id.slot == dpb->reference[best].id.slot ||
			     distance[i] < distance[second]))
				second = i;
		for (i = 0; i < 2; i++) {
			u32 canvas = next.canvas[(i ? second : best) + 1] & 0xffff;

			next.cache_canvas[i] = canvas | (canvas << 16);
		}
	}
	*plan = next;
	return 0;
}

static int write_many(struct regmap *map, const struct reg_sequence *registers,
		      size_t count)
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

static int program_mc(struct regmap *map,
		      const struct s7d_av1_hw_picture_plan *plan)
{
	u32 i, j, bank;
	int ret;

	ret = regmap_write(map, HREG(0x34f0), 2);
	if (!ret)
		ret = regmap_write(map, HREG(0x3460), BIT(1) | BIT(2));
	for (i = 0; !ret && i < S7D_AV1_FRAME_SLOTS; i++)
		ret = regmap_write(map, HREG(0x3464), plan->anc2axi[i]);
	if (!ret)
		ret = regmap_write(map, HREG(0x3460), 1);
	if (!ret)
		ret = regmap_write(map, HREG(0x34c0), 1);
	for (i = 0; !ret && i < 32; i++)
		ret = regmap_write(map, HREG(0x34c1), 0);
	for (bank = 0; !ret && bank < 2; bank++) {
		ret = regmap_write(map, HREG(0x34c0), (bank * 16 << 8) | 1);
		for (i = 0; !ret && i < 8; i++)
			ret = regmap_write(map, HREG(0x34c1), plan->canvas[i]);
	}
	if (!ret)
		ret = regmap_write(map, HREG(0x3442), BIT(2));
	for (i = 0; !ret && i < 8; i++)
		for (j = 0; !ret && j < 5; j++)
			ret = regmap_write(map, HREG(0x3443), plan->reference_info[i][j]);
	if (!ret)
		ret = regmap_write(map, HREG(0x3441), plan->scale_enable);
	if (!ret)
		ret = regmap_write(map, HREG(0x316b), plan->scale_enable);
	if (!ret) {
		const struct reg_sequence registers[] = {
			{ HREG(0x34f1), plan->cache_canvas[0] },
			{ HREG(0x34f2), plan->cache_canvas[1] },
			{ HREG(0x34f0), 0xff0 },
		};

		ret = write_many(map, registers, ARRAY_SIZE(registers));
	}
	return ret;
}

static int program_motion(struct regmap *map,
			  const struct s7d_av1_picture_input *input,
			  const struct s7d_av1_hw_picture_plan *plan)
{
	u32 i;
	int ret;

	ret = regmap_update_bits(map, HREG(0x3201), BIT(10) | BIT(11), 0);
	if (!ret) {
		const struct reg_sequence registers[] = {
			{ HREG(0x321d), 0x10151015 },
			{ HREG(0x3212), input->workspace->part[S7D_AV1_MPRED_ABOVE].address },
			{ HREG(0x3220), plan->mv_order },
			{ HREG(0x3213), plan->current_mv },
			{ HREG(0x3215), plan->current_mv },
		};

		ret = write_many(map, registers, ARRAY_SIZE(registers));
	}
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x324c), GENMASK(19, 12) |
			(plan->inter ? 0 : BIT(6)) | BIT(1), plan->sign_bias | BIT(1));
	for (i = 0; !ret && i < 3; i++)
		ret = regmap_write(map, HREG(0x3223 + i), plan->mv_references[i]);
	for (i = 0; !ret && i < 6; i++)
		ret = regmap_write(map, HREG(0x3226 + i), plan->mv_offsets[i]);
	if (!ret) {
		const struct reg_sequence registers[] = {
			{ HREG(0x3214), plan->mv_read[0] },
			{ HREG(0x3216), plan->mv_read[0] },
			{ HREG(0x3221), plan->mv_read[1] },
			{ HREG(0x3263), plan->mv_read[1] },
			{ HREG(0x3222), plan->mv_read[2] },
			{ HREG(0x3264), plan->mv_read[2] },
		};

		ret = write_many(map, registers, ARRAY_SIZE(registers));
	}
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x3201), BIT(10) | BIT(11),
			BIT(10) | (plan->read_motion ? BIT(11) : 0));
	return ret;
}

static int program_output(struct regmap *map,
			  const struct s7d_av1_picture_input *input,
			  const struct s7d_av1_hw_picture_plan *plan)
{
	const struct s7d_av1_dpb_picture *picture = &input->dpb->current_picture;
	const struct reg_sequence registers[] = {
		{ HREG(0x3123), (picture->height << 16) | picture->width },
		{ HREG(0x300d), (picture->height << 16) | picture->width },
		{ HREG(0x34c2), BIT(4) },
		{ HREG(0x34c3), plan->body_bytes >> 5 },
		{ HREG(0x3627), plan->body_bytes },
		{ HREG(0x362b), plan->body_bytes },
		{ HREG(0x3629), plan->header_bytes },
		{ HREG(0x363e), input->mmu_map.address },
		{ HREG(0x3601), input->sequence->use_128x128_superblock ? 7 : 6 },
		{ HREG(0x3626), 0 },
		{ HREG(0x3628), plan->header_address },
		{ HREG(0x3666), plan->grain_table },
		{ HREG(0x3667), GRAIN_BANK_BYTES * 8 },
		{ HREG(0x360b), plan->y_address },
		{ HREG(0x360c), plan->y_bytes },
		{ HREG(0x360d), plan->uv_address },
		{ HREG(0x360e), plan->uv_bytes },
		{ HREG(0x360f), plan->y_address },
		{ HREG(0x3610), plan->uv_address },
		{ HREG(0x3677), 0 },
	};
	int ret;

	ret = write_many(map, registers, ARRAY_SIZE(registers));
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x362d), BIT(1) | BIT(10), BIT(1));
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x3623), BIT(9) | BIT(10) |
			BIT(14) | BIT(15) | GENMASK(23, 16), BIT(9) | BIT(10) | BIT(14));
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x3602), GENMASK(15, 4) | GENMASK(1, 0),
			0xff0 | BIT(15));
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x3621), BIT(1), 0);
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x340b), GENMASK(5, 0) |
			GENMASK(9, 8) | BIT(12), 0xf | BIT(9));
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x350b), GENMASK(9, 8), GENMASK(9, 8));
	return ret;
}

static u32 filter_threshold(u32 level, u32 sharpness)
{
	u32 limit = level >> ((sharpness > 0) + (sharpness > 4));

	if (sharpness)
		limit = min(limit, 9 - sharpness);
	limit = max(limit, 1U);
	return (limit << 8) | (2 * (level + 2) + limit);
}

static int program_filter(struct regmap *map,
			  const struct s7d_av1_picture_input *input,
			  const struct s7d_av1_hw_picture_plan *plan)
{
	const struct s7d_av1_reference_state *state = input->state;
	const struct s7d_av1_header *header = input->header;
	u32 i, value;
	int ret;

	for (i = 0; i < 32; i++) {
		value = filter_threshold(i * 2, header->sharpness) |
			(filter_threshold(i * 2 + 1, header->sharpness) << 16);
		ret = regmap_write(map, HREG(0x3509), value);
		if (ret)
			return ret;
	}
	value = header->filter_level[0] | (header->filter_level[1] << 6) |
		(header->filter_level[2] << 12) | (header->filter_level[3] << 18);
	ret = regmap_write(map, HREG(0x3523), value);
	for (i = 0; !ret && i < 10; i++)
		ret = regmap_write(map, HREG(0x3524),
			i < 2 ? (s32)state->mode_delta[i] : (s32)state->ref_delta[i - 2]);
	for (i = 0; !ret && i < 8; i++)
		ret = regmap_write(map, HREG(0x3525),
			state->seg_lf_y[i] | ((u32)state->seg_lf_c[i] << 16));
	value = header->filter_delta_enabled ? BIT(28) : 0;
	if (state->segmentation_enabled)
		value |= BIT(29);
	if (input->dpb->current_picture.width >= 1280)
		value |= BIT(4);
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x350b), BIT(28) | BIT(29) |
			GENMASK(5, 4), value);
	if (!ret) {
		const struct reg_sequence registers[] = {
			{ HREG(0x3515), input->workspace->part[S7D_AV1_CDEF_DATA].address },
			{ HREG(0x351c), input->workspace->part[S7D_AV1_UPSCALE_DATA].address },
			{ HREG(0x351d), 128 },
			{ HREG(0x351e), 128 },
			{ HREG(0x351f), BIT(14) },
			{ HREG(0x3520), BIT(14) },
			{ HREG(0x316e), (128 << 16) | 128 },
			{ HREG(0x316f), (BIT(14) << 16) | BIT(14) },
			{ HREG(0x30c8), plan->cdf_write },
			{ HREG(0x30c9), plan->cdf_read },
			{ HREG(0x3165), plan->segment_write },
			{ HREG(0x3166), plan->segment_read },
		};

		ret = write_many(map, registers, ARRAY_SIZE(registers));
	}
	if (!ret)
		ret = regmap_update_bits(map, HREG(0x3516), 0xff, 17);
	if (plan->restore_segment_features)
		for (i = 0; !ret && i < 8; i++)
			ret = regmap_write(map, HREG(0x3146), input->primary->segment_feature[i]);
	return ret;
}

int s7d_av1_picture_configure(struct regmap *map,
			      const struct s7d_av1_picture_input *input,
			      struct s7d_av1_reference_state *configured)
{
	struct s7d_av1_hw_picture_plan plan;
	struct s7d_av1_reference_state state;
	u32 i, value = 0;
	int ret;

	if (!map || !configured)
		return -EINVAL;
	ret = s7d_av1_picture_plan(input, &plan);
	if (ret)
		return ret;
	state = *input->state;
	ret = program_mc(map, &plan);
	if (!ret)
		ret = program_motion(map, input, &plan);
	if (!ret)
		ret = program_output(map, input, &plan);
	if (!ret)
		ret = program_filter(map, input, &plan);
	if (!ret)
		ret = regmap_write(map, HREG(0x316c), plan.skip_mode);
	if (!ret)
		ret = regmap_write(map, HREG(0x312d), 0x11b0 + plan.current_slot);
	if (!ret && state.segmentation_enabled)
		ret = regmap_read(map, HREG(0x3171), &value);
	if (!ret)
		ret = regmap_write(map, HREG(0x312e), value);
	for (i = 0; !ret && i < 8; i++) {
		if (state.segmentation_enabled)
			ret = regmap_read(map, HREG(0x3146), &state.segment_feature[i]);
		else
			state.segment_feature[i] = BIT(31) | (i << 22);
	}
	if (ret)
		return ret;
	*configured = state;
	return 0;
}
