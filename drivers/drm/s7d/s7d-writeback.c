// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/atomic.h>
#include <linux/dma-fence.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_writeback.h>

#include "s7d-crtc.h"
#include "s7d-writeback.h"

#define S7D_WB_ALIGN 64U

struct s7d_writeback_job {
	struct s7d_writeback *wb;
	struct s7d_writeback_buffer buffer;
};

struct s7d_writeback_state {
	struct drm_connector_state base;
	bool checked;
};

struct s7d_writeback {
	struct drm_writeback_connector connector;
	struct drm_crtc *crtc;
	struct s7d_writeback_caps caps;
	const struct s7d_writeback_ops *ops;
	void *data;
	struct mutex operation_lock; /* Serializes capture, cancel and queueing. */
	spinlock_t lock; /* Reservation and deferred cleanup. */
	wait_queue_head_t cleanup_wait;
	atomic_t jobs;
	struct s7d_writeback_job *reservation;
	struct s7d_writeback_buffer buffer;
	struct drm_crtc_commit *commit;
	int error;
	bool queued;
	bool started;
	bool stopping;
};

static struct s7d_writeback *connector_to_wb(struct drm_connector *connector)
{
	return container_of(drm_connector_to_writeback(connector),
			    struct s7d_writeback, connector);
}

static struct s7d_writeback_state *to_wb_state(struct drm_connector_state *state)
{
	return container_of(state, struct s7d_writeback_state, base);
}

static bool wb_format_supported(const struct s7d_writeback_caps *caps, u32 format)
{
	unsigned int i;

	for (i = 0; i < caps->num_formats; i++)
		if (caps->formats[i] == format)
			return true;
	return false;
}

static int wb_plane_check(struct drm_framebuffer *fb, unsigned int plane,
			  dma_addr_t *dma, u64 *size)
{
	const struct drm_format_info *info = fb->format;
	u32 width = drm_format_info_plane_width(info, fb->width, plane);
	u32 height = drm_format_info_plane_height(info, fb->height, plane);
	struct drm_gem_dma_object *gem;
	struct drm_gem_object *obj;
	u64 row, object_end, start, end;

	obj = drm_gem_fb_get_obj(fb, plane);
	if (!obj || obj->dev != fb->dev || obj->import_attach)
		return -EINVAL;
	gem = to_drm_gem_dma_obj(obj);
	if (gem->map_noncoherent)
		return -EINVAL;
	if (check_mul_overflow((u64)width, (u64)info->cpp[plane], &row) ||
	    fb->pitches[plane] < row || !IS_ALIGNED(fb->pitches[plane], S7D_WB_ALIGN) ||
	    check_mul_overflow((u64)fb->pitches[plane], (u64)height, size) ||
	    check_add_overflow((u64)fb->offsets[plane], *size, &object_end) ||
	    object_end > obj->size ||
	    check_add_overflow((u64)gem->dma_addr, (u64)fb->offsets[plane], &start) ||
	    !IS_ALIGNED(start, S7D_WB_ALIGN) ||
	    check_add_overflow(start, *size - 1, &end) || end > U32_MAX)
		return -ERANGE;
	*dma = start;
	return 0;
}

static int wb_buffer_check(struct s7d_writeback *wb, struct drm_framebuffer *fb,
			   struct s7d_writeback_buffer *buffer)
{
	const struct drm_format_info *info = fb->format;
	struct s7d_writeback_buffer result = {0};
	int ret;

	if (fb->dev != wb->connector.base.dev ||
	    !wb_format_supported(&wb->caps, info->format) || info->num_planes > 2 ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    fb->flags & DRM_MODE_FB_INTERLACED || !fb->width || !fb->height ||
	    fb->width % info->hsub || fb->height % info->vsub ||
	    fb->width > wb->caps.max_width || fb->height > wb->caps.max_height)
		return -EINVAL;
	ret = wb_plane_check(fb, 0, &result.dma, &result.size);
	if (!ret && info->num_planes == 2)
		ret = wb_plane_check(fb, 1, &result.chroma_dma, &result.chroma_size);
	if (ret)
		return ret;
	result.format = info->format;
	result.width = fb->width;
	result.height = fb->height;
	result.pitch = fb->pitches[0];
	if (info->num_planes == 2)
		result.chroma_pitch = fb->pitches[1];
	*buffer = result;
	return 0;
}

static int wb_source_check(struct drm_gem_object *target,
			   const struct drm_plane_state *state)
{
	unsigned int i;

	if (!state->crtc || !state->fb)
		return 0;
	for (i = 0; i < state->fb->format->num_planes; i++) {
		struct drm_gem_object *obj = drm_gem_fb_get_obj(state->fb, i);

		if (!obj || obj == target || obj->resv == target->resv)
			return -EINVAL;
	}
	return 0;
}

static int wb_output_check(struct s7d_writeback *wb,
			   const struct s7d_crtc_state *state,
			   const struct s7d_writeback_buffer *buffer)
{
	const struct drm_crtc_state *base = &state->base;

	if (base->crtc != wb->crtc || !base->active || !base->enable ||
	    !state->valid || base->vrr_enabled || base->self_refresh_active ||
	    base->async_flip ||
	    buffer->width > base->adjusted_mode.hdisplay ||
	    buffer->height > base->adjusted_mode.vdisplay)
		return -EINVAL;
	return wb->ops->check(wb->data, state, buffer);
}

static void wb_state_destroy(struct drm_connector *connector,
			     struct drm_connector_state *state)
{
	__drm_atomic_helper_connector_destroy_state(state);
	kfree(to_wb_state(state));
}

static void wb_state_reset(struct drm_connector *connector)
{
	struct s7d_writeback_state *state;

	if (connector->state)
		wb_state_destroy(connector, connector->state);
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	__drm_atomic_helper_connector_reset(connector, state ? &state->base : NULL);
}

static struct drm_connector_state *wb_state_duplicate(struct drm_connector *connector)
{
	struct s7d_writeback_state *state;

	if (WARN_ON(!connector->state))
		return NULL;
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	return &state->base;
}

static enum drm_mode_status wb_mode_valid(struct drm_connector *connector,
					struct drm_display_mode *mode)
{
	struct s7d_writeback *wb = connector_to_wb(connector);

	return wb->ops->mode_valid(wb->data, mode);
}

static int wb_get_modes(struct drm_connector *connector)
{
	struct s7d_writeback *wb = connector_to_wb(connector);
	unsigned int i;
	int count = 0;

	for (i = 0; i < wb->caps.num_modes; i++) {
		struct drm_display_mode *mode;

		if (wb->ops->mode_valid(wb->data, &wb->caps.modes[i]) != MODE_OK)
			continue;
		mode = drm_mode_duplicate(connector->dev, &wb->caps.modes[i]);
		if (!mode)
			break;
		mode->type |= DRM_MODE_TYPE_DRIVER;
		drm_mode_probed_add(connector, mode);
		count++;
	}
	return count;
}

static int wb_atomic_check(struct drm_connector *connector, struct drm_atomic_state *state)
{
	struct s7d_writeback *wb = connector_to_wb(connector);
	struct drm_connector_state *conn = drm_atomic_get_new_connector_state(state, connector);
	struct s7d_writeback_buffer buffer;
	struct drm_plane *plane;
	int ret;

	to_wb_state(conn)->checked = false;
	if (!conn->writeback_job || !conn->writeback_job->fb)
		return 0;
	if (conn->crtc != wb->crtc)
		return -EINVAL;
	ret = drm_atomic_helper_check_wb_connector_state(connector, state);
	if (ret)
		return ret;
	ret = wb_buffer_check(wb, conn->writeback_job->fb, &buffer);
	if (ret)
		return ret;
	drm_for_each_plane(plane, state->dev) {
		struct drm_plane_state *ps = drm_atomic_get_plane_state(state, plane);

		if (IS_ERR(ps))
			return PTR_ERR(ps);
	}
	return 0;
}

int s7d_writeback_atomic_check(struct s7d_writeback *wb, struct drm_atomic_state *state)
{
	struct drm_connector_state *conn;
	struct drm_crtc_state *crtc;
	struct drm_plane_state *old, *new;
	struct s7d_writeback_buffer buffer;
	struct drm_gem_object *target;
	struct drm_plane *plane;
	int ret;

	if (!wb)
		return 0;
	conn = drm_atomic_get_new_connector_state(state, &wb->connector.base);
	if (!conn || !conn->writeback_job || !conn->writeback_job->fb)
		return 0;
	to_wb_state(conn)->checked = false;
	if (conn->crtc != wb->crtc)
		return -EINVAL;
	crtc = drm_atomic_get_new_crtc_state(state, wb->crtc);
	if (!crtc)
		return -EINVAL;
	ret = wb_buffer_check(wb, conn->writeback_job->fb, &buffer);
	if (ret)
		return ret;
	target = drm_gem_fb_get_obj(conn->writeback_job->fb, 0);
	drm_for_each_plane(plane, state->dev) {
		old = drm_atomic_get_old_plane_state(state, plane);
		new = drm_atomic_get_new_plane_state(state, plane);
		if (!old || !new)
			return -EINVAL;
		ret = wb_source_check(target, old);
		if (ret)
			return ret;
		ret = wb_source_check(target, new);
		if (ret)
			return ret;
	}
	ret = wb_output_check(wb, to_s7d_crtc_state(crtc), &buffer);
	if (!ret)
		to_wb_state(conn)->checked = true;
	return ret;
}

static int wb_prepare(struct drm_writeback_connector *connector, struct drm_writeback_job *job)
{
	struct s7d_writeback *wb = container_of(connector, struct s7d_writeback, connector);
	struct s7d_writeback_job *private;
	unsigned long flags;
	int ret;

	private = kzalloc(sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;
	private->wb = wb;
	ret = wb_buffer_check(wb, job->fb, &private->buffer);
	if (ret)
		goto free;
	spin_lock_irqsave(&wb->lock, flags);
	if (wb->stopping) {
		ret = -ESHUTDOWN;
	} else if (wb->reservation) {
		ret = -EBUSY;
	} else {
		wb->reservation = private;
		atomic_inc(&wb->jobs);
		job->priv = private;
	}
	spin_unlock_irqrestore(&wb->lock, flags);
	if (!ret)
		return 0;
free:
	kfree(private);
	return ret;
}

static void wb_cleanup(struct drm_writeback_connector *connector, struct drm_writeback_job *job)
{
	struct s7d_writeback_job *private = job->priv;
	struct s7d_writeback *wb;
	unsigned long flags;

	if (!private)
		return;
	wb = private->wb;
	spin_lock_irqsave(&wb->lock, flags);
	if (wb->reservation == private) {
		WARN_ON(wb->queued || wb->started);
		wb->reservation = NULL;
	}
	kfree(private);
	atomic_dec(&wb->jobs);
	wake_up_all(&wb->cleanup_wait);
	spin_unlock_irqrestore(&wb->lock, flags);
}

static void wb_atomic_commit(struct drm_connector *connector, struct drm_atomic_state *state)
{
	struct s7d_writeback *wb = connector_to_wb(connector);
	struct drm_connector_state *conn = drm_atomic_get_new_connector_state(state, connector);
	struct drm_crtc_state *crtc = drm_atomic_get_new_crtc_state(state, wb->crtc);
	struct s7d_writeback_job *private = conn->writeback_job->priv;
	unsigned long flags;

	mutex_lock(&wb->operation_lock);
	spin_lock_irqsave(&wb->lock, flags);
	if (WARN_ON(wb->reservation != private || wb->queued || !private)) {
		spin_unlock_irqrestore(&wb->lock, flags);
		/* The job never reaches the queue; still fail its out-fence. */
		if (conn->writeback_job->out_fence) {
			dma_fence_set_error(conn->writeback_job->out_fence, -EIO);
			dma_fence_signal(conn->writeback_job->out_fence);
		}
		mutex_unlock(&wb->operation_lock);
		return;
	}
	wb->buffer = private->buffer;
	wb->error = !to_wb_state(conn)->checked || !crtc || !crtc->commit ? -EINVAL : 0;
	if (wb->stopping)
		wb->error = -ESHUTDOWN;
	wb->commit = crtc && crtc->commit ? drm_crtc_commit_get(crtc->commit) : NULL;
	wb->queued = true;
	wb->started = false;
	spin_unlock_irqrestore(&wb->lock, flags);
	drm_writeback_queue_job(&wb->connector, conn);
	mutex_unlock(&wb->operation_lock);
}

static int wb_finish(struct s7d_writeback *wb, int status)
{
	struct drm_crtc_commit *commit;
	unsigned long flags;

	lockdep_assert_held(&wb->operation_lock);
	spin_lock_irqsave(&wb->lock, flags);
	if (WARN_ON(!wb->queued)) {
		spin_unlock_irqrestore(&wb->lock, flags);
		return -ENOENT;
	}
	commit = wb->commit;
	wb->commit = NULL;
	wb->reservation = NULL;
	wb->queued = false;
	wb->started = false;
	wb->error = 0;
	spin_unlock_irqrestore(&wb->lock, flags);
	if (commit)
		drm_crtc_commit_put(commit);
	drm_writeback_signal_completion(&wb->connector, status);
	return status;
}

bool s7d_writeback_has_job(struct s7d_writeback *wb, struct drm_atomic_state *state)
{
	struct drm_connector_state *conn;

	if (!wb)
		return false;
	conn = drm_atomic_get_new_connector_state(state, &wb->connector.base);
	return conn && conn->writeback_job && conn->writeback_job->fb;
}

int s7d_writeback_capture(struct s7d_writeback *wb, const struct s7d_crtc_state *state)
{
	struct s7d_writeback_result result;
	bool fault = false;
	int ret;

	if (!wb || !state)
		return -EINVAL;
	mutex_lock(&wb->operation_lock);
	if (!wb->queued) {
		ret = -ENOENT;
		goto unlock;
	}
	if (wb->started) {
		ret = wb->error ? wb->error : -EBUSY;
		goto unlock;
	}
	ret = wb->error;
	if (!ret)
		ret = s7d_crtc_last_error(wb->crtc);
	if (!ret && wb->commit != state->base.commit)
		ret = -ESTALE;
	if (!ret)
		ret = wb_output_check(wb, state, &wb->buffer);
	if (ret) {
		wb_finish(wb, ret);
		goto unlock;
	}
	wb->started = true;
	result = wb->ops->capture(wb->data, state, &wb->buffer);
	ret = result.status > 0 ? -EIO : result.status;
	/* Armed: s7d_writeback_wait() completes the job. */
	if (!ret && !result.target_retired)
		goto unlock;
	if (!result.target_retired) {
		wb->error = ret;
		fault = true;
		goto unlock;
	}
	wb_finish(wb, ret);
unlock:
	mutex_unlock(&wb->operation_lock);
	if (fault)
		s7d_crtc_link_error(wb->crtc, ret);
	return ret;
}

int s7d_writeback_wait(struct s7d_writeback *wb)
{
	struct s7d_writeback_result result;
	bool fault = false;
	int ret;

	if (!wb)
		return -EINVAL;
	ret = wb->ops->wait(wb->data);
	mutex_lock(&wb->operation_lock);
	/* A cancel has already completed the job, or retained its target. */
	if (!wb->queued || !wb->started || wb->error) {
		ret = wb->queued && wb->error ? wb->error : ret ? ret : -ECANCELED;
		goto unlock;
	}
	result = wb->ops->finish(wb->data, ret);
	ret = result.status > 0 ? -EIO : result.status;
	if (!ret)
		ret = s7d_crtc_last_error(wb->crtc);
	if (!result.target_retired) {
		wb->error = ret ? ret : -EIO;
		ret = wb->error;
		fault = true;
		goto unlock;
	}
	wb_finish(wb, ret);
unlock:
	mutex_unlock(&wb->operation_lock);
	if (fault)
		s7d_crtc_link_error(wb->crtc, ret);
	return ret;
}

int s7d_writeback_cancel(struct s7d_writeback *wb, int error)
{
	struct s7d_writeback_result result;
	int ret = 0;

	if (!wb)
		return 0;
	if (error >= 0)
		error = -ECANCELED;
	mutex_lock(&wb->operation_lock);
	if (!wb->queued)
		goto unlock;
	if (wb->started) {
		result = wb->ops->cancel(wb->data);
		if (!result.target_retired) {
			ret = result.status < 0 ? result.status : -EIO;
			wb->error = ret;
			goto unlock;
		}
	}
	wb_finish(wb, error);
unlock:
	mutex_unlock(&wb->operation_lock);
	return ret;
}

int s7d_writeback_shutdown(struct s7d_writeback *wb)
{
	unsigned long flags;
	int ret;

	if (!wb)
		return 0;
	spin_lock_irqsave(&wb->lock, flags);
	wb->stopping = true;
	spin_unlock_irqrestore(&wb->lock, flags);
	ret = s7d_writeback_cancel(wb, -ESHUTDOWN);
	if (ret)
		return ret;
	if (!wait_event_timeout(wb->cleanup_wait, !atomic_read(&wb->jobs), HZ))
		return -EBUSY;
	spin_lock_irqsave(&wb->lock, flags);
	spin_unlock_irqrestore(&wb->lock, flags);
	return 0;
}

struct drm_connector *s7d_writeback_connector(struct s7d_writeback *wb)
{
	return &wb->connector.base;
}

struct drm_encoder *s7d_writeback_encoder(struct s7d_writeback *wb)
{
	return &wb->connector.encoder;
}

static const struct drm_connector_funcs wb_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = wb_state_reset,
	.atomic_duplicate_state = wb_state_duplicate,
	.atomic_destroy_state = wb_state_destroy,
};

static const struct drm_connector_helper_funcs wb_helper_funcs = {
	.get_modes = wb_get_modes,
	.mode_valid = wb_mode_valid,
	.atomic_check = wb_atomic_check,
	.prepare_writeback_job = wb_prepare,
	.cleanup_writeback_job = wb_cleanup,
	.atomic_commit = wb_atomic_commit,
};

int s7d_writeback_init(struct drm_device *drm, struct drm_crtc *crtc,
		      const struct s7d_writeback_caps *caps,
		      const struct s7d_writeback_ops *ops, void *data,
		      struct s7d_writeback **result)
{
	struct s7d_writeback *wb;
	unsigned int i;
	int ret;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!drm || !crtc || crtc->dev != drm || !s7d_crtc_is_native(crtc) ||
	    !caps || !caps->max_width || !caps->max_height ||
	    caps->max_width > INT_MAX || caps->max_height > INT_MAX ||
	    !caps->formats || !caps->num_formats || caps->num_formats > INT_MAX ||
	    caps->max_bpc < 8 || caps->max_bpc > 16 || !caps->modes || !caps->num_modes ||
	    !ops || !ops->mode_valid || !ops->check || !ops->capture || !ops->wait ||
	    !ops->finish || !ops->cancel)
		return -EINVAL;
	for (i = 0; i < caps->num_formats; i++) {
		const struct drm_format_info *info = drm_format_info(caps->formats[i]);

		if (!info || info->num_planes > 2)
			return -EINVAL;
	}
	for (i = 0; i < caps->num_modes; i++) {
		const struct drm_display_mode *mode = &caps->modes[i];

		if (mode->hdisplay <= 0 || mode->vdisplay <= 0 ||
		    mode->hdisplay > caps->max_width || mode->vdisplay > caps->max_height ||
		    ops->mode_valid(data, mode) != MODE_OK)
			return -EINVAL;
	}

	wb = drmm_kzalloc(drm, sizeof(*wb), GFP_KERNEL);
	if (!wb)
		return -ENOMEM;
	wb->crtc = crtc;
	wb->caps = *caps;
	wb->ops = ops;
	wb->data = data;
	ret = drmm_mutex_init(drm, &wb->operation_lock);
	if (ret)
		return ret;
	spin_lock_init(&wb->lock);
	init_waitqueue_head(&wb->cleanup_wait);
	atomic_set(&wb->jobs, 0);
	ret = drmm_encoder_init(drm, &wb->connector.encoder, NULL,
				DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret)
		return ret;
	wb->connector.encoder.possible_crtcs = drm_crtc_mask(crtc);
	drm_connector_helper_add(&wb->connector.base, &wb_helper_funcs);
	ret = drmm_writeback_connector_init(drm, &wb->connector,
		&wb_connector_funcs, &wb->connector.encoder,
		wb->caps.formats, wb->caps.num_formats);
	if (ret)
		return ret;
	*result = wb;
	return 0;
}
