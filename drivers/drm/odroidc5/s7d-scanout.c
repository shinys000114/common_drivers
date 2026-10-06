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
	if (buffers->video.fb && !video_owned(buffers))
		return -EINVAL;
	flush_work(&s->retire_work);
	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
	} else if (s->phase != S7D_SCANOUT_STOPPED ||
		   buffers_present(&s->active) || buffers_present(&s->pending) ||
		   buffers_present(&s->retired)) {
		ret = -EBUSY;
	} else {
		buffers_get(buffers);
		s->pending = *buffers;
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

int s7d_scanout_submit(struct s7d_scanout *s,
			const struct s7d_scanout_buffers *buffers,
			const struct s7d_rdma_entry *entries, unsigned int count,
			bool video_unchanged)
{
	unsigned long flags;
	bool put = false;
	int ret;

	if (!buffers || !buffers_present(buffers))
		return -EINVAL;
	if (buffers->video.fb && !video_owned(buffers))
		return -EINVAL;
	/* The previous IRQ may already have scheduled a sleeping GEM release. */
	flush_work(&s->retire_work);
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
	/* An omitted video payload must match the owner, not just old DRM state. */
	if (video_unchanged && !s7d_scanout_video_equal(&s->active, buffers)) {
		ret = -EINVAL;
		goto out;
	}
	buffers_get(buffers);
	/*
	 * Nest owner -> RDMA lock. IRQ must release the RDMA lock before calling
	 * s7d_scanout_irq. Publishing pending and arming are serialized vs IRQ.
	 */
	ret = s7d_rdma_submit(s->rdma, entries, count);
	if (ret) {
		/* Backend errors precede table replacement and trigger arming. */
		put = true;
		if (ret == -EIO)
			s->fault = true;
		goto out;
	}
	s->pending = *buffers;
	s->phase = S7D_SCANOUT_RDMA;
	s->vblank_valid = false;
	s->early_complete = false;
out:
	spin_unlock_irqrestore(&s->lock, flags);
	if (put)
		buffers_put(buffers);
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
		s->early_complete = frame->early;
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
	if (!buffers_present(&s->pending) || buffers_present(&s->retired)) {
		s->fault = true;
		result = S7D_SCANOUT_FAULT;
		goto out;
	}
	s->retired = s->active;
	s->active = s->pending;
	s->pending = (struct s7d_scanout_buffers) {0};
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
