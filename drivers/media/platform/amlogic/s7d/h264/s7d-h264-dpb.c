// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "s7d-codec-types.h"
#include "s7d-h264-dpb.h"

#define H264_MAX_REFERENCES 16U
/* Pictures fill [0, count); the "non-existing" frames of 8.2.5.2 sit above. */
#define H264_GAP_BASE S7D_CODEC_MAX_CAPTURE_BUFFERS
#define H264_ENTRIES (H264_GAP_BASE + H264_MAX_REFERENCES)
#define for_each_entry(dpb, i) \
	for ((i) = 0; (i) < H264_ENTRIES; \
	     (i) = (i) + 1 == (dpb)->count ? H264_GAP_BASE : (i) + 1)

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
	/* A frame_num gap entry: a reference with no surface, never output. */
	bool nonexisting;
	/* Decoded or concealment content that is never published. */
	bool hidden;
};

struct s7d_h264_marking {
	bool reference[H264_ENTRIES];
	s32 long_term_index[H264_ENTRIES];
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

/*
 * After a start at a recovery point (D.2.8) and no IDR: pictures before the
 * recovery point in decoding order, and those output before the start, are
 * decoded against whatever references exist and never published.
 */
struct s7d_h264_recovery {
	u32 frame_num;
	u32 frame_cnt;
	s32 poc;
	bool active;
	bool leading;
};

struct s7d_h264_dpb {
	struct s7d_codec_session *session;
	struct s7d_codec_layout layout;
	struct s7d_h264_capture_binding bindings[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_h264_picture pictures[H264_ENTRIES];
	struct s7d_h264_marking marking;
	struct s7d_h264_poc_state previous;
	struct s7d_h264_poc_state next;
	struct s7d_h264_recovery recovery;
	struct s7d_h264_recovery next_recovery;
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
	/* The active picture may name references from before a recovery start. */
	bool lenient;
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
		return -EBADMSG;
	if (slice->poc_type > 2)
		return -EOPNOTSUPP;
	if (slice->poc_type == 1 &&
	    (!slice->poc1 || slice->poc1->cycle > S7D_H264_POC_CYCLE_MAX))
		return -EINVAL;
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

/* 8.2.1.2: the expected count of a picture from its absolute frame number. */
static int expected_poc(const struct s7d_h264_poc1 *poc1, s64 absolute,
			bool reference, s64 *expected)
{
	s64 delta = 0, partial = 0, cycles, value = 0;
	u32 i, in_cycle;

	if (poc1->cycle && absolute > 0) {
		for (i = 0; i < poc1->cycle; i++)
			delta += poc1->offset[i];
		cycles = (absolute - 1) / poc1->cycle;
		in_cycle = (absolute - 1) % poc1->cycle;
		for (i = 0; i <= in_cycle; i++)
			partial += poc1->offset[i];
		if (check_mul_overflow(cycles, delta, &value) ||
		    check_add_overflow(value, partial, &value))
			return -EOVERFLOW;
	}
	if (!reference && check_add_overflow(value, (s64)poc1->non_ref, &value))
		return -EOVERFLOW;
	if (value < INT_MIN || value > INT_MAX)
		return -EOVERFLOW;
	*expected = value;
	return 0;
}

static int calculate_poc(const struct s7d_h264_dpb *dpb,
			 const struct s7d_h264_slice *slice,
			 struct s7d_h264_picture *picture,
			 struct s7d_h264_poc_state *next)
{
	const struct s7d_h264_poc_state *prev = &dpb->previous;
	u32 max_frame_num = 1U << slice->log2_frame_num, expected;
	struct s7d_h264_poc_state start;
	s64 msb, top, bottom, offset;
	s64 limit;

	if (!prev->initialized && !slice->idr) {
		if (!slice->recovery)
			return -EOPNOTSUPP;
		/* A recovery point starts the order count where it stands. */
		start = (struct s7d_h264_poc_state) {
			.lsb = slice->poc_lsb, .frame_num = slice->frame_num,
			.reference_frame_num = (slice->frame_num + max_frame_num - 1U) %
					       max_frame_num,
			.poc_type = slice->poc_type, .log2_frame_num = slice->log2_frame_num,
			.log2_poc_lsb = slice->log2_poc_lsb, .initialized = true,
		};
		prev = &start;
	}
	msb = prev->msb;
	offset = prev->frame_offset;
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
		if (slice->poc_type == 1) {
			s64 absolute = slice->poc1->cycle ? offset + slice->frame_num : 0;
			int ret;

			if (!slice->reference && absolute > 0)
				absolute--;
			ret = expected_poc(slice->poc1, absolute, slice->reference, &top);
			if (ret)
				return ret;
			top += slice->delta_poc[0];
			bottom = top + slice->poc1->top_to_bottom + slice->delta_poc[1];
		} else {
			top = slice->idr ? 0 : 2 * (offset + slice->frame_num) - !slice->reference;
			bottom = top;
		}
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

static bool frame_num_gap(const struct s7d_h264_poc_state *previous, bool idr,
			  u32 frame_num, u32 log2_frame_num)
{
	u32 max_frame_num = 1U << log2_frame_num;

	return !idr && previous->initialized && log2_frame_num == previous->log2_frame_num &&
	       frame_num != previous->reference_frame_num &&
	       frame_num != (previous->reference_frame_num + 1U) % max_frame_num;
}

static int reclaim_pictures(struct s7d_h264_dpb *dpb);
static int bump_picture(struct s7d_h264_dpb *dpb);

/* 8.2.5.3 before storing an inferred frame; @slice is the coded picture. */
static int slide_window(struct s7d_h264_dpb *dpb, const struct s7d_h264_slice *slice)
{
	u32 i, references = 0;
	s32 oldest = INT_MAX, number;
	int found = -1;

	for_each_entry(dpb, i) {
		const struct s7d_h264_picture *p = &dpb->pictures[i];

		if (!p->occupied || !p->reference)
			continue;
		references++;
		number = picture_number(p, slice);
		if (p->long_term_index < 0 && number < oldest) {
			oldest = number;
			found = i;
		}
	}
	if (references < max(1U, dpb->max_references))
		return 0;
	if (found < 0)
		return -EBADMSG;
	dpb->pictures[found].reference = false;
	if (dpb->pictures[found].mv_slot >= 0) {
		dpb->mv_used[dpb->pictures[found].mv_slot] = false;
		dpb->pictures[found].mv_slot = -1;
	}
	return 0;
}

/*
 * 8.2.5.2: store a "non-existing" short-term frame for each frame_num skipped
 * since the last reference picture, with sliding window marking, before the
 * coded picture is decoded. Only the last max_num_ref_frames survive the
 * window, so only those are stored; the FrameNumOffset still advances over
 * the whole gap. The inference holds whether or not this picture completes.
 */
static int fill_frame_num_gap(struct s7d_h264_dpb *dpb,
			      const struct s7d_h264_slice *slice)
{
	struct s7d_h264_poc_state *prev = &dpb->previous;
	u32 max_frame_num = 1U << slice->log2_frame_num;
	u32 gap, keep, i, n, frame_num, slot, stored = 0;
	s64 offset = prev->frame_offset, top, bottom;
	int ret;

	if (!frame_num_gap(prev, slice->idr, slice->frame_num, slice->log2_frame_num))
		return 0;
	/* Without gaps_in_frame_num_value_allowed_flag this is a lost picture. */
	if (!slice->frame_num_gaps || slice->poc_type != prev->poc_type)
		return -EBADMSG;
	gap = (slice->frame_num + max_frame_num - prev->reference_frame_num - 1U) %
	      max_frame_num;
	keep = min(gap, max(1U, dpb->max_references));
	frame_num = prev->frame_num;
	for (i = 0; i < gap; i++) {
		n = (prev->reference_frame_num + 1U + i) % max_frame_num;
		if (n < frame_num)
			offset += max_frame_num;
		frame_num = n;
		if (i < gap - keep)
			continue;
		if (slice->poc_type == 0) {
			/* No POC is defined (8.2.1); keep it after the last reference. */
			stored++;
			top = (s64)prev->msb + prev->lsb + 2 * (s64)stored;
			bottom = top;
		} else if (slice->poc_type == 1) {
			ret = expected_poc(slice->poc1, slice->poc1->cycle ? offset + n : 0,
					   true, &top);
			if (ret)
				return ret;
			bottom = top + slice->poc1->top_to_bottom;
		} else {
			top = 2 * (offset + n);
			bottom = top;
		}
		if (offset > U32_MAX || top < INT_MIN || top > INT_MAX ||
		    bottom < INT_MIN || bottom > INT_MAX)
			return -EOVERFLOW;
		ret = slide_window(dpb, slice);
		if (ret)
			return ret;
		for (slot = H264_GAP_BASE; slot < H264_ENTRIES; slot++)
			if (!dpb->pictures[slot].occupied || !dpb->pictures[slot].reference)
				break;
		if (slot == H264_ENTRIES)
			return -EBADMSG;
		dpb->pictures[slot] = (struct s7d_h264_picture) {
			.frame_num = n, .poc = min(top, bottom), .top_poc = top,
			.bottom_poc = bottom, .long_term_index = -1, .mv_slot = -1,
			.occupied = true, .complete = true, .output = true,
			.reference = true, .nonexisting = true,
		};
	}
	if (offset > U32_MAX)
		return -EOVERFLOW;
	prev->frame_num = frame_num;
	prev->frame_offset = offset;
	prev->reference_frame_num = frame_num;
	return reclaim_pictures(dpb);
}

/*
 * A P or B picture whose references are all gap entries still needs a picture
 * for the hardware to read: back the newest gap entry with a free surface. Its
 * content is whatever the buffer held; it is never published.
 */
static int back_gap_entry(struct s7d_h264_dpb *dpb, const struct s7d_h264_slice *slice)
{
	struct s7d_h264_picture *gap;
	u32 i, slot, pass, mv;
	s32 newest = INT_MIN;
	int found = -1, ret;
	u64 token;

	if (slice->type == S7D_H264_SLICE_I)
		return 0;
	for_each_entry(dpb, i) {
		const struct s7d_h264_picture *p = &dpb->pictures[i];

		if (!p->occupied || !p->reference)
			continue;
		if (!p->nonexisting)
			return 0;
		if (picture_number(p, slice) > newest) {
			newest = picture_number(p, slice);
			found = i;
		}
	}
	if (found < 0)
		return 0;
	for (mv = 0; mv < dpb->mv_slots; mv++)
		if (!dpb->mv_used[mv])
			break;
	if (mv == dpb->mv_slots)
		return -EAGAIN;
retry:
	for (pass = 0; pass < 2; pass++) {
		for (slot = 0; slot < dpb->count; slot++) {
			if (dpb->pictures[slot].occupied ||
			    (!pass && s7d_codec_surface_reading(dpb->bindings[slot].surface)))
				continue;
			ret = s7d_codec_surface_acquire(dpb->bindings[slot].surface, &token);
			if (!ret)
				break;
			if (ret != -EAGAIN)
				return ret;
		}
		if (slot < dpb->count)
			break;
	}
	if (slot == dpb->count) {
		ret = bump_picture(dpb);
		if (ret)
			return ret;
		goto retry;
	}
	ret = s7d_codec_surface_written(dpb->bindings[slot].surface, token);
	if (!ret)
		ret = s7d_codec_surface_ref_get(dpb->bindings[slot].surface);
	if (ret) {
		dpb->failed = ret;
		return ret;
	}
	gap = &dpb->pictures[found];
	dpb->pictures[slot] = *gap;
	dpb->pictures[slot].nonexisting = false;
	dpb->pictures[slot].hidden = true;
	dpb->pictures[slot].mv_slot = mv;
	dpb->pictures[slot].direct_8x8 = (slice->mode_8x8_flags & 6U) == 6U;
	dpb->pictures[slot].write_token = token;
	dpb->mv_used[mv] = true;
	memset(gap, 0, sizeof(*gap));
	gap->long_term_index = -1;
	gap->mv_slot = -1;
	return 0;
}

static int find_reference(const struct s7d_h264_dpb *dpb,
			  const struct s7d_h264_marking *marking,
			  const struct s7d_h264_slice *slice,
			  bool long_term, s32 number)
{
	u32 i;

	for_each_entry(dpb, i) {
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

	for_each_entry(dpb, i)
		if (marking->reference[i] && marking->long_term_index[i] == (s32)index)
			marking->reference[i] = false;
}

/* @lenient: references from before a recovery start may be missing. */
static int plan_marking(const struct s7d_h264_dpb *dpb,
			const struct s7d_h264_slice *slice, u32 active_slot,
			bool lenient, struct s7d_h264_marking *marking)
{
	u32 i, references = 0, max_references = max(1U, dpb->max_references);
	s32 oldest = INT_MAX, number;
	int found = -1;

	memset(marking, 0, sizeof(*marking));
	marking->max_long_term_index = dpb->max_long_term_index;
	for_each_entry(dpb, i) {
		marking->reference[i] = dpb->pictures[i].reference;
		marking->long_term_index[i] = dpb->pictures[i].long_term_index;
	}
	if (slice->idr) {
		memset(marking->reference, 0, sizeof(marking->reference));
		marking->max_long_term_index = slice->long_term_reference ? 0 : -1;
	} else if (slice->reference && !slice->mmco_count) {
		for_each_entry(dpb, i) {
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
			if (found < 0 && lenient)
				continue;
			if (found < 0)
				return found;
		} else if (operation == 2) {
			if (command->long_term_index >= H264_MAX_REFERENCES)
				return -EBADMSG;
			found = find_reference(dpb, marking, slice, true,
					       command->long_term_index);
			if (found < 0 && lenient)
				continue;
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
			for_each_entry(dpb, i)
				if (marking->long_term_index[i] > marking->max_long_term_index)
					marking->reference[i] = false;
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
	for_each_entry(dpb, i) {
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
			bool lenient, u32 *list, u32 *count)
{
	struct s7d_h264_marking refs = { 0 };
	u32 i, j, n = *count, pred = slice->frame_num;
	u32 max_frame_num = 1U << slice->log2_frame_num, diff;
	s32 number;
	int found;

	for_each_entry(dpb, i) {
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
		if (found < 0 && lenient)
			continue;
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
	if (slice->active[which] > n && (!lenient || !n))
		return -EBADMSG;
	/* Entries past the references there are repeat the last one. */
	for (; n < slice->active[which]; n++)
		list[n] = list[n - 1];
	*count = slice->active[which];
	return 0;
}

/*
 * The picture the hardware reads for list entry @index. A conformant stream
 * never predicts from a frame_num gap entry (8.2.5.2), but the list still
 * names it: give the hardware the real reference nearest to it in POC.
 */
static int list_picture(const struct s7d_h264_dpb *dpb, u32 index)
{
	const struct s7d_h264_picture *gap = &dpb->pictures[index];
	s64 distance, best = S64_MAX;
	int found = -EBADMSG;
	u32 i;

	if (!gap->nonexisting)
		return index;
	for (i = 0; i < dpb->count; i++) {
		const struct s7d_h264_picture *p = &dpb->pictures[i];

		if (!p->occupied || !p->reference)
			continue;
		distance = (s64)p->poc - gap->poc;
		distance = distance < 0 ? -distance : distance;
		if (distance < best) {
			best = distance;
			found = i;
		}
	}
	return found;
}

static int build_lists(const struct s7d_h264_dpb *dpb,
		       const struct s7d_h264_slice *slice, s32 poc, bool lenient,
		       struct s7d_h264_picture_plan *plan)
{
	u32 lists[2][S7D_H264_REF_LIST_SIZE + 1] = { 0 };
	u32 counts[2] = { 0 }, which, i, j;
	int ret;

	if (slice->type == S7D_H264_SLICE_I)
		return 0;
	for (which = 0; which < (slice->type == S7D_H264_SLICE_B ? 2U : 1U); which++) {
		for_each_entry(dpb, i) {
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
		ret = reorder_list(dpb, slice, which, lenient, lists[which], &counts[which]);
		if (ret)
			return ret;
		plan->list_count[which] = counts[which];
		for (i = 0; i < counts[which]; i++) {
			const struct s7d_h264_picture *p;
			int slot = list_picture(dpb, lists[which][i]);

			if (slot < 0)
				return slot;
			p = &dpb->pictures[slot];
			if (p->direct_8x8 != ((slice->mode_8x8_flags & 6U) == 6U))
				return -EOPNOTSUPP;
			plan->list[which][i] = (struct s7d_h264_picture_ref) {
				.slot = slot, .frame_num = p->frame_num,
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
		(a->poc_type != 1 || (a->delta_poc[0] == b->delta_poc[0] &&
		 a->delta_poc[1] == b->delta_poc[1])) &&
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
	for (i = 0; i < H264_ENTRIES; i++) {
		dpb->pictures[i].long_term_index = -1;
		dpb->pictures[i].mv_slot = -1;
	}
	*out = dpb;
	return 0;
}

/*
 * A P or B recovery point (intra refresh) predicts from pictures before it:
 * give its lists one entry, a gap frame just before it, which
 * back_gap_entry() backs with a buffer.
 */
static int start_reference(struct s7d_h264_dpb *dpb, const struct s7d_h264_slice *slice,
			   const struct s7d_h264_picture *picture)
{
	u32 max_frame_num = 1U << slice->log2_frame_num, i;
	s64 poc = (s64)picture->poc - 2;

	if (slice->type == S7D_H264_SLICE_I)
		return 0;
	for_each_entry(dpb, i)
		if (dpb->pictures[i].occupied && dpb->pictures[i].reference)
			return 0;
	if (poc < INT_MIN)
		return -EOVERFLOW;
	dpb->pictures[H264_GAP_BASE] = (struct s7d_h264_picture) {
		.frame_num = (slice->frame_num + max_frame_num - 1U) % max_frame_num,
		.poc = poc, .top_poc = poc, .bottom_poc = poc,
		.long_term_index = -1, .mv_slot = -1, .occupied = true, .complete = true,
		.output = true, .reference = true, .nonexisting = true,
	};
	return 0;
}

/* D.2.8: recoveryFrameNum lies recovery_frame_cnt frames after the start. */
static void update_recovery(const struct s7d_h264_slice *slice, bool start,
			    struct s7d_h264_picture *picture,
			    struct s7d_h264_recovery *recovery)
{
	u32 max_frame_num = 1U << slice->log2_frame_num;

	if (slice->idr)
		memset(recovery, 0, sizeof(*recovery));
	if (start)
		*recovery = (struct s7d_h264_recovery) {
			.frame_num = slice->frame_num, .frame_cnt = slice->recovery_frame_cnt,
			.poc = picture->poc, .active = true, .leading = true,
		};
	if (recovery->active && (slice->reference || start) &&
	    (slice->frame_num + max_frame_num - recovery->frame_num) % max_frame_num >=
	    recovery->frame_cnt)
		recovery->active = false;
	picture->hidden = recovery->active || (recovery->leading && picture->poc < recovery->poc);
}

bool s7d_h264_dpb_frame_num_gap(const struct s7d_h264_dpb *dpb, bool idr,
				u32 frame_num, u32 log2_frame_num)
{
	return dpb && dpb->active_slot < 0 && log2_frame_num >= 4 && log2_frame_num <= 16 &&
	       frame_num_gap(&dpb->previous, idr, frame_num, log2_frame_num);
}

int s7d_h264_dpb_adopt_sequence(struct s7d_h264_dpb *dpb,
				const struct s7d_h264_sps *sps)
{
	if (!dpb || !sps)
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	if (dpb->active_slot >= 0)
		return -EBUSY;
	if (sps->max_num_ref_frames != dpb->max_references)
		return -EINVAL;
	/* The IDR's own completion still outputs or discards the prior pictures. */
	dpb->reorder_limit = sps->bitstream_restriction ?
			     sps->num_reorder_frames : dpb->layout.dpb_slots;
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
	struct s7d_h264_recovery recovery;
	u32 slot = 0, pass, i;
	bool new_picture, start = false, lenient;
	u64 token;
	int ret;

	if (!dpb || !slice || !input_token || !timestamp_meta || !plan)
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	new_picture = dpb->active_slot < 0;
	recovery = dpb->recovery;
	lenient = dpb->lenient;
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
		start = !dpb->previous.initialized && !slice->idr && slice->recovery;
		ret = fill_frame_num_gap(dpb, slice);
		if (!ret)
			ret = calculate_poc(dpb, slice, &picture, &next);
		if (!ret && start)
			ret = start_reference(dpb, slice, &picture);
		if (!ret)
			ret = back_gap_entry(dpb, slice);
		if (ret)
			return ret;
		update_recovery(slice, start, &picture, &recovery);
		/* Pictures that are never shown may predict from missing references. */
		lenient = start || recovery.active || picture.hidden;
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
	ret = build_lists(dpb, slice, picture.poc, lenient, prepared);
	if (ret)
		return ret;
	if (new_picture) {
retry:
		for (pass = 0; pass < 2; pass++) {
			for (slot = 0; slot < dpb->count; slot++) {
				if (dpb->pictures[slot].occupied)
					continue;
				/* First a buffer no importer still reads, then any. */
				if (!pass && s7d_codec_surface_reading(dpb->bindings[slot].surface))
					continue;
				ret = plan_marking(dpb, slice, slot, lenient, &marking);
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
			if (slot < dpb->count)
				break;
		}
		if (slot == dpb->count) {
			ret = bump_picture(dpb);
			if (ret)
				return ret;
			goto retry;
		}
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
		dpb->next_recovery = recovery;
		dpb->lenient = lenient;
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

	for (i = H264_GAP_BASE; i < H264_ENTRIES; i++) {
		if (dpb->pictures[i].reference)
			continue;
		memset(&dpb->pictures[i], 0, sizeof(dpb->pictures[i]));
		dpb->pictures[i].long_term_index = -1;
		dpb->pictures[i].mv_slot = -1;
	}
	for (i = 0; i < dpb->count; i++) {
		struct s7d_h264_picture *picture = &dpb->pictures[i];

		if (!picture->occupied || !picture->complete ||
		    !picture->output || picture->reference)
			continue;
		/* An unpublished surface goes back to the queue. */
		ret = picture->hidden ?
		      s7d_codec_surface_discard(dpb->bindings[i].surface, picture->write_token) :
		      s7d_codec_surface_ref_put(dpb->bindings[i].surface);
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

static int publish_picture(struct s7d_h264_dpb *dpb, u32 index)
{
	struct s7d_h264_picture *picture = &dpb->pictures[index];
	struct s7d_codec_frame frame = {
		.surface_id = dpb->bindings[index].surface_id,
		.generation = dpb->bindings[index].generation,
		.write_token = picture->write_token,
		.timestamp_ns = picture->timestamp_ns,
		.timestamp_meta = picture->timestamp_meta,
		.sequence = dpb->sequence,
		.flags = picture->flags,
		.bytesused = { dpb->layout.size[0], dpb->layout.size[1] },
	};
	int ret;

	if (picture->hidden) {
		picture->output = true;
		return 0;
	}
	if (dpb->output_started && picture->poc < dpb->last_output_poc)
		return -EBADMSG;
	ret = s7d_codec_session_frame_ready(dpb->session, &frame);
	if (ret)
		return ret;
	picture->output = true;
	dpb->last_output_poc = picture->poc;
	dpb->output_started = true;
	dpb->sequence++;
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
		ret = publish_picture(dpb, selected);
		if (!ret)
			ret = reclaim_pictures(dpb);
		if (ret)
			return ret;
	}
}

/*
 * C.4.5.3: no picture buffer is free, as a backed gap entry needs one more:
 * output the first picture in output order, once per call until a buffer is
 * empty. A hidden picture's buffer is free at once, a published one once the
 * client returns it. 0: look for a free buffer again; -EAGAIN: wait for the
 * client.
 */
static int bump_picture(struct s7d_h264_dpb *dpb)
{
	int selected = -1, ret;
	u32 i;

	for (i = 0; i < dpb->count; i++) {
		const struct s7d_h264_picture *picture = &dpb->pictures[i];

		/* An empty buffer the client still holds. */
		if (!picture->occupied)
			return -EAGAIN;
		if (picture->complete && !picture->output &&
		    (selected < 0 || picture->poc < dpb->pictures[selected].poc))
			selected = i;
	}
	if (selected < 0)
		return -EAGAIN;
	ret = publish_picture(dpb, selected);
	if (!ret)
		ret = reclaim_pictures(dpb);
	if (ret)
		dpb->failed = ret;
	return ret;
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
	for_each_entry(dpb, i) {
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
		/* The order count restarts: no later picture is a leading one. */
		memset(&dpb->next_recovery, 0, sizeof(dpb->next_recovery));
	}
	picture->flags |= flags;
	picture->complete = true;
	dpb->previous = dpb->next;
	dpb->recovery = dpb->next_recovery;
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
	/*
	 * A drain outputs pictures ahead of display order. Decoding resumes
	 * with all state, so later pictures may precede those already out.
	 */
	else
		dpb->output_started = false;
	return ret;
}

int s7d_h264_dpb_abort_picture(struct s7d_h264_dpb *dpb)
{
	struct s7d_h264_picture *picture;
	int ret;

	if (!dpb)
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	if (dpb->active_slot < 0)
		return 0;
	picture = &dpb->pictures[dpb->active_slot];
	/* Not published: the surface goes back to the queue unwritten. */
	ret = s7d_codec_surface_written(dpb->bindings[dpb->active_slot].surface,
					picture->write_token);
	if (!ret)
		ret = s7d_codec_surface_discard(dpb->bindings[dpb->active_slot].surface,
						picture->write_token);
	if (ret) {
		dpb->failed = ret;
		return ret;
	}
	if (picture->mv_slot >= 0)
		dpb->mv_used[picture->mv_slot] = false;
	memset(picture, 0, sizeof(*picture));
	picture->long_term_index = -1;
	picture->mv_slot = -1;
	dpb->active_slot = -1;
	return 0;
}

int s7d_h264_dpb_resync(struct s7d_h264_dpb *dpb)
{
	u32 i;
	int ret;

	if (!dpb)
		return -EINVAL;
	if (dpb->failed)
		return dpb->failed;
	if (dpb->active_slot >= 0)
		return -EBUSY;
	/* What is decoded goes out now, in POC order, not after the next IDR. */
	dpb->output_started = false;
	ret = output_pictures(dpb, true);
	if (ret)
		goto failed;
	for_each_entry(dpb, i) {
		struct s7d_h264_picture *picture = &dpb->pictures[i];

		picture->reference = false;
		picture->long_term_index = -1;
		if (picture->mv_slot >= 0) {
			dpb->mv_used[picture->mv_slot] = false;
			picture->mv_slot = -1;
		}
	}
	ret = reclaim_pictures(dpb);
	if (ret)
		goto failed;
	memset(&dpb->marking, 0, sizeof(dpb->marking));
	memset(&dpb->previous, 0, sizeof(dpb->previous));
	memset(&dpb->next, 0, sizeof(dpb->next));
	memset(&dpb->recovery, 0, sizeof(dpb->recovery));
	memset(&dpb->next_recovery, 0, sizeof(dpb->next_recovery));
	dpb->max_long_term_index = -1;
	dpb->output_started = false;
	return 0;
failed:
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
	for_each_entry(dpb, i) {
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
