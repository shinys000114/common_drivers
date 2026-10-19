// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* Reference mapping derived from Copyright (C) 2017 Amlogic, Inc. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "s7d-av1-dpb.h"

static int sequence_valid(const struct s7d_av1_dpb_sequence *sequence)
{
	if (!sequence || sequence->order_hint_bits > 8 ||
	    sequence->frame_id_bits > 16)
		return -EINVAL;
	if (!sequence->frame_id_bits)
		return sequence->delta_frame_id_bits ? -EINVAL : 0;
	if (sequence->delta_frame_id_bits < 2 ||
	    sequence->delta_frame_id_bits >= sequence->frame_id_bits)
		return -EINVAL;
	return 0;
}

static int dpb_valid(const struct s7d_av1_dpb *dpb)
{
	if (!dpb || !dpb->generation || dpb->slot_count < 9 ||
	    dpb->slot_count > S7D_AV1_DPB_MAX_SLOTS)
		return -EINVAL;
	return sequence_valid(&dpb->sequence);
}

static int map_mask(const struct s7d_av1_dpb *dpb, const u32 *map, u32 *mask)
{
	u32 i, owned = 0;
	int ret = dpb_valid(dpb);

	if (ret)
		return ret;
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
		u32 slot = map[i];

		if (slot == S7D_AV1_DPB_NO_SLOT)
			continue;
		if (slot >= dpb->slot_count || !dpb->backing_cookie[slot] ||
		    !dpb->picture[slot].decoded ||
		    dpb->picture[slot].id.slot != slot ||
		    dpb->picture[slot].id.backing_cookie != dpb->backing_cookie[slot])
			return -EPROTO;
		owned |= BIT(slot);
	}
	*mask = owned;
	return 0;
}

int s7d_av1_dpb_init(struct s7d_av1_dpb *dpb,
		     const struct s7d_av1_dpb_sequence *sequence,
		     u64 generation, u32 slot_count)
{
	u32 i;
	int ret = sequence_valid(sequence);

	if (ret || !dpb || !generation || slot_count < 9 ||
	    slot_count > S7D_AV1_DPB_MAX_SLOTS)
		return ret ?: -EINVAL;
	memset(dpb, 0, sizeof(*dpb));
	dpb->sequence = *sequence;
	dpb->generation = generation;
	dpb->slot_count = slot_count;
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++)
		dpb->map[i] = S7D_AV1_DPB_NO_SLOT;
	return 0;
}

int s7d_av1_dpb_bind(struct s7d_av1_dpb *dpb, u32 slot, u64 backing_cookie)
{
	u32 i;
	int ret = dpb_valid(dpb);

	if (ret || slot >= dpb->slot_count || !backing_cookie)
		return ret ?: -EINVAL;
	if (dpb->active)
		return -EBUSY;
	if (dpb->backing_cookie[slot])
		return dpb->backing_cookie[slot] == backing_cookie ? 0 : -EBUSY;
	for (i = 0; i < dpb->slot_count; i++)
		if (dpb->backing_cookie[i] == backing_cookie)
			return -EEXIST;
	dpb->backing_cookie[slot] = backing_cookie;
	return 0;
}

static s32 relative_dist(u32 bits, u32 a, u32 b)
{
	u32 half = 1U << (bits - 1);
	u32 diff = a - b;

	return (s32)(diff & (half - 1)) - (s32)(diff & half);
}

struct ref_sort {
	s32 order;
	u32 map;
};

static int short_remap(const struct s7d_av1_dpb *dpb,
		       const struct s7d_av1_dpb_frame *frame,
		       const u32 *map, u32 *remap)
{
	static const u32 remaining[] = { 1, 2, 4, 5, 6 };
	struct ref_sort sorted[S7D_AV1_DPB_REFERENCES];
	u32 assigned = BIT(0) | BIT(3);
	s32 half = 1U << (dpb->sequence.order_hint_bits - 1);
	s32 first = 0, last = S7D_AV1_DPB_REFERENCES - 1;
	s32 back_first, back_last;
	u32 i, j;

	if (frame->last_ref >= S7D_AV1_DPB_REFERENCES ||
	    frame->golden_ref >= S7D_AV1_DPB_REFERENCES ||
	    map[frame->last_ref] == S7D_AV1_DPB_NO_SLOT ||
	    map[frame->golden_ref] == S7D_AV1_DPB_NO_SLOT)
		return -ENODATA;
	if (relative_dist(dpb->sequence.order_hint_bits,
			  dpb->picture[map[frame->last_ref]].order_hint,
			  frame->order_hint) >= 0 ||
	    relative_dist(dpb->sequence.order_hint_bits,
			  dpb->picture[map[frame->golden_ref]].order_hint,
			  frame->order_hint) >= 0)
		return -EBADMSG;
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
		struct ref_sort entry = { .map = i, .order = -1 };

		if (map[i] != S7D_AV1_DPB_NO_SLOT)
			entry.order = half + relative_dist(dpb->sequence.order_hint_bits,
					 dpb->picture[map[i]].order_hint,
					 frame->order_hint);
		j = i;
		while (j && (sorted[j - 1].order > entry.order ||
		       (sorted[j - 1].order == entry.order &&
			 sorted[j - 1].map > entry.map))) {
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = entry;
	}
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
		if (sorted[i].order == -1) {
			first++;
			continue;
		}
		if (sorted[i].order >= half) {
			last = i - 1;
			break;
		}
	}
	back_first = last + 1;
	back_last = S7D_AV1_DPB_REFERENCES - 1;
	remap[0] = frame->last_ref;
	remap[3] = frame->golden_ref;
	if (back_first <= back_last) {
		remap[6] = sorted[back_last--].map;
		assigned |= BIT(6);
	}
	if (back_first <= back_last) {
		remap[4] = sorted[back_first++].map;
		assigned |= BIT(4);
	}
	if (back_first <= back_last) {
		remap[5] = sorted[back_first].map;
		assigned |= BIT(5);
	}
	for (i = 0; i < ARRAY_SIZE(remaining); i++) {
		u32 ref = remaining[i];

		if (assigned & BIT(ref))
			continue;
		while (first <= last &&
		       (sorted[last].map == frame->last_ref ||
			sorted[last].map == frame->golden_ref))
			last--;
		if (first > last)
			break;
		remap[ref] = sorted[last--].map;
		assigned |= BIT(ref);
	}
	for (; i < ARRAY_SIZE(remaining); i++)
		if (!(assigned & BIT(remaining[i])))
			remap[remaining[i]] = sorted[first].map;
	return 0;
}

static int frame_ids(const struct s7d_av1_dpb *dpb,
		     const struct s7d_av1_dpb_frame *frame, u32 *map)
{
	u32 bits = dpb->sequence.frame_id_bits;
	u32 mask, distance, window, i;

	if (!bits)
		return 0;
	mask = (1U << bits) - 1;
	window = 1U << dpb->sequence.delta_frame_id_bits;
	if (frame->current_frame_id > mask)
		return -EBADMSG;
	if (dpb->previous_frame_id_valid &&
	    !(frame->frame_type == S7D_AV1_KEY_FRAME && frame->show_frame)) {
		distance = (frame->current_frame_id - dpb->previous_frame_id) & mask;
		if (!distance || distance >= (1U << (bits - 1)))
			return -EBADMSG;
	}
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
		if (map[i] == S7D_AV1_DPB_NO_SLOT)
			continue;
		distance = (frame->current_frame_id - dpb->picture[map[i]].frame_id) & mask;
		if ((frame->frame_type == S7D_AV1_KEY_FRAME && frame->show_frame) ||
		    distance > window)
			map[i] = S7D_AV1_DPB_NO_SLOT;
	}
	return 0;
}

static int existing_plan(const struct s7d_av1_dpb *dpb,
			 const struct s7d_av1_dpb_frame *frame,
			 struct s7d_av1_dpb_plan *plan)
{
	const struct s7d_av1_dpb_picture *picture;
	u32 slot, i;

	if (!dpb->started || frame->existing_slot >= S7D_AV1_DPB_REFERENCES)
		return -ENODATA;
	slot = dpb->map[frame->existing_slot];
	if (slot == S7D_AV1_DPB_NO_SLOT)
		return -ENODATA;
	picture = &dpb->picture[slot];
	if (!picture->showable)
		return -EBADMSG;
	if (dpb->sequence.frame_id_bits) {
		if (!frame->display_frame_id_valid)
			return -EOPNOTSUPP;
		if (frame->display_frame_id != picture->frame_id)
			return -EBADMSG;
	}
	plan->current_picture = *picture;
	plan->output = true;
	plan->consume_key_showable = picture->frame_type == S7D_AV1_KEY_FRAME;
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
		plan->next_map[i] = plan->consume_key_showable ? slot : dpb->map[i];
		if (plan->next_map[i] != S7D_AV1_DPB_NO_SLOT)
			plan->next_mask |= BIT(plan->next_map[i]);
	}
	return 0;
}

static int decoded_plan(const struct s7d_av1_dpb *dpb,
			const struct s7d_av1_dpb_frame *frame,
			u32 current_slot, struct s7d_av1_dpb_plan *plan)
{
	u32 map[S7D_AV1_DPB_REFERENCES];
	u32 refresh, i;
	bool intra, resilient;
	int ret;

	if (frame->frame_type > S7D_AV1_SWITCH_FRAME || !frame->width ||
	    !frame->height || !frame->decode_width ||
	    frame->decode_width > frame->width ||
	    (frame->bit_depth != 8 && frame->bit_depth != 10) ||
	    current_slot >= dpb->slot_count || !dpb->backing_cookie[current_slot])
		return -EINVAL;
	if (plan->old_mask & BIT(current_slot))
		return -EBUSY;
	intra = frame->frame_type == S7D_AV1_KEY_FRAME ||
		frame->frame_type == S7D_AV1_INTRA_ONLY_FRAME;
	if (!dpb->started && !intra)
		return -ENODATA;
	if (dpb->sequence.order_hint_bits &&
	    frame->order_hint >= (1U << dpb->sequence.order_hint_bits))
		return -EBADMSG;
	if (!intra && frame->short_signaling && !dpb->sequence.order_hint_bits)
		return -EBADMSG;
	refresh = frame->refresh_flags;
	if (frame->frame_type == S7D_AV1_SWITCH_FRAME ||
	    (frame->frame_type == S7D_AV1_KEY_FRAME && frame->show_frame))
		refresh = 0xff;
	if (refresh > 0xff)
		return -EBADMSG;
	if (frame->frame_type == S7D_AV1_INTRA_ONLY_FRAME && refresh == 0xff)
		return -EBADMSG;
	memcpy(map, dpb->map, sizeof(map));
	ret = frame_ids(dpb, frame, map);
	if (ret)
		return ret;
	resilient = frame->error_resilient || frame->frame_type == S7D_AV1_SWITCH_FRAME ||
		(frame->frame_type == S7D_AV1_KEY_FRAME && frame->show_frame);
	if (!intra && !resilient && frame->primary_ref > S7D_AV1_DPB_PRIMARY_NONE)
		return -EBADMSG;
	if ((!intra || refresh != 0xff) && resilient &&
	    dpb->sequence.order_hint_bits)
		for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
			if (frame->ref_order_hint[i] >=
			    (1U << dpb->sequence.order_hint_bits))
				return -EBADMSG;
			if (map[i] != S7D_AV1_DPB_NO_SLOT &&
			    dpb->picture[map[i]].order_hint != frame->ref_order_hint[i])
				map[i] = S7D_AV1_DPB_NO_SLOT;
		}
	if (!intra) {
		if (frame->short_signaling) {
			ret = short_remap(dpb, frame, map, plan->remap);
			if (ret)
				return ret;
		} else {
			memcpy(plan->remap, frame->remap, sizeof(plan->remap));
		}
		for (i = 0; i < S7D_AV1_DPB_FRAME_REFERENCES; i++) {
			u32 logical = plan->remap[i], slot;

			if (logical >= S7D_AV1_DPB_REFERENCES)
				return -EBADMSG;
			slot = map[logical];
			if (slot == S7D_AV1_DPB_NO_SLOT)
				return -ENODATA;
			plan->reference[i] = dpb->picture[slot];
			if (dpb->sequence.frame_id_bits) {
				u32 mask = (1U << dpb->sequence.frame_id_bits) - 1;
				u32 delta = frame->delta_frame_id_minus_1[i];

				if (delta >= (1U << dpb->sequence.delta_frame_id_bits) ||
				    ((frame->current_frame_id - delta - 1) & mask) !=
				    plan->reference[i].frame_id)
					return -EBADMSG;
			}
			if (dpb->sequence.order_hint_bits &&
			    relative_dist(dpb->sequence.order_hint_bits,
					  plan->reference[i].order_hint,
					  frame->order_hint) > 0)
				plan->sign_bias_mask |= BIT(i);
		}
		if (!resilient && frame->primary_ref != S7D_AV1_DPB_PRIMARY_NONE)
			plan->primary_ref = frame->primary_ref;
	}
	plan->current_picture.id.slot = current_slot;
	plan->current_picture.id.backing_cookie = dpb->backing_cookie[current_slot];
	plan->current_picture.width = frame->width;
	plan->current_picture.height = frame->height;
	plan->current_picture.decode_width = frame->decode_width;
	plan->current_picture.bit_depth = frame->bit_depth;
	plan->current_picture.frame_type = frame->frame_type;
	plan->current_picture.frame_id = dpb->sequence.frame_id_bits ? frame->current_frame_id : 0;
	plan->current_picture.order_hint = dpb->sequence.order_hint_bits ? frame->order_hint : 0;
	plan->current_picture.showable = frame->show_frame ?
		frame->frame_type != S7D_AV1_KEY_FRAME : frame->showable_frame;
	for (i = 0; i < S7D_AV1_DPB_FRAME_REFERENCES; i++)
		plan->current_picture.ref_order_hint[i] = plan->reference[i].order_hint;
	for (i = 0; i < S7D_AV1_DPB_REFERENCES; i++) {
		plan->next_map[i] = refresh & BIT(i) ? current_slot : map[i];
		if (plan->next_map[i] != S7D_AV1_DPB_NO_SLOT)
			plan->next_mask |= BIT(plan->next_map[i]);
	}
	plan->decode = true;
	plan->output = frame->show_frame;
	return 0;
}

int s7d_av1_dpb_begin(struct s7d_av1_dpb *dpb,
		      const struct s7d_av1_dpb_frame *frame, u32 current_slot,
		      const struct s7d_av1_dpb_metadata *metadata,
		      struct s7d_av1_dpb_plan *plan)
{
	struct s7d_av1_dpb_plan next = { .primary_ref = S7D_AV1_DPB_PRIMARY_NONE };
	u32 i;
	int ret;

	if (!frame || !metadata || !metadata->input_token || !plan)
		return -EINVAL;
	ret = map_mask(dpb, dpb ? dpb->map : NULL, &next.old_mask);
	if (ret)
		return ret;
	if (dpb->active || dpb->output_pending)
		return -EBUSY;
	if (dpb->serial == U64_MAX)
		return -EOVERFLOW;
	for (i = 0; i < S7D_AV1_DPB_FRAME_REFERENCES; i++) {
		next.reference[i].id.slot = S7D_AV1_DPB_NO_SLOT;
		next.remap[i] = S7D_AV1_DPB_NO_SLOT;
	}
	if (frame->show_existing) {
		if (current_slot != S7D_AV1_DPB_NO_SLOT)
			return -EINVAL;
		ret = existing_plan(dpb, frame, &next);
	} else {
		ret = decoded_plan(dpb, frame, current_slot, &next);
	}
	if (ret)
		return ret;
	next.generation = dpb->generation;
	next.serial = dpb->serial + 1;
	next.base_revision = dpb->revision;
	next.metadata = *metadata;
	dpb->pending = next;
	dpb->serial = next.serial;
	dpb->active = true;
	*plan = next;
	return 0;
}

static int pending_matches(const struct s7d_av1_dpb *dpb, u64 serial,
			   u64 input_token)
{
	int ret = dpb_valid(dpb);

	if (ret || !serial || !input_token)
		return ret ?: -EINVAL;
	if (!dpb->active || dpb->pending.serial != serial ||
	    dpb->pending.metadata.input_token != input_token ||
	    dpb->pending.generation != dpb->generation ||
	    dpb->pending.base_revision != dpb->revision)
		return -ESTALE;
	return 0;
}

int s7d_av1_dpb_complete(struct s7d_av1_dpb *dpb, u64 serial,
			 u64 input_token, bool picture_done, bool physical_idle)
{
	struct s7d_av1_dpb_plan *plan;
	int ret = pending_matches(dpb, serial, input_token);

	if (ret)
		return ret;
	if (!physical_idle)
		return -EBUSY;
	plan = &dpb->pending;
	if (plan->decode && !picture_done)
		return -EIO;
	plan->current_picture.decoded = true;
	if (plan->consume_key_showable)
		plan->current_picture.showable = false;
	dpb->picture[plan->current_picture.id.slot] = plan->current_picture;
	memcpy(dpb->map, plan->next_map, sizeof(dpb->map));
	if (plan->decode || plan->consume_key_showable) {
		dpb->previous_frame_id = plan->current_picture.frame_id;
		dpb->previous_frame_id_valid = !!dpb->sequence.frame_id_bits;
	}
	if (plan->output) {
		dpb->output = (struct s7d_av1_dpb_output) {
			.generation = dpb->generation,
			.serial = plan->serial,
			.metadata = plan->metadata,
			.picture = plan->current_picture,
		};
		dpb->output_pending = true;
	}
	dpb->started = true;
	dpb->revision = plan->serial;
	dpb->active = false;
	memset(plan, 0, sizeof(*plan));
	return 0;
}

int s7d_av1_dpb_abort(struct s7d_av1_dpb *dpb, u64 serial,
		      u64 input_token, bool physical_idle)
{
	int ret = pending_matches(dpb, serial, input_token);

	if (ret)
		return ret;
	if (!physical_idle)
		return -EBUSY;
	memset(&dpb->pending, 0, sizeof(dpb->pending));
	dpb->active = false;
	return 0;
}

int s7d_av1_dpb_peek_output(const struct s7d_av1_dpb *dpb,
			    struct s7d_av1_dpb_output *output)
{
	int ret = dpb_valid(dpb);

	if (ret || !output)
		return ret ?: -EINVAL;
	if (!dpb->output_pending)
		return -EAGAIN;
	*output = dpb->output;
	return 0;
}

int s7d_av1_dpb_output_committed(struct s7d_av1_dpb *dpb,
				 const struct s7d_av1_dpb_output *output)
{
	int ret = dpb_valid(dpb);

	if (ret || !output)
		return ret ?: -EINVAL;
	if (!dpb->output_pending || output->generation != dpb->generation ||
	    output->serial != dpb->output.serial ||
	    output->metadata.input_token != dpb->output.metadata.input_token ||
	    output->picture.id.slot != dpb->output.picture.id.slot ||
	    output->picture.id.backing_cookie != dpb->output.picture.id.backing_cookie)
		return -ESTALE;
	dpb->output_pending = false;
	memset(&dpb->output, 0, sizeof(dpb->output));
	return 0;
}

int s7d_av1_dpb_owned_mask(const struct s7d_av1_dpb *dpb, u32 *mask)
{
	u32 value;
	int ret = map_mask(dpb, dpb ? dpb->map : NULL, &value);

	if (ret || !mask)
		return ret ?: -EINVAL;
	if (dpb->active)
		value |= BIT(dpb->pending.current_picture.id.slot);
	if (dpb->output_pending)
		value |= BIT(dpb->output.picture.id.slot);
	*mask = value;
	return 0;
}

int s7d_av1_dpb_reset(struct s7d_av1_dpb *dpb, u64 new_generation,
		      bool physical_idle, u32 *unbound_mask)
{
	struct s7d_av1_dpb_sequence sequence;
	u32 slot_count, mask = 0, i;
	int ret = dpb_valid(dpb);

	if (ret || !unbound_mask || new_generation <= dpb->generation)
		return ret ?: -EINVAL;
	if (!physical_idle)
		return -EBUSY;
	for (i = 0; i < dpb->slot_count; i++)
		if (dpb->backing_cookie[i])
			mask |= BIT(i);
	sequence = dpb->sequence;
	slot_count = dpb->slot_count;
	ret = s7d_av1_dpb_init(dpb, &sequence, new_generation, slot_count);
	if (!ret)
		*unbound_mask = mask;
	return ret;
}

int s7d_av1_dpb_release(struct s7d_av1_dpb *dpb, bool physical_idle,
			u32 *unbound_mask)
{
	u32 mask = 0, i;
	int ret = dpb_valid(dpb);

	if (ret || !unbound_mask)
		return ret ?: -EINVAL;
	if (!physical_idle)
		return -EBUSY;
	for (i = 0; i < dpb->slot_count; i++)
		if (dpb->backing_cookie[i])
			mask |= BIT(i);
	memset(dpb, 0, sizeof(*dpb));
	*unbound_mask = mask;
	return 0;
}
