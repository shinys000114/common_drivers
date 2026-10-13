/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_FLIP_TIMESTAMP_H
#define __S7D_FLIP_TIMESTAMP_H

#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/types.h>

#define S7D_FLIP_TIMESTAMP_HISTORY 4

struct s7d_flip_latch {
	u64 cookie;
	u64 vblank_sequence;
	u8 field;
	bool ambiguous;
};

struct s7d_flip_stamp {
	u64 vblank_sequence;
	u64 drm_sequence;
	ktime_t time;
	u8 field;
	bool valid;
};

struct s7d_flip_timestamp {
	u64 cookie;
	u64 after_sequence;
	u64 latest_sequence;
	unsigned int next;
	bool have_latch;
	struct s7d_flip_latch latch;
	struct s7d_flip_stamp history[S7D_FLIP_TIMESTAMP_HISTORY];
	struct s7d_flip_stamp stamp;
};

static inline int s7d_flip_timestamp_begin(struct s7d_flip_timestamp *t,
					 u64 cookie, u64 after_sequence)
{
	if (!cookie || after_sequence == ~0ULL)
		return -EINVAL;
	if (t->cookie)
		return -EBUSY;
	*t = (struct s7d_flip_timestamp) {
		.cookie = cookie,
		.after_sequence = after_sequence,
		.latest_sequence = after_sequence,
	};
	return 0;
}

static inline int s7d_flip_timestamp_vblank(struct s7d_flip_timestamp *t,
					  const struct s7d_flip_stamp *stamp)
{
	const struct s7d_flip_stamp *last;

	if (!t->cookie)
		return -ENOENT;
	if (!stamp->valid || stamp->field > 7 || !stamp->drm_sequence ||
	    ktime_to_ns(stamp->time) <= 0)
		return -ENODATA;
	if (t->latest_sequence == ~0ULL ||
	    stamp->vblank_sequence != t->latest_sequence + 1)
		return -ESTALE;
	last = &t->history[(t->next + S7D_FLIP_TIMESTAMP_HISTORY - 1) %
			   S7D_FLIP_TIMESTAMP_HISTORY];
	if (last->valid && (stamp->drm_sequence <= last->drm_sequence ||
			    ktime_compare(stamp->time, last->time) <= 0))
		return -ESTALE;
	t->history[t->next] = *stamp;
	t->next = (t->next + 1) % S7D_FLIP_TIMESTAMP_HISTORY;
	t->latest_sequence = stamp->vblank_sequence;
	if (t->have_latch && !t->stamp.valid) {
		if (t->latch.vblank_sequence < stamp->vblank_sequence)
			return -ESTALE;
		if (t->latch.vblank_sequence == stamp->vblank_sequence) {
			if (t->latch.field != stamp->field)
				return -EPROTO;
			t->stamp = *stamp;
		}
	}
	return 0;
}

static inline int s7d_flip_timestamp_latch(struct s7d_flip_timestamp *t,
					 const struct s7d_flip_latch *latch)
{
	unsigned int i;

	if (!t->cookie || latch->cookie != t->cookie)
		return -ESTALE;
	if (latch->ambiguous)
		return -ENODATA;
	if (latch->field > 7 || latch->vblank_sequence <= t->after_sequence ||
	    (latch->vblank_sequence > t->latest_sequence &&
	     (t->latest_sequence == ~0ULL ||
	      latch->vblank_sequence != t->latest_sequence + 1)))
		return -ESTALE;
	if (t->have_latch)
		return t->latch.vblank_sequence == latch->vblank_sequence &&
		       t->latch.field == latch->field ? 0 : -EPROTO;
	for (i = 0; i < S7D_FLIP_TIMESTAMP_HISTORY; i++) {
		const struct s7d_flip_stamp *stamp = &t->history[i];

		if (!stamp->valid || stamp->vblank_sequence != latch->vblank_sequence)
			continue;
		if (stamp->field != latch->field)
			return -EPROTO;
		t->stamp = *stamp;
		break;
	}
	if (!t->stamp.valid && latch->vblank_sequence <= t->latest_sequence)
		return -ESTALE;
	t->latch = *latch;
	t->have_latch = true;
	return 0;
}

#endif
