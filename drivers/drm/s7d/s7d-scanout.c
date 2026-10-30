// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/errno.h>
#include <linux/string.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>

#include "s7d-scanout.h"
#include "s7d-plane.h"
#include "s7d-afbc-engine.h"

static bool buffers_present(const struct s7d_scanout_buffers *buffers)
{
	unsigned int i;

	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers->fb[i])
			return true;
	return false;
}

static bool video_owned(const struct s7d_scanout_buffers *buffers,
			const struct s7d_scanout_video *video, unsigned int slot)
{
	return video->fb && buffers->fb[slot] == video->fb;
}

static bool afbc_owned(const struct s7d_scanout_buffers *buffers)
{
	const struct s7d_scanout_afbc *afbc = &buffers->afbc;
	unsigned int i;

	if (s7d_afbc_plan_check(&afbc->plan))
		return false;
	for (i = 0; i < S7D_AFBC_MAX_SURFACES; i++)
		if (afbc->plan.surface_mask & BIT(i) ?
		    !afbc->fb[i] || buffers->fb[s7d_plane_afbc_slot(i)] != afbc->fb[i] :
		    !!afbc->fb[i])
			return false;
	return true;
}

static bool gfcd_owned(const struct s7d_scanout_buffers *buffers)
{
	return buffers->gfcd.fb &&
		(!buffers->afbc.plan.surface_mask ||
		 (afbc_owned(buffers) && buffers->afbc.plan.surface_mask == BIT(1))) &&
		buffers->fb[S7D_PLANE_PRIMARY] == buffers->gfcd.fb &&
		!s7d_gfcd_check_state(&buffers->gfcd.plan);
}

bool s7d_scanout_afbc_equal(const struct s7d_scanout_buffers *a,
			    const struct s7d_scanout_buffers *b)
{
	return afbc_owned(a) && afbc_owned(b) &&
		(a->afbc.plan.surface_mask != BIT(1) ||
		 (gfcd_owned(a) && gfcd_owned(b))) &&
		!memcmp(a->afbc.fb, b->afbc.fb, sizeof(a->afbc.fb)) &&
		s7d_afbc_plan_same_layout(&a->afbc.plan, &b->afbc.plan) &&
		s7d_afbc_plan_same_headers(&a->afbc.plan, &b->afbc.plan);
}

static bool replay_buffers(const struct s7d_scanout_buffers *b)
{
	struct drm_framebuffer *secondary = b->fb[S7D_PLANE_RGB] ?
		b->fb[S7D_PLANE_RGB] : b->fb[S7D_PLANE_CURSOR];

	return afbc_owned(b) && !b->video.fb && !b->video2.fb &&
		!b->fb[S7D_PLANE_VIDEO] && !b->fb[S7D_PLANE_VIDEO2] &&
		!(b->fb[S7D_PLANE_RGB] && b->fb[S7D_PLANE_CURSOR]) &&
		((b->afbc.plan.surface_mask == BIT(0) && !b->gfcd.fb &&
		  (!secondary || secondary->modifier == DRM_FORMAT_MOD_LINEAR)) ||
		 (gfcd_owned(b) && !b->fb[S7D_PLANE_CURSOR]));
}

bool s7d_scanout_replay_compatible(const struct s7d_scanout_buffers *a,
				 const struct s7d_scanout_buffers *b)
{
	return replay_buffers(a) && replay_buffers(b) &&
		!!a->gfcd.fb == !!b->gfcd.fb &&
		(!a->gfcd.fb || s7d_gfcd_same_config(&a->gfcd.plan, &b->gfcd.plan)) &&
		s7d_afbc_plan_same_layout(&a->afbc.plan, &b->afbc.plan);
}

static bool video_same_config(const struct s7d_scanout_buffers *a,
			      const struct s7d_scanout_buffers *b,
			      const struct s7d_scanout_video *x,
			      const struct s7d_scanout_video *y, unsigned int slot)
{
	unsigned int i, count = slot == S7D_PLANE_VIDEO ?
		S7D_VIDEO_UPDATE_REG_COUNT : S7D_VIDEO2_UPDATE_REG_COUNT;
	u32 base = slot == S7D_PLANE_VIDEO ? 0x4820 : 0x48a0;

	if (!x->fb && !y->fb)
		return true;
	if (!video_owned(a, x, slot) || !video_owned(b, y, slot) ||
	    x->pipeline.update_count != count || y->pipeline.update_count != count ||
	    !(x->pipeline.control.value & BIT(0)) ||
	    memcmp(x->pipeline.setup, y->pipeline.setup, sizeof(x->pipeline.setup)) ||
	    memcmp(&x->pipeline.control, &y->pipeline.control, sizeof(x->pipeline.control)) ||
	    memcmp(&x->pipeline.scaler, &y->pipeline.scaler, sizeof(x->pipeline.scaler)) ||
	    memcmp(&x->csc, &y->csc, sizeof(x->csc)))
		return false;
	for (i = 0; i < count; i++) {
		const struct s7d_rdma_entry *r = &x->pipeline.update[i];
		const struct s7d_rdma_entry *t = &y->pipeline.update[i];

		if (r->reg != t->reg ||
		    (i < 2 ? le32_to_cpu(r->reg) != base + i : r->value != t->value))
			return false;
	}
	for (i = 0; i < S7D_POSTBLEND_REG_COUNT; i++) {
		const struct s7d_rdma_entry *r = &x->postblend.regs[i];
		const struct s7d_rdma_entry *t = &y->postblend.regs[i];
		u32 mask = le32_to_cpu(r->reg) == 0x1dfe ? BIT(10) : 0;

		if (r->reg != t->reg || (le32_to_cpu(r->value ^ t->value) & ~mask))
			return false;
	}
	return true;
}

bool s7d_scanout_video_same_config(const struct s7d_scanout_buffers *a,
				   const struct s7d_scanout_buffers *b)
{
	return (a->video.fb || a->video2.fb) &&
		video_same_config(a, b, &a->video, &b->video, S7D_PLANE_VIDEO) &&
		video_same_config(a, b, &a->video2, &b->video2, S7D_PLANE_VIDEO2);
}

static bool video_equal(const struct s7d_scanout_video *x,
			const struct s7d_scanout_video *y)
{
	return x->fb == y->fb && (!x->fb ||
		(x->pipeline.update[0].value == y->pipeline.update[0].value &&
		 x->pipeline.update[1].value == y->pipeline.update[1].value));
}

bool s7d_scanout_video_equal(const struct s7d_scanout_buffers *a,
			     const struct s7d_scanout_buffers *b)
{
	return s7d_scanout_video_same_config(a, b) &&
		video_equal(&a->video, &b->video) && video_equal(&a->video2, &b->video2);
}

static void buffers_get(const struct s7d_scanout_buffers *buffers)
{
	unsigned int i;

	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers->fb[i])
			drm_framebuffer_get(buffers->fb[i]);
}

static void buffers_put(struct drm_framebuffer *const buffers[S7D_SCANOUT_MAX_PLANES])
{
	unsigned int i;

	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (buffers[i])
			drm_framebuffer_put(buffers[i]);
}

static void s7d_scanout_retire(struct work_struct *work)
{
	struct s7d_scanout *s = container_of(work, struct s7d_scanout, retire_work);
	struct drm_framebuffer *retired[S7D_SCANOUT_MAX_PLANES];
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	memcpy(retired, s->retired.fb, sizeof(retired));
	s->retired = (struct s7d_scanout_buffers) {0};
	spin_unlock_irqrestore(&s->lock, flags);
	buffers_put(retired);
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
	if ((buffers->video.fb && !video_owned(buffers, &buffers->video, S7D_PLANE_VIDEO)) ||
	    (buffers->video2.fb && !video_owned(buffers, &buffers->video2, S7D_PLANE_VIDEO2)) ||
	    (buffers->afbc.plan.surface_mask && !afbc_owned(buffers)) ||
	    (buffers->gfcd.fb && !gfcd_owned(buffers)))
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
		s->pending.afbc.generation = buffers->afbc.plan.surface_mask ?
			s->pending.generation : 0;
		s->pending_afbc_epoch = 0;
		s->pending_afbc_unchanged = false;
		s->pending_transaction = S7D_SCANOUT_TRANSACTION_RDMA;
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

u64 s7d_scanout_pending_afbc_generation(struct s7d_scanout *s)
{
	unsigned long flags;
	u64 generation;

	spin_lock_irqsave(&s->lock, flags);
	generation = s->pending.afbc.plan.surface_mask && !s->pending_afbc_unchanged ?
		s->pending.afbc.generation : 0;
	spin_unlock_irqrestore(&s->lock, flags);
	return generation;
}

bool s7d_scanout_afbc_unchanged(struct s7d_scanout *s)
{
	unsigned long flags;
	bool unchanged;

	spin_lock_irqsave(&s->lock, flags);
	unchanged = s->pending_afbc_unchanged;
	spin_unlock_irqrestore(&s->lock, flags);
	return unchanged;
}

int s7d_scanout_afbc_started(struct s7d_scanout *s, u64 generation, u64 epoch,
			     u8 field)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
	} else if (!epoch || !generation || generation != s->pending.afbc.generation ||
		   s->pending_afbc_unchanged || !afbc_owned(&s->pending) ||
		   (s->phase == S7D_SCANOUT_DECODE &&
	     s->pending_transaction != S7D_SCANOUT_TRANSACTION_DECODE_ONLY &&
	     s->pending_transaction != S7D_SCANOUT_TRANSACTION_REPLAY) ||
		   (s->phase != S7D_SCANOUT_INITIAL && s->phase != S7D_SCANOUT_VBLANK &&
		    s->phase != S7D_SCANOUT_DECODE)) {
		ret = -EINVAL;
	} else if (s->pending_afbc_epoch && s->pending_afbc_epoch != epoch) {
		ret = -EBUSY;
	} else {
		s->pending_afbc_epoch = epoch;
		if (s->phase == S7D_SCANOUT_DECODE) {
			s->phase = S7D_SCANOUT_VBLANK;
			s->applied_field = field;
			s->wait_field = true;
		}
	}
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}

int s7d_scanout_replay_applied(struct s7d_scanout *s, u64 cookie,
			       u64 afbc_generation, u64 epoch, u8 field)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&s->lock, flags);
	if (s->fault) {
		ret = -EIO;
	} else if (!cookie || !epoch || !afbc_generation ||
		   cookie != s->pending.generation ||
		   afbc_generation != s->pending.afbc.generation ||
		   s->phase != S7D_SCANOUT_DECODE ||
		   s->pending_transaction != S7D_SCANOUT_TRANSACTION_REPLAY ||
		   !s7d_scanout_replay_compatible(&s->active, &s->pending) ||
		   (s->pending_afbc_unchanged &&
		    afbc_generation != s->active.afbc.generation)) {
		ret = -EINVAL;
	} else {
		s->pending_afbc_epoch = epoch;
		s->phase = S7D_SCANOUT_VBLANK;
		s->applied_field = field;
		s->wait_field = true;
		s->early_complete = true;
	}
	spin_unlock_irqrestore(&s->lock, flags);
	return ret;
}

int s7d_scanout_submit_staged(struct s7d_scanout *s,
			     const struct s7d_scanout_buffers *buffers,
			     const struct s7d_rdma_entry *entries, unsigned int count,
			     bool video_unchanged, bool video_address_only, bool afbc_unchanged,
			     enum s7d_scanout_transaction transaction,
			     const struct s7d_scanout_afbc *previous,
			     int (*stage)(void *data,
				const struct s7d_scanout_buffers *active,
				const struct s7d_scanout_buffers *candidate,
				u64 generation,
				enum s7d_scanout_transaction transaction),
			     int (*cancel)(void *data, u64 generation), void *data)
{
	struct s7d_scanout_buffers candidate;
	unsigned long flags;
	int ret;

	if (!buffers || !buffers_present(buffers) || (!!stage != !!cancel) ||
	    (video_unchanged && video_address_only) ||
	    (transaction != S7D_SCANOUT_TRANSACTION_RDMA &&
	     transaction != S7D_SCANOUT_TRANSACTION_DECODE_ONLY &&
	     transaction != S7D_SCANOUT_TRANSACTION_REPLAY) ||
	    (buffers->video.fb && !video_owned(buffers, &buffers->video, S7D_PLANE_VIDEO)) ||
	    (buffers->video2.fb && !video_owned(buffers, &buffers->video2, S7D_PLANE_VIDEO2)) ||
	    (buffers->afbc.plan.surface_mask && (!afbc_owned(buffers) || !stage)) ||
	    (buffers->gfcd.fb && !gfcd_owned(buffers)))
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
	/* Mali surface entry and exit require a stopped, fully prepared pipeline. */
	if (s->active.afbc.plan.surface_mask != buffers->afbc.plan.surface_mask ||
	    !!s->active.gfcd.fb != !!buffers->gfcd.fb ||
	    (buffers->gfcd.fb &&
	     !s7d_gfcd_same_config(&s->active.gfcd.plan, &buffers->gfcd.plan)) ||
	    (video_unchanged && !s7d_scanout_video_equal(&s->active, buffers)) ||
	    (video_address_only && !s7d_scanout_video_same_config(&s->active, buffers)) ||
	    (afbc_unchanged && (!s->active.afbc.generation ||
			       !s7d_scanout_afbc_equal(&s->active, buffers)))) {
		ret = -EINVAL;
		goto out;
	}
	if (transaction != S7D_SCANOUT_TRANSACTION_RDMA) {
		unsigned int i;

		if (video_unchanged || video_address_only ||
		    (afbc_unchanged && transaction != S7D_SCANOUT_TRANSACTION_REPLAY) ||
		    !previous || !stage ||
		    !s7d_scanout_replay_compatible(&s->active, buffers)) {
			ret = -EINVAL;
			goto out;
		}
		if (s7d_afbc_plan_check(&previous->plan) ||
		    memcmp(previous->fb, s->active.afbc.fb, sizeof(previous->fb)) ||
		    !s7d_afbc_plan_same_layout(&previous->plan, &s->active.afbc.plan) ||
		    !s7d_afbc_plan_same_headers(&previous->plan, &s->active.afbc.plan)) {
			ret = -EIO;
			s->fault = true;
			goto out;
		}
		if (transaction == S7D_SCANOUT_TRANSACTION_DECODE_ONLY)
			for (i = 1; i < S7D_SCANOUT_MAX_PLANES; i++)
				if (buffers->fb[i] != s->active.fb[i]) {
					ret = -EINVAL;
					goto out;
				}
		ret = transaction == S7D_SCANOUT_TRANSACTION_REPLAY ?
			s7d_rdma_loop_check_stage(s->rdma) :
			s7d_rdma_check_idle(s->rdma);
		if (ret)
			goto out;
	}
	if (s->generation == ~0ULL) {
		ret = -EOVERFLOW;
		goto out;
	}
	candidate = *buffers;
	candidate.generation = ++s->generation;
	candidate.afbc.generation = !candidate.afbc.plan.surface_mask ? 0 :
		(afbc_unchanged ? s->active.afbc.generation : candidate.generation);
	buffers_get(&candidate);
	if (stage) {
		ret = stage(data, &s->active, &candidate, candidate.generation, transaction);
		if (ret)
			goto rollback;
	}
	/* Backend frame lock -> owner lock -> RDMA lock. */
	if (transaction == S7D_SCANOUT_TRANSACTION_RDMA) {
		ret = s7d_rdma_submit(s->rdma, entries, count);
		if (ret)
			goto rollback;
	}
	s->pending = candidate;
	s->pending_afbc_epoch = 0;
	s->pending_afbc_unchanged = afbc_unchanged;
	s->pending_transaction = transaction;
	s->phase = transaction == S7D_SCANOUT_TRANSACTION_RDMA ?
		S7D_SCANOUT_RDMA : S7D_SCANOUT_DECODE;
	s->vblank_valid = false;
	s->early_complete = false;
	goto out;
rollback:
	if (cancel && (!afbc_unchanged || transaction == S7D_SCANOUT_TRANSACTION_REPLAY) &&
	    cancel(data, candidate.generation)) {
		/* An uncertain stage still owns its header and framebuffer. */
		s->pending = candidate;
		s->pending_afbc_epoch = 0;
		s->pending_transaction = transaction;
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
				       false, false, S7D_SCANOUT_TRANSACTION_RDMA, NULL,
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
	bool early = false;

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
		s->early_complete = frame->early &&
			(!s->pending.afbc.plan.surface_mask || s->pending_afbc_unchanged);
	}
	if (s->phase != S7D_SCANOUT_VBLANK)
		goto out;
	if (s->wait_field) {
		early = s->early_complete && frame->early && frame->idle && s->vblank_valid &&
			frame->field == s->applied_field && frame->field == s->vblank_field;

		if (!early && (!vblank || !frame->idle || frame->field == s->applied_field))
			goto out;
	} else if (!vblank) {
		goto out;
	}
	if (s->pending.afbc.plan.surface_mask &&
	    s->pending_transaction == S7D_SCANOUT_TRANSACTION_REPLAY &&
	    (!frame->afbc_gate || (!vblank && !early) || !frame->idle ||
	     !s->pending_afbc_epoch ||
	     (!s->pending_afbc_unchanged &&
	      (frame->afbc_handover_generation != s->pending.afbc.generation ||
	       frame->afbc_handover_epoch != s->pending_afbc_epoch))))
		goto out;
	if (s->pending.afbc.plan.surface_mask &&
	    s->pending_transaction != S7D_SCANOUT_TRANSACTION_REPLAY &&
	    (!frame->afbc_gate ||
	     frame->afbc_completed_generation != s->pending.afbc.generation ||
	     (!s->pending_afbc_unchanged &&
	      (!vblank || !frame->idle || !s->pending_afbc_epoch ||
	       frame->afbc_completed_epoch != s->pending_afbc_epoch))))
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
	s->pending_afbc_unchanged = false;
	s->pending_transaction = S7D_SCANOUT_TRANSACTION_RDMA;
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
	struct drm_framebuffer *active[S7D_SCANOUT_MAX_PLANES];
	struct drm_framebuffer *pending[S7D_SCANOUT_MAX_PLANES];
	unsigned long flags;
	int ret;

	/* Prevent a late IRQ from completing a transition while reset sleeps. */
	s7d_scanout_fail(s);
	ret = s7d_rdma_quiesce(s->rdma);
	if (ret)
		return ret;
	flush_work(&s->retire_work);
	spin_lock_irqsave(&s->lock, flags);
	memcpy(active, s->active.fb, sizeof(active));
	memcpy(pending, s->pending.fb, sizeof(pending));
	s->active = (struct s7d_scanout_buffers) {0};
	s->pending = (struct s7d_scanout_buffers) {0};
	s->phase = S7D_SCANOUT_STOPPED;
	s->pending_afbc_epoch = 0;
	s->pending_afbc_unchanged = false;
	s->pending_transaction = S7D_SCANOUT_TRANSACTION_RDMA;
	s->wait_field = false;
	s->fault = false;
	spin_unlock_irqrestore(&s->lock, flags);
	buffers_put(active);
	buffers_put(pending);
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
