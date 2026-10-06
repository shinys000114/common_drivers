// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/errno.h>
#include <linux/string.h>

#include <drm/drm_framebuffer.h>

#include "s7d-scanout.h"

static bool buffers_present(const struct s7d_scanout_buffers *buffers)
{
	unsigned int i;

	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers->fb[i])
			return true;
	return false;
}

static bool video_owned(const struct s7d_scanout_buffers *buffers)
{
	unsigned int i;

	if (!buffers->video.fb)
		return false;
	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers->fb[i] == buffers->video.fb)
			return true;
	return false;
}

static bool afbc_owned(const struct s7d_scanout_buffers *buffers)
{
	unsigned int i;

	if (!buffers->afbc.fb || s7d_afbc_check_state(&buffers->afbc.plan))
		return false;
	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers->fb[i] == buffers->afbc.fb)
			return true;
	return false;
}

bool s7d_scanout_video_equal(const struct s7d_scanout_buffers *a,
			     const struct s7d_scanout_buffers *b)
{
	unsigned int i;

	if (!video_owned(a) || !video_owned(b) || a->video.fb != b->video.fb ||
	    a->video.pipeline.update_count != S7D_VIDEO_UPDATE_REG_COUNT ||
	    b->video.pipeline.update_count != S7D_VIDEO_UPDATE_REG_COUNT ||
	    !(a->video.pipeline.control.value & BIT(0)) ||
	    memcmp(&a->video.pipeline, &b->video.pipeline, sizeof(a->video.pipeline)) ||
	    memcmp(&a->video.csc, &b->video.csc, sizeof(a->video.csc)))
		return false;
	for (i = 0; i < S7D_POSTBLEND_REG_COUNT; i++) {
		const struct s7d_rdma_entry *x = &a->video.postblend.regs[i];
		const struct s7d_rdma_entry *y = &b->video.postblend.regs[i];
		u32 mask = le32_to_cpu(x->reg) == 0x1dfe ? BIT(10) : 0;

		if (x->reg != y->reg)
			return false;
		/* Only the OSD2 source 4 enable may differ. */
		if (le32_to_cpu(x->value ^ y->value) & ~mask)
			return false;
	}
	return true;
}

static void buffers_get(const struct s7d_scanout_buffers *buffers)
{
	unsigned int i;

	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers->fb[i])
			drm_framebuffer_get(buffers->fb[i]);
}

static void buffers_put(const struct s7d_scanout_buffers *buffers)
{
	unsigned int i;

	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers->fb[i])
			drm_framebuffer_put(buffers->fb[i]);
}

static void s7d_scanout_retire(struct work_struct *work)
{
	struct s7d_scanout *s = container_of(work, struct s7d_scanout, retire_work);
	struct s7d_scanout_buffers retired;
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	retired = s->retired;
	s->retired = (struct s7d_scanout_buffers) {0};
	spin_unlock_irqrestore(&s->lock, flags);
	buffers_put(&retired);
}

void s7d_scanout_init(struct s7d_scanout *s, struct s7d_rdma *rdma)
{
	*s = (struct s7d_scanout) { .rdma = rdma, .phase = S7D_SCANOUT_STOPPED };
	spin_lock_init(&s->lock);
	INIT_WORK(&s->retire_work, s7d_scanout_retire);
}

int s7d_scanout_begin_initial(struct s7d_scanout *s,
			      const struct s7d_scanout_buffers *buffers)
{
	unsigned long flags;
	int ret = 0;

	if (!buffers || !buffers_present(buffers))
		return -EINVAL;
	if ((buffers->video.fb && !video_owned(buffers)) ||
	    (buffers->afbc.fb && !afbc_owned(buffers)))
		return -EINVAL;
	flush_work(&s->retire_work);
	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
	} else if (s->phase != S7D_SCANOUT_STOPPED ||
		   buffers_present(&s->active) || buffers_present(&s->pending) ||
		   buffers_present(&s->retired)) {
		ret = -EBUSY;
	} else if (s->generation == ~0ULL) {
		ret = -EOVERFLOW;
	} else {
		buffers_get(buffers);
		s->pending = *buffers;
		s->pending.generation = ++s->generation;
		s->pending_afbc_epoch = 0;
		s->phase = S7D_SCANOUT_INITIAL;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}

int s7d_scanout_initial_ready(struct s7d_scanout *s)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
	} else if (s->phase != S7D_SCANOUT_INITIAL || !buffers_present(&s->pending)) {
		ret = -EINVAL;
	} else {
		/* Initial programming and VENC start have succeeded. */
		s->wait_field = false;
		s->phase = S7D_SCANOUT_VBLANK;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}

void s7d_scanout_flush_retired(struct s7d_scanout *s)
{
	flush_work(&s->retire_work);
}

u64 s7d_scanout_pending_generation(struct s7d_scanout *s)
{
	unsigned long flags;
	u64 generation;

	spin_lock_irqsave(&s->lock, flags);
	generation = buffers_present(&s->pending) ? s->pending.generation : 0;
	spin_unlock_irqrestore(&s->lock, flags);
	return generation;
}

int s7d_scanout_afbc_started(struct s7d_scanout *s, u64 generation, u64 epoch)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
	} else if (!epoch || !generation || generation != s->pending.generation ||
		   !afbc_owned(&s->pending) ||
		   (s->phase != S7D_SCANOUT_INITIAL && s->phase != S7D_SCANOUT_VBLANK)) {
		ret = -EINVAL;
	} else if (s->pending_afbc_epoch && s->pending_afbc_epoch != epoch) {
		ret = -EBUSY;
	} else {
		s->pending_afbc_epoch = epoch;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}

int s7d_scanout_submit_staged(struct s7d_scanout *s,
			     const struct s7d_scanout_buffers *buffers,
			     const struct s7d_rdma_entry *entries, unsigned int count,
			     bool video_unchanged,
			     int (*stage)(void *data,
				const struct s7d_scanout_buffers *active,
				const struct s7d_scanout_buffers *candidate,
				u64 generation),
			     int (*cancel)(void *data, u64 generation), void *data)
{
	struct s7d_scanout_buffers candidate;
	unsigned long flags;
	int ret;

	if (!buffers || !buffers_present(buffers) || (!!stage != !!cancel) ||
	    (buffers->video.fb && !video_owned(buffers)) ||
	    (buffers->afbc.fb && (!afbc_owned(buffers) || !stage)))
		return -EINVAL;
	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
		goto out;
	}
	if (s->phase != S7D_SCANOUT_IDLE || !buffers_present(&s->active) ||
	    buffers_present(&s->pending) || buffers_present(&s->retired)) {
		ret = -EBUSY;
		goto out;
	}
	/* AFBC entry and exit require a stopped, fully prepared pipeline. */
	if (!!s->active.afbc.fb != !!buffers->afbc.fb ||
	    (video_unchanged && !s7d_scanout_video_equal(&s->active, buffers))) {
		ret = -EINVAL;
		goto out;
	}
	if (s->generation == ~0ULL) {
		ret = -EOVERFLOW;
		goto out;
	}
	candidate = *buffers;
	candidate.generation = ++s->generation;
	buffers_get(&candidate);
	if (stage) {
		ret = stage(data, &s->active, &candidate, candidate.generation);
		if (ret)
			goto rollback;
	}
	/* Backend frame lock -> owner lock -> RDMA lock. */
	ret = s7d_rdma_submit(s->rdma, entries, count);
	if (ret)
		goto rollback;
	s->pending = candidate;
	s->pending_afbc_epoch = 0;
	s->phase = S7D_SCANOUT_RDMA;
	s->vblank_valid = false;
	s->early_complete = false;
	goto out;
rollback:
	if (cancel && cancel(data, candidate.generation)) {
		/* An uncertain stage still owns its header and framebuffer. */
		s->pending = candidate;
		s->pending_afbc_epoch = 0;
		s->fault = true;
	} else {
		/* GEM release may sleep; the caller still holds the frame lock. */
		s->retired = candidate;
		schedule_work(&s->retire_work);
	}
	if (ret == -EIO)
		s->fault = true;
out:
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}

int s7d_scanout_submit(struct s7d_scanout *s,
			const struct s7d_scanout_buffers *buffers,
			const struct s7d_rdma_entry *entries, unsigned int count,
			bool video_unchanged)
{
	int ret;

	s7d_scanout_flush_retired(s);
	ret = s7d_scanout_submit_staged(s, buffers, entries, count, video_unchanged,
				       NULL, NULL, NULL);
	if (ret)
		s7d_scanout_flush_retired(s);
	return ret;
}

enum s7d_scanout_result
s7d_scanout_irq(struct s7d_scanout *s, bool vblank, enum s7d_rdma_result rdma_result,
		const struct s7d_frame_state *frame)
{
	enum s7d_scanout_result result = S7D_SCANOUT_NO_CHANGE;
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	if (vblank) {
		s->vblank_seq++;
		s->vblank_field = frame->field;
		s->vblank_valid = true;
	}
	if (rdma_result == S7D_RDMA_FAULT ||
	    (rdma_result == S7D_RDMA_COMPLETE && s->phase != S7D_SCANOUT_RDMA))
		s->fault = true;
	if (s->fault) {
		result = S7D_SCANOUT_FAULT;
		goto out;
	}
	if (rdma_result == S7D_RDMA_COMPLETE) {
		s->phase = S7D_SCANOUT_VBLANK;
		s->applied_field = frame->field;
		s->wait_field = true;
		s->early_complete = frame->early && !s->pending.afbc.fb;
	}
	if (s->phase != S7D_SCANOUT_VBLANK)
		goto out;
	if (s->wait_field) {
		bool early = s->early_complete && frame->early && frame->idle && s->vblank_valid &&
			     frame->field == s->applied_field &&
			     frame->field == s->vblank_field;

		if (!early && (!vblank || !frame->idle || frame->field == s->applied_field))
			goto out;
	} else if (!vblank) {
		goto out;
	}
	if (s->pending.afbc.fb &&
	    (!vblank || !frame->idle || !frame->afbc_gate ||
	     !s->pending_afbc_epoch ||
	     frame->afbc_completed_generation != s->pending.generation ||
	     frame->afbc_completed_epoch != s->pending_afbc_epoch))
		goto out;
	if (!buffers_present(&s->pending) || buffers_present(&s->retired)) {
		s->fault = true;
		result = S7D_SCANOUT_FAULT;
		goto out;
	}
	s->retired = s->active;
	s->active = s->pending;
	s->pending = (struct s7d_scanout_buffers) {0};
	s->pending_afbc_epoch = 0;
	s->phase = S7D_SCANOUT_IDLE;
	if (buffers_present(&s->retired))
		schedule_work(&s->retire_work);
	result = S7D_SCANOUT_FRAME_COMPLETE;
out:
	spin_unlock_irqrestore(&s->lock, flags);
	return result;
}

void s7d_scanout_fail(struct s7d_scanout *s)
{
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	s->fault = true;
	spin_unlock_irqrestore(&s->lock, flags);
}

int s7d_scanout_quiesce(struct s7d_scanout *s)
{
	struct s7d_scanout_buffers active, pending;
	unsigned long flags;
	int ret;

	/* Prevent a late IRQ from completing a transition while reset sleeps. */
	s7d_scanout_fail(s);
	ret = s7d_rdma_quiesce(s->rdma);
	if (ret)
		return ret;
	flush_work(&s->retire_work);
	spin_lock_irqsave(&s->lock, flags);
	active = s->active;
	pending = s->pending;
	s->active = (struct s7d_scanout_buffers) {0};
	s->pending = (struct s7d_scanout_buffers) {0};
	s->phase = S7D_SCANOUT_STOPPED;
	s->pending_afbc_epoch = 0;
	s->wait_field = false;
	s->fault = false;
	spin_unlock_irqrestore(&s->lock, flags);
	buffers_put(&active);
	buffers_put(&pending);
	return 0;
}

int s7d_scanout_fini(struct s7d_scanout *s)
{
	/* Caller has synchronized IRQs and serialized against new commits. */
	flush_work(&s->retire_work);
	if (s->phase != S7D_SCANOUT_STOPPED || s->fault ||
	    buffers_present(&s->active) || buffers_present(&s->pending) ||
	    buffers_present(&s->retired))
		return -EBUSY;
	return 0;
}
