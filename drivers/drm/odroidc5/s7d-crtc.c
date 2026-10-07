// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/dma-fence.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "s7d-crtc.h"
#include "s7d-afbc-engine.h"
#include "s7d-plane.h"

struct s7d_crtc {
	struct drm_crtc base;
	struct s7d_scanout *scanout;
	const struct s7d_crtc_ops *ops;
	void *data;
	u8 revision;
	/* Serializes commit callbacks, recovery work and shutdown. */
	struct mutex mutex;
	struct work_struct error_work;
	struct delayed_work timeout_work;
	bool prepared;
	bool vblank_on;
	bool vblank_ref;
	int last_error;
	/* Protected by drm_device.event_lock, including start-vs-IRQ ordering. */
	struct drm_pending_vblank_event *event;
	bool starting;
	bool completed_while_starting;
	bool awaiting_frame;
	unsigned long deadline;
};

#define to_s7d_crtc(c) container_of(c, struct s7d_crtc, base)

static void s7d_crtc_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_s7d_crtc_state(state));
}

static void s7d_crtc_reset(struct drm_crtc *crtc)
{
	struct s7d_crtc_state *state;

	if (crtc->state)
		s7d_crtc_destroy_state(crtc, crtc->state);
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	__drm_atomic_helper_crtc_reset(crtc, state ? &state->base : NULL);
}

static struct drm_crtc_state *s7d_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct s7d_crtc_state *state;

	if (!crtc->state)
		return NULL;
	state = kmemdup(to_s7d_crtc_state(crtc->state), sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);
	state->video_unchanged = false;
	state->input_captured = false;
	state->primary_submitted = false;
	state->connector_submitted = false;
	state->primary_dependency = false;
	state->afbc_unchanged = false;
	return &state->base;
}

void s7d_crtc_capture_input(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct drm_crtc_state *base = drm_atomic_get_new_crtc_state(atomic, crtc);
	struct s7d_crtc_state *state;
	struct drm_connector *connector;
	struct drm_connector_state *connector_state;
	int i;

	if (!base || !s7d_crtc_is_native(crtc))
		return;
	state = to_s7d_crtc_state(base);
	if (state->input_captured)
		return;
	state->input_captured = true;
	state->primary_submitted = drm_atomic_get_new_plane_state(atomic, crtc->primary);
	for_each_new_connector_in_state(atomic, connector, connector_state, i)
		if (connector_state->crtc == crtc ||
		    drm_atomic_get_old_connector_state(atomic, connector)->crtc == crtc)
			state->connector_submitted = true;
}

void s7d_crtc_primary_dependency(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct drm_crtc_state *base = drm_atomic_get_new_crtc_state(atomic, crtc);
	struct s7d_crtc_state *state;

	if (!base || !s7d_crtc_is_native(crtc))
		return;
	state = to_s7d_crtc_state(base);
	state->primary_dependency = true;
	state->afbc_unchanged = false;
}

static enum drm_mode_status
s7d_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct s7d_encp_state state;

	return s7d_encp_build_state(mode, &state);
}

static void s7d_crtc_check_video_update(struct s7d_crtc_state *state,
				      const struct s7d_crtc_state *old)
{
	state->buffers.video = (struct s7d_scanout_video) {0};
	if (state->buffers.fb[S7D_PLANE_VIDEO])
		state->buffers.video = (struct s7d_scanout_video) {
			.fb = state->buffers.fb[S7D_PLANE_VIDEO],
			.pipeline = state->video,
			.csc = state->csc,
			.postblend = state->postblend,
		};
	state->video_unchanged = old && old->valid && old->base.active &&
		!drm_atomic_crtc_needs_modeset(&state->base) &&
		s7d_scanout_video_equal(&old->buffers, &state->buffers);
}

static void s7d_crtc_build_update(struct s7d_crtc_state *state)
{
	unsigned int count = 0, i;

	for (i = 0; i < S7D_OSD_UPDATE_REG_COUNT; i++) {
		u32 reg = le32_to_cpu(state->osd.update[i].reg);

		/* Leave the selected AFBC MIF fixed; the other OSD stays linear. */
		if (state->buffers.afbc.fb) {
			u32 mask = state->buffers.afbc.plan.surface_mask;

			if ((mask == BIT(0) &&
			     (reg == 0x1a1b || reg == 0x1a14 || reg == 0x1a15 ||
			      reg == 0x1a1c || reg == 0x1a1d || reg == 0x1a2d ||
			      reg == 0x1a2f)) ||
			    (mask == BIT(1) &&
			     (reg == 0x1a3b || reg == 0x1a65 || reg == 0x1a66 ||
			      reg == 0x1a3c || reg == 0x1a3d || reg == 0x1a4d ||
			      reg == 0x1abd)))
				continue;
		}
		state->update[count++] = state->osd.update[i];
	}
	if (!state->video_unchanged) {
		memcpy(state->update + count, state->video.update,
		       state->video.update_count * sizeof(*state->update));
		count += state->video.update_count;
		for (i = 0; state->video.update_count && i < S7D_CSC_MATRIX_REG_COUNT; i++) {
			state->update[count++] = (struct s7d_rdma_entry) {
				.reg = cpu_to_le32(state->csc.matrix[i].reg),
				.value = cpu_to_le32(state->csc.matrix[i].value),
			};
		}
	}
	memcpy(state->update + count, state->postblend.regs,
	       sizeof(state->postblend.regs));
	count += S7D_POSTBLEND_REG_COUNT;
	/* Block the blend source before disabling fetch; enable after its setup. */
	if (!state->video_unchanged)
		state->update[count++] = (struct s7d_rdma_entry) {
			.reg = cpu_to_le32(state->video.control.reg),
			.value = cpu_to_le32(state->video.control.value),
		};
	state->update_count = count;
}

static int s7d_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct drm_crtc_state *base = drm_atomic_get_new_crtc_state(atomic, crtc);
	struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(atomic, crtc);
	struct s7d_crtc_state *state = to_s7d_crtc_state(base);
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct drm_plane *plane;
	struct s7d_osd_layer layers[2] = {0};
	const struct s7d_plane_state *video = NULL;
	struct drm_rect video_dst;
	enum drm_color_encoding encoding = DRM_COLOR_YCBCR_BT709;
	enum drm_color_range range = DRM_COLOR_YCBCR_LIMITED_RANGE;
	bool primary_changed = false, afbc_position_changed = false;
	int ret;

	state->valid = false;
	state->video_unchanged = false;
	state->afbc_unchanged = false;
	state->buffers = (struct s7d_scanout_buffers) {0};
	if (!base->active)
		return 0;
	if (!base->enable || base->vrr_enabled || base->self_refresh_active ||
	    base->degamma_lut || base->gamma_lut || base->ctm || base->async_flip)
		return -EINVAL;
	ret = drm_atomic_helper_check_crtc_primary_plane(base);
	if (ret)
		return ret;
	/* Include unchanged planes so fences and buffer ownership cover the frame. */
	drm_for_each_plane(plane, crtc->dev) {
		const struct drm_plane_helper_funcs *funcs = plane->helper_private;
		struct drm_plane_state *ps, *old_ps;
		struct s7d_plane_state *checked;
		enum s7d_plane_slot slot;

		if (!s7d_plane_is_native(plane))
			continue;
		ps = drm_atomic_get_plane_state(atomic, plane);
		if (IS_ERR(ps))
			return PTR_ERR(ps);
		ret = funcs->atomic_check(plane, atomic);
		if (ret)
			return ret;
		checked = to_s7d_plane_state(ps);
		old_ps = drm_atomic_get_old_plane_state(atomic, plane);
		if (!ps->visible)
			continue;
		if (ps->crtc != crtc || !ps->fb)
			return -EINVAL;
		slot = s7d_plane_slot(plane);
		if (slot == S7D_PLANE_PRIMARY)
			primary_changed = !old_ps || old_ps->fb != ps->fb ||
				old_ps->src_x != ps->src_x || old_ps->src_y != ps->src_y ||
				old_ps->src_w != ps->src_w || old_ps->src_h != ps->src_h ||
				old_ps->crtc_x != ps->crtc_x || old_ps->crtc_y != ps->crtc_y ||
				old_ps->crtc_w != ps->crtc_w || old_ps->crtc_h != ps->crtc_h ||
				old_ps->alpha != ps->alpha ||
				old_ps->pixel_blend_mode != ps->pixel_blend_mode;
		if (checked->afbc_valid)
			afbc_position_changed = !old_ps ||
				old_ps->crtc_x != ps->crtc_x || old_ps->crtc_y != ps->crtc_y ||
				old_ps->crtc_w != ps->crtc_w || old_ps->crtc_h != ps->crtc_h;
		if (slot == S7D_PLANE_VIDEO) {
			if (!checked->video_valid)
				return -EINVAL;
			video = checked;
			video_dst = ps->dst;
			encoding = ps->color_encoding;
			range = ps->color_range;
		} else {
			if (!checked->osd_valid)
				return -EINVAL;
			if (slot != S7D_PLANE_PRIMARY && layers[1].enabled)
				return -ENOSPC;
			layers[slot == S7D_PLANE_PRIMARY ? 0 : 1] = checked->layer;
			if (checked->afbc_valid) {
				if (state->buffers.afbc.fb)
					return -ENOSPC;
				state->buffers.afbc = (struct s7d_scanout_afbc) {
					.fb = ps->fb,
					.plan = checked->afbc,
				};
			}
		}
		state->buffers.fb[slot] = ps->fb;
	}
	if (!state->buffers.fb[S7D_PLANE_PRIMARY])
		return -EINVAL;
	if (state->buffers.afbc.fb &&
	    (video || s7d_afbc_check_state(&state->buffers.afbc.plan) ||
	     (state->buffers.fb[S7D_PLANE_CURSOR] &&
	      state->buffers.afbc.plan.surface_mask != BIT(0))))
		return -EINVAL;
	/* Entry, owner, layout and alpha-route changes require stopped preparation. */
	if (old && old->active) {
		const struct s7d_scanout_afbc *before = &to_s7d_crtc_state(old)->buffers.afbc;
		const struct s7d_scanout_afbc *after = &state->buffers.afbc;

		if (!!before->fb != !!after->fb ||
		    (after->fb &&
		     (!s7d_afbc_same_layout(&before->plan, &after->plan) ||
		      afbc_position_changed ||
		      (after->plan.surface_mask == BIT(1) && primary_changed))))
			base->mode_changed = true;
	}
	if (base->mode.hdisplay != base->adjusted_mode.hdisplay ||
	    base->mode.vdisplay != base->adjusted_mode.vdisplay)
		return -EINVAL;
	if (s7d_encp_build_state(&base->adjusted_mode, &state->encp) != MODE_OK)
		return -EINVAL;
	ret = s7d_osd_build_pipeline(c->revision, base->adjusted_mode.hdisplay,
				     base->adjusted_mode.vdisplay, layers, &state->osd);
	if (ret)
		return ret;
	if (old && old->active &&
	    (!s7d_osd_scaler_same(&state->osd.scalers[0],
				 &to_s7d_crtc_state(old)->osd.scalers[0]) ||
	     !s7d_osd_scaler_same(&state->osd.scalers[1],
				 &to_s7d_crtc_state(old)->osd.scalers[1])))
		base->mode_changed = true;
	/* A fault requires a full disable/enable or link-recovery modeset. */
	if (READ_ONCE(c->last_error) && !drm_atomic_crtc_needs_modeset(base))
		return -EIO;
	ret = s7d_video_build_pipeline(video ? &video->video : NULL,
				       video ? &video_dst : NULL, &state->video);
	if (ret)
		return ret;
	ret = s7d_csc_build_state(encoding, range, &state->csc);
	if (ret)
		return ret;
	ret = s7d_postblend_build_state(base->adjusted_mode.hdisplay,
		base->adjusted_mode.vdisplay, video ? &video_dst : NULL,
		layers[1].enabled, &state->postblend);
	if (ret)
		return ret;
	s7d_crtc_check_video_update(state, old ? to_s7d_crtc_state(old) : NULL);
	state->afbc_unchanged = state->input_captured &&
		!state->primary_submitted && !state->connector_submitted &&
		!state->primary_dependency && old && old->active &&
		to_s7d_crtc_state(old)->valid &&
		!drm_atomic_crtc_needs_modeset(base) &&
		s7d_scanout_afbc_equal(&to_s7d_crtc_state(old)->buffers, &state->buffers);
	s7d_crtc_build_update(state);
	state->pixel_rate = (unsigned long)base->adjusted_mode.clock * 1000;
	ret = c->ops->check(c->data, state);
	if (!ret)
		state->valid = true;
	return ret;
}

/* Cancel a failed update, signal its out-fence as failed, and unblock helpers. */
static void s7d_crtc_abort_event(struct drm_crtc *crtc,
				 struct drm_pending_vblank_event *event, int error)
{
	unsigned long flags;

	if (!event)
		return;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (event->base.fence) {
		dma_fence_set_error(event->base.fence, error);
		dma_fence_signal(event->base.fence);
	}
	if (event->base.completion) {
		complete_all(event->base.completion);
		event->base.completion_release(event->base.completion);
		event->base.completion = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	drm_event_cancel_free(crtc->dev, &event->base);
}

static void s7d_crtc_abort_pending(struct s7d_crtc *c, int error)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	event = c->event;
	c->event = NULL;
	c->starting = false;
	c->completed_while_starting = false;
	c->awaiting_frame = false;
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	s7d_crtc_abort_event(&c->base, event, error);
}

static int s7d_crtc_arm_event(struct s7d_crtc *c, struct drm_crtc_state *state,
			      bool starting)
{
	unsigned long flags;
	unsigned long timeout;
	int ret = 0;

	/* Keep recovery below the atomic helpers' ten-second wait limit. */
	timeout = msecs_to_jiffies(clamp_t(u64,
		DIV_ROUND_UP_ULL((u64)state->adjusted_mode.htotal *
				 state->adjusted_mode.vtotal * 6,
				 state->adjusted_mode.clock), 500, 9000));
	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	if (c->awaiting_frame) {
		ret = -EBUSY;
	} else {
		c->event = state->event;
		state->event = NULL;
		c->starting = starting;
		c->completed_while_starting = false;
		c->awaiting_frame = true;
		c->deadline = jiffies + timeout;
		mod_delayed_work(system_wq, &c->timeout_work, timeout);
	}
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	return ret;
}

static int s7d_crtc_stop(struct s7d_crtc *c)
{
	int ret;

	if (c->vblank_ref) {
		drm_crtc_vblank_put(&c->base);
		c->vblank_ref = false;
	}
	if (c->vblank_on) {
		drm_crtc_vblank_off(&c->base);
		c->vblank_on = false;
	}
	/* A never-used CRTC must not reset or blank the firmware display. */
	if (!c->prepared)
		return 0;
	ret = c->ops->stop(c->data);
	if (ret)
		return ret;
	ret = s7d_scanout_quiesce(c->scanout);
	if (ret)
		return ret;
	WRITE_ONCE(c->prepared, false);
	c->ops->release(c->data);
	return 0;
}

static void s7d_crtc_error_work(struct work_struct *work)
{
	struct s7d_crtc *c = container_of(work, struct s7d_crtc, error_work);
	int error, ret;

	mutex_lock(&c->mutex);
	error = READ_ONCE(c->last_error);
	if (error) {
		ret = s7d_crtc_stop(c);
		if (ret)
			drm_err(c->base.dev, "S7D stop failed: %d; retaining scanout resources\n", ret);
		s7d_crtc_abort_pending(c, error);
	}
	mutex_unlock(&c->mutex);
	/* Connector/DRM modeset locks must never nest inside our mutex. */
	if (error)
		c->ops->report_error(c->data, error);
}

static void s7d_crtc_fail(struct s7d_crtc *c, int error, struct drm_crtc_state *state)
{
	unsigned long flags;

	/* Serialize faults with event delivery, including IRQs on other CPUs. */
	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	WRITE_ONCE(c->last_error, error);
	if (READ_ONCE(c->prepared))
		s7d_scanout_fail(c->scanout);
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	if (state && state->event) {
		struct drm_pending_vblank_event *event = state->event;

		state->event = NULL;
		s7d_crtc_abort_event(&c->base, event, error);
	}
	schedule_work(&c->error_work);
}

static void s7d_crtc_timeout_work(struct work_struct *work)
{
	struct s7d_crtc *c = container_of(to_delayed_work(work),
					struct s7d_crtc, timeout_work);
	unsigned long flags, now;
	bool expired = false;

	mutex_lock(&c->mutex);
	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	now = jiffies;
	if (c->awaiting_frame && !c->last_error) {
		if (time_before(now, c->deadline)) {
			mod_delayed_work(system_wq, &c->timeout_work,
					 c->deadline - now);
		} else {
			WRITE_ONCE(c->last_error, -ETIMEDOUT);
			s7d_scanout_fail(c->scanout);
			expired = true;
		}
	}
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	mutex_unlock(&c->mutex);
	if (expired)
		schedule_work(&c->error_work);
}

static void s7d_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct s7d_crtc_state *state = to_s7d_crtc_state(crtc->state);
	int ret;

	mutex_lock(&c->mutex);
	ret = -EINVAL;
	if (!state->valid || !state->buffers.fb[0])
		goto fail;
	ret = -EBUSY;
	if (c->prepared)
		goto fail;
	ret = c->ops->acquire(c->data);
	if (ret)
		goto fail;
	ret = s7d_scanout_begin_initial(c->scanout, &state->buffers);
	if (ret) {
		c->ops->release(c->data);
		goto fail;
	}
	/* stop must also unwind a partially failed prepare. */
	WRITE_ONCE(c->prepared, true);
	ret = c->ops->prepare(c->data, state);
	if (ret)
		goto fail;
	/* prepare left VENC stopped and all old IRQ delivery synchronized. */
	WRITE_ONCE(c->last_error, 0);
	ret = s7d_crtc_arm_event(c, &state->base, true);
	if (ret)
		goto fail;
	drm_crtc_vblank_on(crtc);
	c->vblank_on = true;
	ret = drm_crtc_vblank_get(crtc);
	if (ret)
		goto fail;
	c->vblank_ref = true;
	ret = c->ops->start(c->data);
	if (ret)
		goto fail;
	ret = s7d_scanout_initial_ready(c->scanout);
	if (ret)
		goto fail;
	if (!c->ops->wait_for_link)
		s7d_crtc_link_ready(crtc);
	mutex_unlock(&c->mutex);
	return;
fail:
	s7d_crtc_fail(c, ret, &state->base);
	mutex_unlock(&c->mutex);
}

static void s7d_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct s7d_crtc_state *state = to_s7d_crtc_state(crtc->state);
	int ret;

	if (!state->base.active || drm_atomic_crtc_needs_modeset(&state->base))
		return;
	mutex_lock(&c->mutex);
	ret = -EIO;
	if (READ_ONCE(c->last_error))
		goto fail;
	ret = -EINVAL;
	if (!state->valid || !state->buffers.fb[0])
		goto fail;
	ret = s7d_crtc_arm_event(c, &state->base, false);
	if (ret)
		goto fail;
	ret = c->ops->submit(c->data, state);
	if (ret)
		goto fail;
	mutex_unlock(&c->mutex);
	return;
fail:
	s7d_crtc_fail(c, ret, &state->base);
	mutex_unlock(&c->mutex);
}

static void s7d_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(atomic, crtc);
	unsigned long flags;
	int ret;

	mutex_lock(&c->mutex);
	ret = s7d_crtc_stop(c);
	s7d_crtc_abort_pending(c, ret ? ret : -ECANCELED);
	if (ret) {
		s7d_crtc_fail(c, ret, state);
	} else {
		WRITE_ONCE(c->last_error, 0);
		/* A confirmed disable completes its update, without claiming a frame. */
		if (!state->active && state->event) {
			spin_lock_irqsave(&crtc->dev->event_lock, flags);
			drm_crtc_send_vblank_event(crtc, state->event);
			state->event = NULL;
			spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		}
	}
	mutex_unlock(&c->mutex);
}

void s7d_crtc_irq(struct drm_crtc *crtc, bool vblank, enum s7d_rdma_result result,
		  const struct s7d_frame_state *frame)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	enum s7d_scanout_result scanout;
	unsigned long flags;

	if (!READ_ONCE(c->prepared))
		return;
	if (vblank)
		drm_crtc_handle_vblank(crtc);
	scanout = s7d_scanout_irq(c->scanout, vblank, result, frame);
	if (scanout == S7D_SCANOUT_FAULT) {
		s7d_crtc_fail(c, -EIO, NULL);
		return;
	}
	if (scanout != S7D_SCANOUT_FRAME_COMPLETE)
		return;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (c->starting) {
		c->completed_while_starting = true;
	} else if (!READ_ONCE(c->last_error)) {
		c->awaiting_frame = false;
		if (c->event) {
			drm_crtc_send_vblank_event(crtc, c->event);
			c->event = NULL;
		}
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static int s7d_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);

	if (!READ_ONCE(c->prepared))
		return -EINVAL;
	return c->ops->enable_vblank(c->data);
}

static void s7d_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);

	if (READ_ONCE(c->prepared))
		c->ops->disable_vblank(c->data);
}

void s7d_crtc_link_ready(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	unsigned long flags;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (READ_ONCE(c->prepared) && !READ_ONCE(c->last_error) && c->starting) {
		c->starting = false;
		if (c->completed_while_starting) {
			c->awaiting_frame = false;
			if (c->event) {
				drm_crtc_send_vblank_event(crtc, c->event);
				c->event = NULL;
			}
		}
		c->completed_while_starting = false;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

void s7d_crtc_link_error(struct drm_crtc *crtc, int error)
{
	s7d_crtc_fail(to_s7d_crtc(crtc), error < 0 ? error : -EIO, NULL);
}

int s7d_crtc_last_error(struct drm_crtc *crtc)
{
	return READ_ONCE(to_s7d_crtc(crtc)->last_error);
}

int s7d_crtc_shutdown(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	int ret;

	/* Caller has stopped commit production; IRQ masking happens in stop. */
	mutex_lock(&c->mutex);
	ret = s7d_crtc_stop(c);
	s7d_crtc_abort_pending(c, ret ? ret : -ECANCELED);
	mutex_unlock(&c->mutex);
	if (!ret) {
		cancel_delayed_work_sync(&c->timeout_work);
		cancel_work_sync(&c->error_work);
	}
	return ret;
}

static void s7d_crtc_cleanup(struct drm_device *drm, void *data)
{
	struct s7d_crtc *c = data;

	/* Parent must successfully shut down before releasing backend resources. */
	cancel_delayed_work_sync(&c->timeout_work);
	cancel_work_sync(&c->error_work);
	drm_WARN_ON(drm, c->prepared || c->event);
	mutex_destroy(&c->mutex);
}

static const struct drm_crtc_funcs s7d_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = s7d_crtc_reset,
	.atomic_duplicate_state = s7d_crtc_duplicate_state,
	.atomic_destroy_state = s7d_crtc_destroy_state,
	.enable_vblank = s7d_crtc_enable_vblank,
	.disable_vblank = s7d_crtc_disable_vblank,
};

bool s7d_crtc_is_native(const struct drm_crtc *crtc)
{
	return crtc && crtc->funcs == &s7d_crtc_funcs;
}

static const struct drm_crtc_helper_funcs s7d_crtc_helper_funcs = {
	.mode_valid = s7d_crtc_mode_valid,
	.atomic_check = s7d_crtc_atomic_check,
	.atomic_enable = s7d_crtc_atomic_enable,
	.atomic_disable = s7d_crtc_atomic_disable,
	.atomic_flush = s7d_crtc_atomic_flush,
};

struct drm_crtc *s7d_crtc_create(struct drm_device *drm, struct drm_plane *primary,
			       struct drm_plane *cursor,
			       u8 revision, struct s7d_scanout *scanout,
			       const struct s7d_crtc_ops *ops, void *data)
{
	struct s7d_crtc *c;
	int ret;

	if (!s7d_plane_is_primary(primary) || primary->dev != drm ||
	    !s7d_plane_is_native(cursor) || cursor->dev != drm ||
	    s7d_plane_slot(cursor) != S7D_PLANE_CURSOR ||
	    !scanout || !scanout->rdma || !ops || !ops->check || !ops->acquire || !ops->prepare ||
	    !ops->start || !ops->submit || !ops->stop || !ops->release || !ops->enable_vblank ||
	    !ops->disable_vblank || !ops->report_error)
		return ERR_PTR(-EINVAL);
	if (revision != S7D_OSD_REV_B)
		return ERR_PTR(-EOPNOTSUPP);
	c = drmm_crtc_alloc_with_planes(drm, struct s7d_crtc, base, primary, cursor,
				       &s7d_crtc_funcs, "S7D ENCP");
	if (IS_ERR(c))
		return ERR_CAST(c);
	c->scanout = scanout;
	c->ops = ops;
	c->data = data;
	c->revision = revision;
	mutex_init(&c->mutex);
	INIT_WORK(&c->error_work, s7d_crtc_error_work);
	INIT_DELAYED_WORK(&c->timeout_work, s7d_crtc_timeout_work);
	ret = drmm_add_action_or_reset(drm, s7d_crtc_cleanup, c);
	if (ret)
		return ERR_PTR(ret);
	drm_crtc_helper_add(&c->base, &s7d_crtc_helper_funcs);
	return &c->base;
}
