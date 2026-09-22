// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/errno.h>

#include <drm/drm_framebuffer.h>

#include "s7d-scanout.h"

static void s7d_scanout_retire(struct work_struct *work)
{
	struct s7d_scanout *s = container_of(work, struct s7d_scanout, retire_work);
	struct drm_framebuffer *fb;
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	fb = s->retired_fb;
	s->retired_fb = NULL;
	spin_unlock_irqrestore(&s->lock, flags);
	if (fb)
		drm_framebuffer_put(fb);
}

void s7d_scanout_init(struct s7d_scanout *s, struct s7d_rdma *rdma)
{
	*s = (struct s7d_scanout) { .rdma = rdma, .phase = S7D_SCANOUT_STOPPED };
	spin_lock_init(&s->lock);
	INIT_WORK(&s->retire_work, s7d_scanout_retire);
}

int s7d_scanout_begin_initial(struct s7d_scanout *s, struct drm_framebuffer *fb)
{
	unsigned long flags;
	int ret = 0;

	if (!fb)
		return -EINVAL;
	flush_work(&s->retire_work);
	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
	} else if (s->phase != S7D_SCANOUT_STOPPED ||
		   s->active_fb || s->pending_fb || s->retired_fb) {
		ret = -EBUSY;
	} else {
		drm_framebuffer_get(fb);
		s->pending_fb = fb;
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
	} else if (s->phase != S7D_SCANOUT_INITIAL || !s->pending_fb) {
		ret = -EINVAL;
	} else {
		/* Caller has not enabled VENC yet; its first vblank is sufficient. */
		s->wait_field = false;
		s->phase = S7D_SCANOUT_VBLANK;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}

int s7d_scanout_submit(struct s7d_scanout *s, struct drm_framebuffer *fb,
			const struct s7d_rdma_entry *entries, unsigned int count)
{
	unsigned long flags;
	bool put = false;
	int ret;

	if (!fb)
		return -EINVAL;
	/* The previous IRQ may already have scheduled a sleeping GEM release. */
	flush_work(&s->retire_work);
	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
		goto out;
	}
	if (s->phase != S7D_SCANOUT_IDLE || !s->active_fb || s->pending_fb || s->retired_fb) {
		ret = -EBUSY;
		goto out;
	}
	drm_framebuffer_get(fb);
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
	s->pending_fb = fb;
	s->phase = S7D_SCANOUT_RDMA;
out:
	spin_unlock_irqrestore(&s->lock, flags);
	if (put)
		drm_framebuffer_put(fb);
	return ret;
}

enum s7d_scanout_result
s7d_scanout_irq(struct s7d_scanout *s, bool vblank, enum s7d_rdma_result rdma_result,
		const struct s7d_frame_state *frame)
{
	enum s7d_scanout_result result = S7D_SCANOUT_NO_CHANGE;
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	if (vblank)
		s->vblank_seq++;
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
		/* A vblank supplied in this call belongs to the completion frame. */
		goto out;
	}
	if (!vblank || s->phase != S7D_SCANOUT_VBLANK)
		goto out;
	if (s->wait_field && (!frame->idle || frame->field == s->applied_field))
		goto out;
	if (!s->pending_fb || s->retired_fb) {
		s->fault = true;
		result = S7D_SCANOUT_FAULT;
		goto out;
	}
	s->retired_fb = s->active_fb;
	s->active_fb = s->pending_fb;
	s->pending_fb = NULL;
	s->phase = S7D_SCANOUT_IDLE;
	if (s->retired_fb)
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
	struct drm_framebuffer *active_fb, *pending;
	unsigned long flags;
	int ret;

	/* Prevent a late IRQ from completing a transition while reset sleeps. */
	s7d_scanout_fail(s);
	ret = s7d_rdma_quiesce(s->rdma);
	if (ret)
		return ret;
	flush_work(&s->retire_work);
	spin_lock_irqsave(&s->lock, flags);
	active_fb = s->active_fb;
	pending = s->pending_fb;
	s->active_fb = NULL;
	s->pending_fb = NULL;
	s->phase = S7D_SCANOUT_STOPPED;
	s->wait_field = false;
	s->fault = false;
	spin_unlock_irqrestore(&s->lock, flags);
	if (active_fb)
		drm_framebuffer_put(active_fb);
	if (pending)
		drm_framebuffer_put(pending);
	return 0;
}

int s7d_scanout_fini(struct s7d_scanout *s)
{
	/* Caller has synchronized IRQs and serialized against new commits. */
	flush_work(&s->retire_work);
	if (s->phase != S7D_SCANOUT_STOPPED || s->fault ||
	    s->active_fb || s->pending_fb || s->retired_fb)
		return -EBUSY;
	return 0;
}
