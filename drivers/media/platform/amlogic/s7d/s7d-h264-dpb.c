// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "s7d-codec-types.h"
#include "s7d-h264-dpb.h"

#define H264_MAX_REFERENCES 16U

struct s7d_h264_picture {
	u32 frame_num;
	s32 poc;
	s32 top_poc;
	s32 bottom_poc;
	s32 long_term_index;
	s32 mv_slot;
	u64 write_token;
	u64 input_token;
	u64 timestamp_ns;
	struct s7d_codec_timestamp_metadata timestamp_meta;
	u32 flags;
	bool occupied;
	bool complete;
	bool output;
	bool reference;
	bool direct_8x8;
};

struct s7d_h264_marking {
	bool reference[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	s32 long_term_index[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	s32 max_long_term_index;
	bool reset_poc;
};

struct s7d_h264_poc_state {
	s32 msb;
	u32 lsb;
	u32 frame_num;
	u32 frame_offset;
	u32 reference_frame_num;
	u32 poc_type;
	u32 log2_frame_num;
	u32 log2_poc_lsb;
	bool initialized;
};

struct s7d_h264_dpb {
	struct s7d_codec_session *session;
	struct s7d_codec_layout layout;
	struct s7d_h264_capture_binding bindings[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_h264_picture pictures[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_h264_marking marking;
	struct s7d_h264_poc_state previous;
	struct s7d_h264_poc_state next;
	struct s7d_h264_slice current_slice;
	struct s7d_h264_picture_plan scratch_plan;
	u32 count;
	u32 mv_slots;
	bool mv_used[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	u32 max_references;
	u32 reorder_limit;
	u32 sequence;
	u32 last_mb;
	s32 max_long_term_index;
	s32 last_output_poc;
	int active_slot;
	int failed;
	bool output_started;
};

static int validate_slice(const struct s7d_h264_dpb *dpb,
			  const struct s7d_h264_slice *slice)
{
	u32 max_frame_num, i;

	if (slice->type > S7D_H264_SLICE_B || slice->log2_frame_num < 4 ||
	    slice->log2_frame_num > 16 || slice->max_references != dpb->max_references ||
	    slice->mmco_count > S7D_H264_MMCO_SIZE ||
	    slice->first_mb >= (dpb->layout.coded_width / 16U) *
			      (dpb->layout.coded_height / 16U))
		return -EINVAL;
	if (slice->poc_type != 0 && slice->poc_type != 2)
		return -EOPNOTSUPP;
	max_frame_num = 1U << slice->log2_frame_num;
	if (slice->frame_num >= max_frame_num ||
	    (slice->poc_type == 0 && (slice->log2_poc_lsb < 4 ||
				     slice->log2_poc_lsb > 16 ||
				     slice->poc_lsb >= (1U << slice->log2_poc_lsb))))
		return -EBADMSG;
	if (slice->idr && (!slice->reference || slice->frame_num ||
			   slice->type != S7D_H264_SLICE_I || slice->mmco_count))
		return -EBADMSG;
	if (!slice->reference && slice->mmco_count)
		return -EBADMSG;
	if (!slice->idr && (slice->no_output_of_prior_pics || slice->long_term_reference))
		return -EBADMSG;
	for (i = 0; i < 2; i++) {
		if (slice->active[i] > S7D_H264_REF_LIST_SIZE ||
		    slice->reorder_count[i] > S7D_H264_REF_LIST_SIZE)
			return -EBADMSG;
	}
	if ((slice->type == S7D_H264_SLICE_I && (slice->active[0] || slice->active[1])) ||
	    (slice->type == S7D_H264_SLICE_P && (!slice->active[0] || slice->active[1])) ||
	    (slice->type == S7D_H264_SLICE_B && (!slice->active[0] || !slice->active[1])))
		return -EBADMSG;
	return 0;
}

static int calculate_poc(const struct s7d_h264_dpb *dpb,
			 const struct s7d_h264_slice *slice,
			 struct s7d_h264_picture *picture,
			 struct s7d_h264_poc_state *next)
{
	const struct s7d_h264_poc_state *prev = &dpb->previous;
	u32 max_frame_num = 1U << slice->log2_frame_num, expected;
	s64 msb = prev->msb, top, bottom, offset = prev->frame_offset;
	s64 limit;

	if (!prev->initialized && !slice->idr)
		return -EOPNOTSUPP;
	if (!slice->idr && (slice->poc_type != prev->poc_type ||
			   slice->log2_frame_num != prev->log2_frame_num ||
			   (slice->poc_type == 0 && slice->log2_poc_lsb != prev->log2_poc_lsb)))
		return -EOPNOTSUPP;
	if (!slice->idr) {
		expected = (prev->reference_frame_num + 1U) % max_frame_num;
		if (slice->frame_num != expected &&
		    (slice->reference || slice->frame_num != prev->reference_frame_num))
			return -EOPNOTSUPP;
	}
	*next = *prev;
	if (slice->poc_type == 0) {
		u32 old_lsb = slice->idr ? 0 : prev->lsb;

		limit = 1U << slice->log2_poc_lsb;
		if (slice->idr)
			msb = 0;
		if (slice->poc_lsb < old_lsb && old_lsb - slice->poc_lsb >= limit / 2)
			msb += limit;
		else if (slice->poc_lsb > old_lsb && slice->poc_lsb - old_lsb > limit / 2)
			msb -= limit;
		top = msb + slice->poc_lsb;
		bottom = top + slice->delta_bottom;
		if (slice->reference) {
			if (msb < INT_MIN || msb > INT_MAX)
				return -EOVERFLOW;
			next->msb = msb;
			next->lsb = slice->poc_lsb;
		}
	} else {
		if (slice->idr)
			offset = 0;
		else if (slice->frame_num < prev->frame_num)
			offset += max_frame_num;
		if (offset > U32_MAX)
			return -EOVERFLOW;
		next->frame_offset = offset;
		top = slice->idr ? 0 : 2 * (offset + slice->frame_num) - !slice->reference;
		bottom = top;
	}
	if (top < INT_MIN || top > INT_MAX || bottom < INT_MIN || bottom > INT_MAX)
		return -EOVERFLOW;
	picture->top_poc = top;
	picture->bottom_poc = bottom;
	picture->poc = min(top, bottom);
	next->frame_num = slice->frame_num;
	if (slice->reference)
		next->reference_frame_num = slice->frame_num;
	next->poc_type = slice->poc_type;
	next->log2_frame_num = slice->log2_frame_num;
	next->log2_poc_lsb = slice->log2_poc_lsb;
	next->initialized = true;
	return 0;
}

static s32 picture_number(const struct s7d_h264_picture *picture,
			  const struct s7d_h264_slice *slice)
{
	return picture->frame_num > slice->frame_num ?
		(s32)picture->frame_num - (s32)(1U << slice->log2_frame_num) :
		(s32)picture->frame_num;
}

static int find_reference(const struct s7d_h264_dpb *dpb,
			  const struct s7d_h264_marking *marking,
			  const struct s7d_h264_slice *slice,
			  bool long_term, s32 number)
{
	u32 i;

	for (i = 0; i < dpb->count; i++) {
		if (!dpb->pictures[i].occupied || !marking->reference[i])
			continue;
		if (long_term && number >= 0 && marking->long_term_index[i] == number)
			return i;
		if (!long_term && marking->long_term_index[i] < 0 &&
		    picture_number(&dpb->pictures[i], slice) == number)
			return i;
	}
	return -EBADMSG;
}

static void clear_long_term(const struct s7d_h264_dpb *dpb,
			    struct s7d_h264_marking *marking, u32 index)
{
	u32 i;

	for (i = 0; i < dpb->count; i++)
		if (marking->reference[i] && marking->long_term_index[i] == (s32)index)
			marking->reference[i] = false;
}

static int plan_marking(const struct s7d_h264_dpb *dpb,
			const struct s7d_h264_slice *slice, u32 active_slot,
			struct s7d_h264_marking *marking)
{
	u32 i, references = 0, max_references = max(1U, dpb->max_references);
	s32 oldest = INT_MAX, number;
	int found = -1;

	memset(marking, 0, sizeof(*marking));
	marking->max_long_term_index = dpb->max_long_term_index;
	for (i = 0; i < dpb->count; i++) {
		marking->reference[i] = dpb->pictures[i].reference;
		marking->long_term_index[i] = dpb->pictures[i].long_term_index;
	}
	if (slice->idr) {
		memset(marking->reference, 0, sizeof(marking->reference));
		marking->max_long_term_index = slice->long_term_reference ? 0 : -1;
	} else if (slice->reference && !slice->mmco_count) {
		for (i = 0; i < dpb->count; i++) {
			if (!marking->reference[i])
				continue;
			references++;
			number = picture_number(&dpb->pictures[i], slice);
			if (marking->long_term_index[i] < 0 && number < oldest) {
				oldest = number;
				found = i;
			}
		}
		if (references >= max_references) {
			if (found < 0)
				return -EBADMSG;
			marking->reference[found] = false;
		}
	}
	for (i = 0; i < slice->mmco_count; i++) {
		const struct s7d_h264_mmco *command = &slice->mmco[i];
		u32 operation = command->operation;

		if (!operation || operation > 6 || marking->reset_poc)
			return -EBADMSG;
		if (operation == 1 || operation == 3) {
			if (command->difference_minus1 >= (1U << slice->log2_frame_num))
				return -EBADMSG;
			number = (s32)slice->frame_num - (s32)(command->difference_minus1 + 1);
			found = find_reference(dpb, marking, slice, false, number);
			if (found < 0)
				return found;
		} else if (operation == 2) {
			if (command->long_term_index >= H264_MAX_REFERENCES)
				return -EBADMSG;
			found = find_reference(dpb, marking, slice, true,
					       command->long_term_index);
			if (found < 0)
				return found;
		}
		switch (operation) {
		case 1:
		case 2:
			marking->reference[found] = false;
			break;
		case 3:
		case 6:
			if (marking->max_long_term_index < 0 ||
			    command->long_term_index > (u32)marking->max_long_term_index)
				return -EBADMSG;
			clear_long_term(dpb, marking, command->long_term_index);
			if (operation == 6)
				found = active_slot;
			marking->reference[found] = true;
			marking->long_term_index[found] = command->long_term_index;
			break;
		case 4:
			if (command->long_term_index > max_references)
				return -EBADMSG;
			marking->max_long_term_index = (s32)command->long_term_index - 1;
			for (found = 0; found < (int)dpb->count; found++)
				if (marking->long_term_index[found] > marking->max_long_term_index)
					marking->reference[found] = false;
			break;
		case 5:
			memset(marking->reference, 0, sizeof(marking->reference));
			marking->max_long_term_index = -1;
			marking->reset_poc = true;
			break;
		}
	}
	marking->reference[active_slot] = slice->reference;
	if (slice->idr)
		marking->long_term_index[active_slot] = slice->long_term_reference ? 0 : -1;
	else if (!slice->mmco_count || marking->long_term_index[active_slot] < 0)
		marking->long_term_index[active_slot] = -1;
	if (marking->reference[active_slot] && marking->long_term_index[active_slot] >= 0 &&
	    marking->long_term_index[active_slot] > marking->max_long_term_index)
		return -EBADMSG;
	references = 0;
	for (i = 0; i < dpb->count; i++) {
		if (!marking->reference[i])
			continue;
		references++;
		if (i != active_slot && marking->long_term_index[i] < 0 &&
		    dpb->pictures[i].frame_num == (marking->reset_poc ? 0 : slice->frame_num) &&
		    marking->long_term_index[active_slot] < 0)
			return -EBADMSG;
	}
	return references > max_references ? -EBADMSG : 0;
}

static bool reference_before(const struct s7d_h264_dpb *dpb,
			     const struct s7d_h264_slice *slice, s32 poc,
			     unsigned int list, u32 left, u32 right)
{
	const struct s7d_h264_picture *a = &dpb->pictures[left];
	const struct s7d_h264_picture *b = &dpb->pictures[right];
	bool a_first, b_first;

	if (a->long_term_index >= 0 || b->long_term_index >= 0) {
		if (a->long_term_index < 0 || b->long_term_index < 0)
			return a->long_term_index < 0;
		return a->long_term_index < b->long_term_index;
	}
	if (slice->type == S7D_H264_SLICE_P)
		return picture_number(a, slice) > picture_number(b, slice);
	a_first = list ? a->poc > poc : a->poc < poc;
	b_first = list ? b->poc > poc : b->poc < poc;
	if (a_first != b_first)
		return a_first;
	return (list == a_first) ? a->poc < b->poc : a->poc > b->poc;
}

static int reorder_list(const struct s7d_h264_dpb *dpb,
			const struct s7d_h264_slice *slice, unsigned int which,
			u32 *list, u32 *count)
{
	struct s7d_h264_marking refs = { 0 };
	u32 i, j, n = *count, pred = slice->frame_num;
	u32 max_frame_num = 1U << slice->log2_frame_num, diff;
	s32 number;
	int found;

	for (i = 0; i < dpb->count; i++) {
		refs.reference[i] = dpb->pictures[i].reference;
		refs.long_term_index[i] = dpb->pictures[i].long_term_index;
	}
	for (i = 0; i < slice->reorder_count[which]; i++) {
		const struct s7d_h264_reorder *command = &slice->reorder[which][i];
		u32 w;

		if (i >= slice->active[which] || command->operation > 2)
			return -EBADMSG;
		if (command->operation < 2) {
			if (command->argument >= max_frame_num)
				return -EBADMSG;
			diff = command->argument + 1U;
			pred = command->operation ? (pred + diff) % max_frame_num :
				(pred + max_frame_num - diff) % max_frame_num;
			number = pred > slice->frame_num ?
				 (s32)pred - (s32)max_frame_num : (s32)pred;
			found = find_reference(dpb, &refs, slice, false, number);
		} else {
			if (command->argument >= H264_MAX_REFERENCES)
				return -EBADMSG;
			found = find_reference(dpb, &refs, slice, true, command->argument);
		}
		if (found < 0)
			return found;
		if (n == S7D_H264_REF_LIST_SIZE)
			n--;
		for (j = n; j > i; j--)
			list[j] = list[j - 1];
		list[i] = found;
		w = i + 1;
		for (j = i + 1; j <= n; j++)
			if (list[j] != (u32)found)
				list[w++] = list[j];
		n = w;
	}
	if (slice->active[which] > n)
		return -EBADMSG;
	*count = slice->active[which];
	return 0;
}

static int build_lists(const struct s7d_h264_dpb *dpb,
		       const struct s7d_h264_slice *slice, s32 poc,
		       struct s7d_h264_picture_plan *plan)
{
	u32 lists[2][S7D_H264_REF_LIST_SIZE + 1] = { 0 };
	u32 counts[2] = { 0 }, which, i, j;
	int ret;

	if (slice->type == S7D_H264_SLICE_I)
		return 0;
	for (which = 0; which < (slice->type == S7D_H264_SLICE_B ? 2U : 1U); which++) {
		for (i = 0; i < dpb->count; i++) {
			if (!dpb->pictures[i].occupied || !dpb->pictures[i].reference)
				continue;
			j = counts[which]++;
			while (j && reference_before(dpb, slice, poc, which, i,
						    lists[which][j - 1])) {
				lists[which][j] = lists[which][j - 1];
				j--;
			}
			lists[which][j] = i;
		}
	}
	if (slice->type == S7D_H264_SLICE_B && counts[0] > 1 &&
	    counts[0] == counts[1] &&
	    !memcmp(lists[0], lists[1], counts[0] * sizeof(u32))) {
		i = lists[1][0];
		lists[1][0] = lists[1][1];
		lists[1][1] = i;
	}
	for (which = 0; which < (slice->type == S7D_H264_SLICE_B ? 2U : 1U); which++) {
		ret = reorder_list(dpb, slice, which, lists[which], &counts[which]);
		if (ret)
			return ret;
		plan->list_count[which] = counts[which];
		for (i = 0; i < counts[which]; i++) {
			const struct s7d_h264_picture *p = &dpb->pictures[lists[which][i]];

			if (p->direct_8x8 != ((slice->mode_8x8_flags & 6U) == 6U))
				return -EOPNOTSUPP;
			plan->list[which][i] = (struct s7d_h264_picture_ref) {
				.slot = lists[which][i], .frame_num = p->frame_num,
				.poc = p->poc, .top_poc = p->top_poc,
				.bottom_poc = p->bottom_poc, .long_term_index = p->long_term_index,
				.mv_slot = p->mv_slot, .direct_8x8 = p->direct_8x8,
			};
		}
	}
	return 0;
}

static bool same_picture(const struct s7d_h264_slice *a,
			 const struct s7d_h264_slice *b)
{
	return a->frame_num == b->frame_num && a->idr == b->idr &&
		a->reference == b->reference && (!a->idr || a->idr_pic_id == b->idr_pic_id) &&
		a->poc_type == b->poc_type && a->log2_frame_num == b->log2_frame_num &&
		(a->poc_type != 0 || (a->poc_lsb == b->poc_lsb &&
		 a->delta_bottom == b->delta_bottom && a->log2_poc_lsb == b->log2_poc_lsb)) &&
		a->long_term_reference == b->long_term_reference &&
		a->no_output_of_prior_pics == b->no_output_of_prior_pics &&
		((a->mode_8x8_flags & 6U) == (b->mode_8x8_flags & 6U)) &&
		a->mmco_count == b->mmco_count &&
		!memcmp(a->mmco, b->mmco, a->mmco_count * sizeof(a->mmco[0]));
}

static u32 slice_flags(enum s7d_h264_slice_type type)
{
	if (type == S7D_H264_SLICE_P)
		return S7D_CODEC_FRAME_P;
	if (type == S7D_H264_SLICE_B)
		return S7D_CODEC_FRAME_B;
	return S7D_CODEC_FRAME_KEY;
}

int s7d_h264_dpb_create(struct s7d_codec_session *session,
			const struct s7d_h264_sps *sps,
			const struct s7d_codec_layout *layout,
			const struct s7d_h264_capture_binding *bindings,
			unsigned int count, unsigned int mv_slots,
			struct s7d_h264_dpb **out)
{
	struct s7d_codec_layout checked;
	struct s7d_h264_dpb *dpb;
	u32 i, j;
	int ret;

	if (!session || !sps || !layout || !bindings || !out)
		return -EINVAL;
	ret = s7d_h264_build_layout(sps, layout->format, layout->extra_surfaces, &checked);
	if (ret)
		return ret;
	if (memcmp(&checked, layout, sizeof(checked)) || count < layout->min_capture_buffers ||
	    count > S7D_CODEC_MAX_CAPTURE_BUFFERS || sps->max_num_ref_frames > H264_MAX_REFERENCES ||
	    mv_slots < max(1U, sps->max_num_ref_frames) + 1U ||
	    mv_slots > S7D_CODEC_MAX_CAPTURE_BUFFERS)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!bindings[i].surface || !bindings[i].generation)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (bindings[i].surface == bindings[j].surface ||
			    bindings[i].surface_id == bindings[j].surface_id ||
			    bindings[i].generation != bindings[j].generation)
				return -EINVAL;
	}
	dpb = kzalloc(sizeof(*dpb), GFP_KERNEL);
	if (!dpb)
		return -ENOMEM;
	s7d_codec_session_get(session);
	dpb->session = session;
	dpb->layout = *layout;
	memcpy(dpb->bindings, bindings, count * sizeof(*bindings));
	dpb->count = count;
	dpb->mv_slots = mv_slots;
	dpb->active_slot = -1;
	dpb->max_long_term_index = -1;
	dpb->max_references = sps->max_num_ref_frames;
	dpb->reorder_limit = sps->bitstream_restriction ?
			     sps->num_reorder_frames : layout->dpb_slots;
	for (i = 0; i < count; i++) {
		dpb->pictures[i].long_term_index = -1;
		dpb->pictures[i].mv_slot = -1;
	}
	*out = dpb;
	return 0;
}

int s7d_h264_dpb_prepare(struct s7d_h264_dpb *dpb,
			 const struct s7d_h264_slice *slice, u64 input_token,
			 u64 timestamp_ns,
			 const struct s7d_codec_timestamp_metadata *timestamp_meta,
			 struct s7d_h264_picture_plan *plan)
{
	struct s7d_h264_picture_plan *prepared;
	struct s7d_h264_marking marking;
	struct s7d_h264_poc_state next;
	struct s7d_h264_picture picture = { .long_term_index = -1, .mv_slot = -1 };
	u32 slot = 0, i;
	bool new_picture;
	u64 token;
	int ret;

	if (!dpb || !slice || !input_token || !timestamp_meta || !plan)
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	new_picture = dpb->active_slot < 0;
	prepared = &dpb->scratch_plan;
	memset(prepared, 0, sizeof(*prepared));
	ret = validate_slice(dpb, slice);
	if (ret)
		return ret;
	if (!new_picture) {
		slot = dpb->active_slot;
		if (!same_picture(&dpb->current_slice, slice) || slice->first_mb <= dpb->last_mb)
			return -EBADMSG;
		picture = dpb->pictures[slot];
	} else {
		if (slice->first_mb)
			return -EOPNOTSUPP;
		ret = calculate_poc(dpb, slice, &picture, &next);
		if (ret)
			return ret;
		picture.direct_8x8 = (slice->mode_8x8_flags & 6U) == 6U;
		if (slice->reference) {
			for (i = 0; i < dpb->mv_slots; i++)
				if (!dpb->mv_used[i])
					break;
			if (i == dpb->mv_slots)
				return -EAGAIN;
			picture.mv_slot = i;
		}
	}
	ret = build_lists(dpb, slice, picture.poc, prepared);
	if (ret)
		return ret;
	if (new_picture) {
		for (slot = 0; slot < dpb->count; slot++) {
			if (dpb->pictures[slot].occupied)
				continue;
			ret = plan_marking(dpb, slice, slot, &marking);
			if (ret)
				return ret;
			if (marking.reset_poc &&
			    ((s64)picture.top_poc - picture.poc > INT_MAX ||
			     (s64)picture.bottom_poc - picture.poc > INT_MAX))
				return -EOVERFLOW;
			ret = s7d_codec_surface_acquire(dpb->bindings[slot].surface, &token);
			if (!ret)
				break;
			if (ret != -EAGAIN)
				return ret;
		}
		if (slot == dpb->count)
			return -EAGAIN;
		picture.occupied = true;
		picture.frame_num = slice->frame_num;
		picture.write_token = token;
		picture.input_token = input_token;
		picture.timestamp_ns = timestamp_ns;
		picture.timestamp_meta = *timestamp_meta;
		picture.flags = slice_flags(slice->type);
		dpb->pictures[slot] = picture;
		if (picture.mv_slot >= 0)
			dpb->mv_used[picture.mv_slot] = true;
		dpb->active_slot = slot;
		ret = s7d_codec_surface_ref_get(dpb->bindings[slot].surface);
		if (ret) {
			dpb->failed = ret;
			return ret;
		}
		dpb->marking = marking;
		dpb->next = next;
		dpb->current_slice = *slice;
		prepared->new_picture = true;
	} else if (slice->type != S7D_H264_SLICE_I) {
		dpb->pictures[slot].flags &= ~S7D_CODEC_FRAME_KEY;
		dpb->pictures[slot].flags |= slice_flags(slice->type);
	}
	dpb->last_mb = slice->first_mb;
	prepared->type = slice->type;
	prepared->first_mb = slice->first_mb;
	prepared->frame_num = slice->frame_num;
	prepared->mode_8x8_flags = slice->mode_8x8_flags;
	prepared->reference = slice->reference;
	prepared->slot = slot;
	prepared->slot_count = dpb->count;
	prepared->surface = dpb->bindings[slot].surface;
	prepared->surface_id = dpb->bindings[slot].surface_id;
	prepared->generation = dpb->bindings[slot].generation;
	prepared->write_token = picture.write_token;
	prepared->input_token = picture.input_token;
	prepared->timestamp_ns = picture.timestamp_ns;
	prepared->timestamp_meta = picture.timestamp_meta;
	prepared->poc = picture.poc;
	prepared->top_poc = picture.top_poc;
	prepared->bottom_poc = picture.bottom_poc;
	prepared->mv_slot = picture.mv_slot;
	prepared->direct_8x8 = picture.direct_8x8;
	for (i = 0; i < dpb->count; i++) {
		const struct s7d_h264_picture *p = &dpb->pictures[i];

		if (!p->occupied)
			continue;
		prepared->table[i] = (struct s7d_h264_picture_table) {
			.picture = {
				.slot = i, .frame_num = p->frame_num, .poc = p->poc,
				.top_poc = p->top_poc, .bottom_poc = p->bottom_poc,
				.long_term_index = p->long_term_index, .mv_slot = p->mv_slot,
				.direct_8x8 = p->direct_8x8,
			},
			.active = true, .is_current = i == slot, .reference = p->reference,
		};
	}
	*plan = *prepared;
	return 0;
}

static int reclaim_pictures(struct s7d_h264_dpb *dpb)
{
	u32 i;
	int ret;

	for (i = 0; i < dpb->count; i++) {
		struct s7d_h264_picture *picture = &dpb->pictures[i];

		if (!picture->occupied || !picture->complete ||
		    !picture->output || picture->reference)
			continue;
		ret = s7d_codec_surface_ref_put(dpb->bindings[i].surface);
		if (ret)
			return ret;
		memset(picture, 0, sizeof(*picture));
		picture->long_term_index = -1;
		picture->mv_slot = -1;
	}
	return 0;
}

static int discard_prior_pictures(struct s7d_h264_dpb *dpb)
{
	u32 i;
	int ret;

	for (i = 0; i < dpb->count; i++) {
		struct s7d_h264_picture *picture = &dpb->pictures[i];

		if (!picture->occupied || !picture->complete || picture->output)
			continue;
		ret = s7d_codec_surface_discard(dpb->bindings[i].surface,
					       picture->write_token);
		if (ret)
			return ret;
		if (picture->mv_slot >= 0)
			dpb->mv_used[picture->mv_slot] = false;
		memset(picture, 0, sizeof(*picture));
		picture->long_term_index = -1;
		picture->mv_slot = -1;
	}
	return 0;
}

static int output_pictures(struct s7d_h264_dpb *dpb, bool flush)
{
	u32 i, pending, resident;
	int selected, ret;

	for (;;) {
		selected = -1;
		pending = 0;
		resident = 0;
		for (i = 0; i < dpb->count; i++) {
			const struct s7d_h264_picture *picture = &dpb->pictures[i];

			if (!picture->occupied || !picture->complete)
				continue;
			resident++;
			if (picture->output)
				continue;
			pending++;
			if (selected < 0 || picture->poc < dpb->pictures[selected].poc)
				selected = i;
		}
		if (!pending || (!flush && pending <= dpb->reorder_limit &&
				 resident <= dpb->layout.dpb_slots))
			return 0;
		if (dpb->output_started && dpb->pictures[selected].poc < dpb->last_output_poc)
			return -EBADMSG;
		{
			struct s7d_h264_picture *picture = &dpb->pictures[selected];
			struct s7d_codec_frame frame = {
				.surface_id = dpb->bindings[selected].surface_id,
				.generation = dpb->bindings[selected].generation,
				.write_token = picture->write_token,
				.timestamp_ns = picture->timestamp_ns,
				.timestamp_meta = picture->timestamp_meta,
				.sequence = dpb->sequence,
				.flags = picture->flags,
				.bytesused = { dpb->layout.size[0], dpb->layout.size[1] },
			};

			ret = s7d_codec_session_frame_ready(dpb->session, &frame);
			if (ret)
				return ret;
			picture->output = true;
			dpb->last_output_poc = picture->poc;
			dpb->output_started = true;
			dpb->sequence++;
		}
		ret = reclaim_pictures(dpb);
		if (ret)
			return ret;
	}
}

int s7d_h264_dpb_picture_done(struct s7d_h264_dpb *dpb, u32 slot,
			      u64 write_token, u32 flags)
{
	struct s7d_h264_picture *picture;
	u32 i;
	int ret;

	if (!dpb || (flags & ~S7D_CODEC_FRAME_ERROR))
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	if (dpb->active_slot < 0 || slot != (u32)dpb->active_slot ||
	    write_token != dpb->pictures[slot].write_token)
		return -ESTALE;
	ret = s7d_codec_surface_written(dpb->bindings[slot].surface, write_token);
	if (ret)
		goto failed;
	if (flags & S7D_CODEC_FRAME_ERROR) {
		ret = -EILSEQ;
		goto failed;
	}
	picture = &dpb->pictures[slot];
	/* Finish the old POC epoch before publishing IDR/MMCO5's reset picture. */
	if (dpb->current_slice.idr || dpb->marking.reset_poc) {
		if (dpb->current_slice.idr && dpb->current_slice.no_output_of_prior_pics)
			ret = discard_prior_pictures(dpb);
		else
			ret = output_pictures(dpb, true);
		if (ret)
			goto failed;
		dpb->output_started = false;
	}
	for (i = 0; i < dpb->count; i++) {
		dpb->pictures[i].reference = dpb->marking.reference[i];
		dpb->pictures[i].long_term_index = dpb->marking.long_term_index[i];
		if (!dpb->pictures[i].reference && dpb->pictures[i].mv_slot >= 0) {
			dpb->mv_used[dpb->pictures[i].mv_slot] = false;
			dpb->pictures[i].mv_slot = -1;
		}
	}
	dpb->max_long_term_index = dpb->marking.max_long_term_index;
	if (dpb->marking.reset_poc) {
		picture->top_poc -= picture->poc;
		picture->bottom_poc -= picture->poc;
		picture->poc = 0;
		picture->frame_num = 0;
		dpb->next.msb = 0;
		dpb->next.lsb = picture->top_poc;
		dpb->next.frame_num = 0;
		dpb->next.frame_offset = 0;
		dpb->next.reference_frame_num = 0;
	}
	picture->flags |= flags;
	picture->complete = true;
	dpb->previous = dpb->next;
	dpb->active_slot = -1;
	ret = reclaim_pictures(dpb);
	if (!ret)
		ret = output_pictures(dpb, false);
	if (!ret)
		return 0;
failed:
	dpb->failed = ret;
	return ret;
}

int s7d_h264_dpb_flush_output(struct s7d_h264_dpb *dpb)
{
	int ret;

	if (!dpb)
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	if (dpb->active_slot >= 0)
		return -EBUSY;
	ret = output_pictures(dpb, true);
	if (ret)
		dpb->failed = ret;
	return ret;
}

int s7d_h264_dpb_finish_sequence(struct s7d_h264_dpb *dpb, bool discard_prior)
{
	int ret;

	if (!dpb)
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	if (dpb->active_slot >= 0)
		return -EBUSY;
	ret = discard_prior ? discard_prior_pictures(dpb) : output_pictures(dpb, true);
	if (ret)
		dpb->failed = ret;
	return ret;
}

int s7d_h264_dpb_get_state(const struct s7d_h264_dpb *dpb,
			   const struct s7d_codec_session *original_session,
			   struct s7d_h264_dpb_state *state)
{
	struct s7d_h264_dpb_state snapshot = {};
	u32 i;

	if (!dpb || !original_session || !state)
		return -EINVAL;
	if (dpb->session != original_session)
		return -EXDEV;
	snapshot.generation = dpb->bindings[0].generation;
	snapshot.active_slot = dpb->active_slot;
	snapshot.publication_count = dpb->sequence;
	snapshot.failure = dpb->failed;
	if (dpb->active_slot >= 0) {
		const struct s7d_h264_picture *picture = &dpb->pictures[dpb->active_slot];

		snapshot.surface_id = dpb->bindings[dpb->active_slot].surface_id;
		snapshot.write_token = picture->write_token;
		snapshot.input_token = picture->input_token;
	}
	for (i = 0; i < dpb->count; i++) {
		const struct s7d_h264_picture *picture = &dpb->pictures[i];

		if (!picture->occupied)
			continue;
		if (picture->complete && !picture->output)
			snapshot.complete_unpublished++;
		if (picture->reference)
			snapshot.reference_count++;
	}
	*state = snapshot;
	return 0;
}

void s7d_h264_dpb_fail(struct s7d_h264_dpb *dpb, int error)
{
	if (dpb && !dpb->failed)
		dpb->failed = error < 0 ? error : -EIO;
}

int s7d_h264_dpb_close(struct s7d_h264_dpb *dpb)
{
	int ret;

	if (!dpb)
		return -EINVAL;
	ret = s7d_codec_session_stop(dpb->session);
	if (ret) {
		s7d_h264_dpb_fail(dpb, ret);
		return ret;
	}
	s7d_codec_session_put(dpb->session);
	kfree(dpb);
	return 0;
}
