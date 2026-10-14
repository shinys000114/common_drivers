// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/cache.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-vmalloc.h>

#include "s7d-codec-memory.h"
#include "s7d-codec-vb2.h"
#include "s7d-h264-input.h"
#include "s7d-v4l2.h"

#define S7D_OUTPUT_MIN_SIZE SZ_64K
#define S7D_OUTPUT_DEFAULT_SIZE SZ_2M
#define S7D_OUTPUT_MAX_SIZE SZ_16M

enum s7d_v4l2_state {
	S7D_V4L2_IDLE,
	S7D_V4L2_INITIALIZING,
	S7D_V4L2_CAPTURE_SETUP,
	S7D_V4L2_DECODING,
	S7D_V4L2_DRAINING,
	S7D_V4L2_STOPPED,
	S7D_V4L2_FAILED,
};

struct s7d_v4l2_buffer {
	struct v4l2_m2m_buffer m2m;
	struct s7d_codec_surface *surface;
	u64 token;
	u64 generation;
	/* Independent from the surface generation, which may be rebound. */
	u64 completion_epoch;
	u64 change_last_epoch;
};

struct s7d_v4l2_context {
	struct device *dev;
	struct s7d_codec_core *core;
	struct s7d_codec_session *session;
	struct s7d_codec_dma *staging;
	struct s7d_v4l2_backend_ops ops;
	struct s7d_v4l2_codec codec;
	void *backend;
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler controls;
	struct mutex queue_lock;
	struct mutex backend_lock;
	struct mutex retire_lock;
	spinlock_t state_lock;
	struct work_struct run_work;
	struct work_struct drain_work;
	struct delayed_work rebind_work;
	struct delayed_work change_work;
	struct s7d_codec_layout query_layout;
	struct s7d_v4l2_reconfigure change;
	struct s7d_codec_layout layout;
	struct v4l2_pix_format_mplane output;
	struct v4l2_pix_format_mplane capture;
	struct s7d_v4l2_buffer *surface[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_v4l2_buffer *active_input;
	enum s7d_v4l2_state state;
	u64 generation;
	u64 next_input_token;
	u64 last_consumed_token;
	u64 initial_token;
	u64 drain_token;
	u64 drain_epoch;
	u64 completion_epoch;
	u64 last_dequeued_epoch;
	u64 cancelled_input_token;
	u64 cancelled_drain_epoch;
	u32 capture_sequence;
	int stop_error;
	bool sequence_valid;
	bool prepared;
	bool initial_pending;
	bool output_streaming;
	bool capture_streaming;
	bool capture_ready;
	bool capture_stopping;
	bool capture_only_stop;
	bool capture_layout_retained;
	bool rebind_pending;
	bool drain_requested;
	bool last_pending;
	bool last_completing;
	bool retired;
	bool change_pending;
	bool change_stopped;
	bool change_capture_off;
	bool change_last_done;
	bool change_last_dequeued;
	bool change_draining;
};

static int s7d_clear_stream_locked(struct s7d_v4l2_context *ctx);
static int s7d_capture_rebind(struct s7d_v4l2_context *ctx);
static void s7d_rebind_work(struct work_struct *work);
static void s7d_change_work(struct work_struct *work);
static void s7d_complete_last(struct s7d_v4l2_context *ctx);
static void s7d_frame_ready(void *priv, const struct s7d_codec_frame *frame);
static int s7d_reconfigure_stop(struct s7d_v4l2_context *ctx);
static int s7d_reconfigure_start(struct s7d_v4l2_context *ctx);

static struct s7d_codec_layout s7d_query_layout(struct s7d_v4l2_context *ctx)
{
	lockdep_assert_held(&ctx->state_lock);
	return ctx->change_pending ? ctx->query_layout : ctx->layout;
}

static struct s7d_v4l2_buffer *s7d_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct s7d_v4l2_buffer, m2m.vb);
}

static bool s7d_output(enum v4l2_buf_type type)
{
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static bool s7d_capture(enum v4l2_buf_type type)
{
	return type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
}

static void s7d_capture_format(const struct s7d_codec_layout *layout,
			       struct v4l2_pix_format_mplane *format)
{
	unsigned int i;

	memset(format, 0, sizeof(*format));
	format->width = layout->coded_width;
	format->height = layout->coded_height;
	format->pixelformat = layout->format == S7D_CODEC_CAPTURE_NV21M ?
		V4L2_PIX_FMT_NV21M : V4L2_PIX_FMT_NV12M;
	format->field = V4L2_FIELD_NONE;
	format->num_planes = 2;
	format->colorspace = layout->colourimetry.colorspace;
	format->ycbcr_enc = layout->colourimetry.ycbcr_enc;
	format->quantization = layout->colourimetry.quantization;
	format->xfer_func = layout->colourimetry.xfer_func;
	for (i = 0; i < 2; i++) {
		format->plane_fmt[i].bytesperline = layout->pitch[i];
		format->plane_fmt[i].sizeimage = layout->size[i];
	}
}

static int s7d_placeholder(const struct s7d_v4l2_codec *codec,
			   u32 width, u32 height, u32 fourcc,
			   struct s7d_codec_layout *layout)
{
	struct s7d_h264_sps sps = {
		.profile_idc = 100, .level_idc = 51, .chroma_format_idc = 1,
		.bit_depth_luma = 8, .bit_depth_chroma = 8,
		.frame_mbs_only = true,
	};

	width = clamp_t(u32, width, 64, codec->max_width);
	height = clamp_t(u32, height, 64, codec->max_height);
	if (codec->engine == S7D_CODEC_ENGINE_HEVC) {
		struct s7d_codec_colour colour = { };
		struct s7d_codec_layout next = {
			.format = S7D_CODEC_CAPTURE_NV12M, .plane_count = 2,
			.coded_width = ALIGN(width, 2), .coded_height = ALIGN(height, 2),
			.visible_width = ALIGN(width, 2), .visible_height = ALIGN(height, 2),
			.storage_height = ALIGN(height, S7D_CODEC_STORAGE_ALIGNMENT),
			.dpb_limit = 1, .dpb_slots = 1, .current_slots = 2,
			.min_capture_buffers = 3,
		};
		int ret;

		next.pitch[0] = ALIGN(width, S7D_CODEC_STORAGE_ALIGNMENT);
		next.pitch[1] = next.pitch[0];
		next.size[1] = ALIGN(next.pitch[0] * next.storage_height / 2, PAGE_SIZE);
		next.size[0] = next.size[1] * 2;
		next.total_size = next.size[0] + next.size[1];
		ret = s7d_codec_resolve_colour(&colour, next.coded_width, next.coded_height,
					     &next.colourimetry);
		if (!ret)
			ret = s7d_codec_validate_layout(&next);
		if (!ret)
			*layout = next;
		return ret;
	}
	sps.mb_width = DIV_ROUND_UP(width, 16);
	sps.mb_height = DIV_ROUND_UP(height, 16);
	return s7d_h264_build_layout(&sps, fourcc == V4L2_PIX_FMT_NV21M ?
		S7D_CODEC_CAPTURE_NV21M : S7D_CODEC_CAPTURE_NV12M, 0, layout);
}

bool s7d_v4l2_codec_valid(const struct s7d_v4l2_codec *codec)
{
	if (!codec || codec->max_width < 64 || codec->max_height < 64 ||
	    codec->max_width > 4096 || codec->max_height > 2304 ||
	    codec->max_input_size < S7D_OUTPUT_MIN_SIZE ||
	    codec->max_input_size > S7D_OUTPUT_MAX_SIZE ||
	    !IS_ALIGNED(codec->max_input_size, PAGE_SIZE))
		return false;
	if (codec->engine == S7D_CODEC_ENGINE_VDEC)
		return codec->output_format == V4L2_PIX_FMT_H264;
	return codec->engine == S7D_CODEC_ENGINE_HEVC &&
		codec->output_format == V4L2_PIX_FMT_HEVC && !codec->nv21 &&
		codec->max_width <= 1920 && codec->max_height <= 1080 &&
		codec->max_input_size <= SZ_2M;
}

int s7d_v4l2_enum_format(struct s7d_v4l2_context *ctx, struct v4l2_fmtdesc *format)
{
	if (s7d_output(format->type)) {
		if (format->index)
			return -EINVAL;
		format->pixelformat = ctx->codec.output_format;
		format->flags = V4L2_FMT_FLAG_COMPRESSED;
		if (ctx->codec.output_format == V4L2_PIX_FMT_H264 &&
		    ctx->ops.reconfigure_stop && ctx->ops.reconfigure_restore)
			format->flags |= V4L2_FMT_FLAG_DYN_RESOLUTION;
		return 0;
	}
	if (!s7d_capture(format->type) || format->index > ctx->codec.nv21)
		return -EINVAL;
	format->pixelformat = format->index ? V4L2_PIX_FMT_NV21M : V4L2_PIX_FMT_NV12M;
	format->flags = 0;
	return 0;
}

int s7d_v4l2_try_format(struct s7d_v4l2_context *ctx, struct v4l2_format *format)
{
	struct v4l2_pix_format_mplane *pix = &format->fmt.pix_mp;
	struct s7d_codec_layout layout;
	unsigned long flags;
	bool valid;
	u32 fourcc;
	int ret;

	if (s7d_output(format->type)) {
		u32 width = clamp_t(u32, pix->width, 64, ctx->codec.max_width);
		u32 height = clamp_t(u32, pix->height, 64, ctx->codec.max_height);
		u32 bytes = clamp_t(u32, pix->plane_fmt[0].sizeimage,
				    S7D_OUTPUT_MIN_SIZE, ctx->codec.max_input_size);

		memset(pix, 0, sizeof(*pix));
		pix->width = width;
		pix->height = height;
		pix->pixelformat = ctx->codec.output_format;
		pix->field = V4L2_FIELD_NONE;
		pix->num_planes = 1;
		pix->plane_fmt[0].sizeimage = ALIGN(bytes, PAGE_SIZE);
		return 0;
	}
	if (!s7d_capture(format->type))
		return -EINVAL;
	fourcc = pix->pixelformat;
	if (!ctx->codec.nv21 || fourcc != V4L2_PIX_FMT_NV21M)
		fourcc = V4L2_PIX_FMT_NV12M;
	spin_lock_irqsave(&ctx->state_lock, flags);
	layout = s7d_query_layout(ctx);
	valid = ctx->sequence_valid;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (!valid) {
		ret = s7d_placeholder(&ctx->codec, pix->width, pix->height, fourcc, &layout);
		if (ret)
			return ret;
	}
	layout.format = fourcc == V4L2_PIX_FMT_NV21M ?
		S7D_CODEC_CAPTURE_NV21M : S7D_CODEC_CAPTURE_NV12M;
	s7d_capture_format(&layout, pix);
	return 0;
}

int s7d_v4l2_set_format(struct s7d_v4l2_context *ctx, struct v4l2_format *format)
{
	struct s7d_codec_layout placeholder;
	struct vb2_queue *queue;
	unsigned long flags;
	int ret;

	if (!s7d_output(format->type) && !s7d_capture(format->type))
		return -EINVAL;
	mutex_lock(&ctx->queue_lock);
	queue = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, format->type);
	if (vb2_is_busy(queue) || (s7d_output(format->type) &&
				  vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = s7d_v4l2_try_format(ctx, format);
	if (ret)
		goto unlock;
	if (s7d_output(format->type)) {
		ret = s7d_placeholder(&ctx->codec, format->fmt.pix_mp.width,
			format->fmt.pix_mp.height, V4L2_PIX_FMT_NV12M, &placeholder);
		if (ret)
			goto unlock;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (ctx->retired || (ctx->ops.clear_stream && ctx->state == S7D_V4L2_FAILED) ||
	    (s7d_output(format->type) && ctx->prepared) ||
	    (s7d_capture(format->type) && ctx->capture_streaming))
		ret = -EBUSY;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (ret)
		goto unlock;
	if (s7d_output(format->type)) {
		ret = s7d_clear_stream_locked(ctx);
		if (ret)
			goto unlock;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (s7d_output(format->type)) {
		ctx->output = format->fmt.pix_mp;
		ctx->layout = placeholder;
		s7d_capture_format(&placeholder, &ctx->capture);
		ctx->sequence_valid = false;
		ctx->capture_layout_retained = false;
	} else {
		ctx->capture = format->fmt.pix_mp;
		if (ctx->change_pending)
			ctx->query_layout.format = ctx->capture.pixelformat == V4L2_PIX_FMT_NV21M ?
				S7D_CODEC_CAPTURE_NV21M : S7D_CODEC_CAPTURE_NV12M;
		else
			ctx->layout.format = ctx->capture.pixelformat == V4L2_PIX_FMT_NV21M ?
				S7D_CODEC_CAPTURE_NV21M : S7D_CODEC_CAPTURE_NV12M;
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
unlock:
	mutex_unlock(&ctx->queue_lock);
	return ret;
}

int s7d_v4l2_get_format(struct s7d_v4l2_context *ctx, struct v4l2_format *format)
{
	unsigned long flags;

	if (!s7d_output(format->type) && !s7d_capture(format->type))
		return -EINVAL;
	spin_lock_irqsave(&ctx->state_lock, flags);
	format->fmt.pix_mp = s7d_output(format->type) ? ctx->output : ctx->capture;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	return 0;
}

__poll_t s7d_v4l2_poll(struct s7d_v4l2_context *ctx, struct file *file,
		     struct poll_table_struct *wait)
{
	struct vb2_queue *output = v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	struct vb2_queue *capture = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	unsigned long flags;
	__poll_t events;
	bool initial;

	mutex_lock(&ctx->queue_lock);
	events = v4l2_m2m_poll(file, ctx->fh.m2m_ctx, wait);
	if ((events & (EPOLLERR | EPOLLPRI)) == (EPOLLERR | EPOLLPRI)) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		initial = !ctx->retired && ctx->prepared && ctx->output_streaming &&
			ctx->sequence_valid && !ctx->capture_streaming &&
			ctx->state == S7D_V4L2_CAPTURE_SETUP &&
			vb2_is_streaming(output) && !vb2_is_streaming(capture) &&
			list_empty(&output->queued_list) &&
			!output->error && !capture->error;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		/* Empty queues must not hide the initial metadata notification. */
		if (initial)
			events &= ~EPOLLERR;
	}
	mutex_unlock(&ctx->queue_lock);
	return events;
}

int s7d_v4l2_get_selection(struct s7d_v4l2_context *ctx,
			   struct v4l2_selection *selection)
{
	struct s7d_codec_layout layout;
	unsigned long flags;

	if (selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    selection->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;
	spin_lock_irqsave(&ctx->state_lock, flags);
	layout = s7d_query_layout(ctx);
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	selection->r.left = 0;
	selection->r.top = 0;
	switch (selection->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		selection->r.width = layout.coded_width;
		selection->r.height = layout.coded_height;
		break;
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		selection->r.left = layout.visible_left;
		selection->r.top = layout.visible_top;
		selection->r.width = layout.visible_width;
		selection->r.height = layout.visible_height;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static bool s7d_can_submit_locked(struct s7d_v4l2_context *ctx,
				 struct vb2_v4l2_buffer *input)
{
	struct s7d_v4l2_buffer *buffer;

	if (!input || ctx->retired || !ctx->prepared || !ctx->output_streaming ||
	    ctx->active_input || ctx->capture_stopping || ctx->change_pending)
		return false;
	buffer = s7d_buffer(&input->vb2_buf);
	if (buffer->token <= ctx->last_consumed_token)
		return false;
	if (ctx->initial_pending && buffer->token != ctx->initial_token)
		return false;
	if (ctx->state == S7D_V4L2_INITIALIZING)
		return true;
	if (ctx->ops.clear_stream && ctx->capture_streaming &&
	    ctx->state == S7D_V4L2_DRAINING && !ctx->capture_ready)
		return buffer->token <= ctx->drain_token;
	if (!ctx->capture_streaming || !ctx->capture_ready)
		return false;
	return ctx->state == S7D_V4L2_DECODING ||
	       (ctx->state == S7D_V4L2_DRAINING && buffer->token <= ctx->drain_token);
}

static bool s7d_can_drain_locked(struct s7d_v4l2_context *ctx)
{
	return !ctx->retired && !ctx->capture_stopping && !ctx->change_pending && ctx->prepared &&
	       ctx->output_streaming && ctx->capture_streaming &&
	       (ctx->capture_ready || ctx->ops.clear_stream) && !ctx->initial_pending &&
	       ctx->state == S7D_V4L2_DRAINING && !ctx->drain_requested &&
	       !ctx->active_input && ctx->last_consumed_token == ctx->drain_token;
}

static void s7d_device_run(void *priv)
{
	struct s7d_v4l2_context *ctx = priv;
	unsigned long flags;

	spin_lock_irqsave(&ctx->state_lock, flags);
	if (!ctx->retired && !ctx->capture_stopping && ctx->prepared &&
	    ctx->output_streaming && ctx->capture_streaming && ctx->sequence_valid &&
	    ctx->rebind_pending && !ctx->change_pending && !ctx->initial_pending &&
	    !ctx->drain_requested &&
	    !ctx->last_pending && !ctx->last_completing && ctx->state != S7D_V4L2_STOPPED)
		mod_delayed_work(system_unbound_wq, &ctx->rebind_work, 0);
	if (!ctx->retired &&
	    s7d_can_submit_locked(ctx, v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx)))
		queue_work(system_unbound_wq, &ctx->run_work);
	if (s7d_can_drain_locked(ctx))
		queue_work(system_unbound_wq, &ctx->drain_work);
	spin_unlock_irqrestore(&ctx->state_lock, flags);
}

static void s7d_return_buffers(struct s7d_v4l2_context *ctx,
				     struct vb2_queue *q, enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *buffer;

	while ((buffer = q->is_output ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(buffer, state);
}

static struct s7d_v4l2_buffer *
s7d_claim_input_locked(struct s7d_v4l2_context *ctx, u64 token, bool consumed)
{
	struct s7d_v4l2_buffer *buffer = ctx->active_input;

	if (!buffer || ctx->retired || buffer->token != token ||
	    token <= ctx->last_consumed_token)
		return NULL;
	ctx->active_input = NULL;
	if (consumed)
		ctx->last_consumed_token = token;
	return buffer;
}

static int s7d_retire(struct s7d_v4l2_context *ctx)
{
	unsigned long flags;
	bool retired;
	int ret;

	mutex_lock(&ctx->retire_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	retired = ctx->retired;
	ret = ctx->stop_error;
	if (!retired) {
		if (ctx->active_input) {
			u64 token = ctx->active_input->token;

			if (s7d_claim_input_locked(ctx, token, false))
				ctx->cancelled_input_token = token;
		}
		if (ctx->state == S7D_V4L2_DRAINING && ctx->drain_requested &&
		    !ctx->active_input && !ctx->last_pending && !ctx->last_completing &&
		    ctx->last_consumed_token == ctx->drain_token)
			ctx->cancelled_drain_epoch = ctx->drain_epoch;
	}
	ctx->retired = true;
	ctx->state = S7D_V4L2_FAILED;
	ctx->output_streaming = false;
	ctx->capture_streaming = false;
	ctx->capture_ready = false;
	ctx->initial_pending = false;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (!retired) {
		mutex_lock(&ctx->backend_lock);
		ctx->ops.detach(ctx->backend);
		mutex_unlock(&ctx->backend_lock);
		ret = ctx->session ? s7d_codec_session_stop(ctx->session) : 0;
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->stop_error = ret;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
	}
	mutex_unlock(&ctx->retire_lock);
	return ret;
}

static int s7d_stop(struct s7d_v4l2_context *ctx)
{
	int ret = s7d_retire(ctx);

	cancel_work_sync(&ctx->run_work);
	cancel_work_sync(&ctx->drain_work);
	cancel_delayed_work_sync(&ctx->rebind_work);
	cancel_delayed_work_sync(&ctx->change_work);
	return ret;
}

static int s7d_reset(struct s7d_v4l2_context *ctx, bool capture_streaming)
{
	struct s7d_codec_session *session;
	struct s7d_v4l2_buffer *buffer;
	unsigned long flags;
	unsigned int i;
	bool released_epoch;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	mutex_lock(&ctx->retire_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	released_epoch = ctx->session || ctx->change_stopped;
	ret = ctx->stop_error;
	if (!ctx->retired || ret) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		goto unlock;
	}
	if (ctx->ops.clear_stream && released_epoch &&
	    (ctx->generation == U64_MAX || ctx->completion_epoch == U64_MAX)) {
		ret = ctx->stop_error = -EOVERFLOW;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		goto unlock;
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	mutex_lock(&ctx->backend_lock);
	ret = ctx->ops.reset(ctx->backend);
	mutex_unlock(&ctx->backend_lock);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->stop_error = ret;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		goto unlock;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	for (i = 0; i < S7D_CODEC_MAX_CAPTURE_BUFFERS; i++) {
		buffer = ctx->surface[i];
		if (buffer) {
			buffer->surface = NULL;
			buffer->generation = 0;
			ctx->surface[i] = NULL;
		}
	}
	session = ctx->session;
	ctx->session = NULL;
	ctx->staging = NULL;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (session)
		s7d_codec_session_put(session);
	v4l2_m2m_clear_state(ctx->fh.m2m_ctx);
	vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->prepared = false;
	if (ctx->change_pending) {
		ctx->layout.format = ctx->query_layout.format;
		s7d_capture_format(&ctx->layout, &ctx->capture);
		ctx->output.width = ctx->layout.coded_width;
		ctx->output.height = ctx->layout.coded_height;
	}
	memset(&ctx->change, 0, sizeof(ctx->change));
	memset(&ctx->query_layout, 0, sizeof(ctx->query_layout));
	ctx->change_pending = false;
	ctx->change_stopped = false;
	ctx->change_capture_off = false;
	ctx->change_last_done = false;
	ctx->change_last_dequeued = false;
	ctx->change_draining = false;
	if (!ctx->ops.clear_stream)
		ctx->sequence_valid = false;
	if (ctx->ops.clear_stream && released_epoch) {
		ctx->generation++;
		ctx->completion_epoch++;
	}
	ctx->last_dequeued_epoch = 0;
	ctx->cancelled_input_token = 0;
	ctx->cancelled_drain_epoch = 0;
	ctx->active_input = NULL;
	ctx->last_consumed_token = ctx->next_input_token;
	ctx->initial_token = 0;
	ctx->initial_pending = false;
	ctx->drain_token = 0;
	ctx->drain_requested = false;
	ctx->last_pending = false;
	ctx->last_completing = false;
	if (!capture_streaming)
		ctx->capture_sequence = 0;
	ctx->capture_stopping = false;
	ctx->capture_only_stop = false;
	ctx->capture_streaming = capture_streaming;
	ctx->capture_ready = false;
	ctx->rebind_pending = false;
	ctx->state = S7D_V4L2_IDLE;
	ctx->retired = false;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
unlock:
	mutex_unlock(&ctx->retire_lock);
	return ret;
}

/* Caller holds the native queue lock; generic buffer release must succeed first. */
static int s7d_clear_stream_locked(struct s7d_v4l2_context *ctx)
{
	unsigned long flags;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	if (!ctx->ops.clear_stream)
		return 0;
	spin_lock_irqsave(&ctx->state_lock, flags);
	ret = ctx->retired || ctx->state == S7D_V4L2_FAILED || ctx->stop_error ?
		-ESHUTDOWN : ctx->session || ctx->prepared ? -EBUSY : 0;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (ret)
		return ret;
	mutex_lock(&ctx->backend_lock);
	ret = ctx->ops.clear_stream(ctx->backend);
	mutex_unlock(&ctx->backend_lock);
	if (ret)
		return ret < 0 ? ret : -EIO;
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->sequence_valid = false;
	ctx->capture_layout_retained =
		vb2_get_num_buffers(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)) != 0;
	ctx->initial_pending = false;
	ctx->initial_token = 0;
	ctx->last_consumed_token = ctx->next_input_token;
	ctx->drain_token = 0;
	ctx->drain_requested = false;
	ctx->last_pending = false;
	ctx->last_completing = false;
	ctx->last_dequeued_epoch = 0;
	ctx->cancelled_input_token = 0;
	ctx->cancelled_drain_epoch = 0;
	ctx->rebind_pending = false;
	ctx->state = S7D_V4L2_IDLE;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	v4l2_m2m_clear_state(ctx->fh.m2m_ctx);
	vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	return 0;
}

int s7d_v4l2_reqbufs(struct s7d_v4l2_context *ctx, struct file *file,
		     struct v4l2_requestbuffers *request)
{
	unsigned long flags;
	bool clear;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	if (!s7d_output(request->type) && !s7d_capture(request->type))
		return -EINVAL;
	clear = s7d_output(request->type) && !request->count;
	ret = v4l2_m2m_reqbufs(file, ctx->fh.m2m_ctx, request);
	if (ret)
		return ret;
	if (clear)
		return s7d_clear_stream_locked(ctx);
	if (s7d_capture(request->type) && !request->count) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->capture_layout_retained = false;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
	}
	return 0;
}

int s7d_v4l2_dqbuf(struct s7d_v4l2_context *ctx, struct file *file,
		   struct v4l2_buffer *result)
{
	struct vb2_queue *q;
	struct vb2_buffer *vb;
	unsigned long flags;
	bool stale = false;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	if (!s7d_output(result->type) && !s7d_capture(result->type))
		return -EINVAL;
	ret = v4l2_m2m_dqbuf(file, ctx->fh.m2m_ctx, result);
	if (ret || !s7d_capture(result->type) || !(result->flags & V4L2_BUF_FLAG_LAST))
		return ret;
	q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	vb = vb2_get_buffer(q, result->index);
	if (!vb)
		return -EIO;
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (ctx->change_pending) {
		if (ctx->change_stopped && ctx->change_last_done &&
		    s7d_buffer(vb)->completion_epoch == ctx->completion_epoch &&
		    s7d_buffer(vb)->change_last_epoch == ctx->change.epoch) {
			ctx->change_last_dequeued = true;
			ctx->last_dequeued_epoch = ctx->completion_epoch;
		} else if (!ctx->change_last_dequeued) {
			stale = true;
		}
	} else if (s7d_buffer(vb)->completion_epoch == ctx->completion_epoch) {
		ctx->last_dequeued_epoch = ctx->completion_epoch;
	} else if (ctx->last_dequeued_epoch != ctx->completion_epoch) {
		stale = true;
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (stale)
		vb2_clear_last_buffer_dequeued(q);
	return 0;
}

static bool s7d_queues_idle(struct s7d_v4l2_context *ctx)
{
	struct vb2_queue *q[2] = {
		v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx),
		v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx),
	};
	struct vb2_buffer *vb;
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(q); i++) {
		if (vb2_is_streaming(q[i]) || q[i]->error)
			return false;
		for (j = 0; j < q[i]->max_num_buffers; j++) {
			vb = vb2_get_buffer(q[i], j);
			if (vb && (vb->prepared || vb->state != VB2_BUF_STATE_DEQUEUED))
				return false;
		}
	}
	return true;
}

int s7d_v4l2_streamon(struct s7d_v4l2_context *ctx, struct file *file,
		    enum v4l2_buf_type type)
{
	struct vb2_queue *q;
	unsigned long flags;
	unsigned int minimum;
	bool valid, retired;

	if (!s7d_output(type) && !s7d_capture(type))
		return -EINVAL;
	spin_lock_irqsave(&ctx->state_lock, flags);
	valid = ctx->sequence_valid;
	retired = ctx->retired;
	minimum = s7d_query_layout(ctx).min_capture_buffers;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (retired)
		return -ESHUTDOWN;
	q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, type);
	if (s7d_capture(type) && !vb2_is_streaming(q)) {
		if (!valid)
			return -EAGAIN;
		q->min_queued_buffers = minimum;
		q->min_reqbufs_allocation = minimum;
	}
	return v4l2_m2m_streamon(file, ctx->fh.m2m_ctx, type);
}

static int s7d_capture_release(struct s7d_v4l2_context *ctx)
{
	struct s7d_v4l2_buffer *buffer;
	unsigned long flags;
	unsigned int i, pinned = 0;
	u64 generation;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (ctx->retired || !ctx->prepared || !ctx->output_streaming ||
	    !ctx->session || !ctx->sequence_valid || !ctx->generation ||
	    ctx->initial_pending || ctx->drain_requested || ctx->last_pending ||
	    ctx->last_completing || (ctx->state != S7D_V4L2_CAPTURE_SETUP &&
			     ctx->state != S7D_V4L2_DECODING)) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return -EOPNOTSUPP;
	}
	generation = ctx->generation;
	for (i = 0; i < S7D_CODEC_MAX_CAPTURE_BUFFERS; i++) {
		buffer = ctx->surface[i];
		if (!buffer)
			continue;
		if (!buffer->surface || buffer->generation != generation) {
			spin_unlock_irqrestore(&ctx->state_lock, flags);
			return -ESTALE;
		}
		pinned++;
	}
	if (pinned && generation == U64_MAX) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return -EOVERFLOW;
	}
	ctx->capture_stopping = true;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	cancel_work_sync(&ctx->run_work);
	cancel_work_sync(&ctx->drain_work);
	cancel_delayed_work_sync(&ctx->rebind_work);
	mutex_lock(&ctx->backend_lock);
	ret = ctx->ops.capture_stop(ctx->backend, generation);
	if (ret)
		goto out;
	for (i = 0; i < S7D_CODEC_MAX_CAPTURE_BUFFERS; i++) {
		buffer = ctx->surface[i];
		if (!buffer || buffer->m2m.vb.vb2_buf.state != VB2_BUF_STATE_ACTIVE)
			continue;
		ret = s7d_codec_surface_return_queued(buffer->surface);
		if (ret)
			goto out;
	}
	if (pinned) {
		ret = s7d_codec_session_retire_generation(ctx->session, generation);
		if (ret)
			goto out;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	for (i = 0; i < S7D_CODEC_MAX_CAPTURE_BUFFERS; i++) {
		buffer = ctx->surface[i];
		if (!buffer)
			continue;
		buffer->surface = NULL;
		buffer->generation = 0;
		ctx->surface[i] = NULL;
	}
	if (pinned)
		ctx->generation++;
	ctx->capture_ready = false;
	ctx->capture_streaming = false;
	ctx->capture_stopping = false;
	ctx->state = S7D_V4L2_CAPTURE_SETUP;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
out:
	mutex_unlock(&ctx->backend_lock);
	return ret;
}

int s7d_v4l2_streamoff(struct s7d_v4l2_context *ctx, struct file *file,
		     enum v4l2_buf_type type)
{
	unsigned long flags;
	int ret;

	if (!s7d_output(type) && !s7d_capture(type))
		return -EINVAL;
	if (s7d_capture(type) && ctx->change_pending && !ctx->retired) {
		ret = s7d_reconfigure_stop(ctx);
		if (ret) {
			s7d_stop(ctx);
			vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
			vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
			return ret;
		}
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->capture_streaming = false;
		ctx->capture_ready = false;
		ctx->capture_only_stop = true;
		ctx->change_capture_off = true;
		ctx->last_pending = false;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
	} else if (s7d_capture(type) && ctx->ops.clear_stream && !ctx->session &&
		   !ctx->retired) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->capture_streaming = false;
		ctx->capture_ready = false;
		ctx->capture_sequence = 0;
		ctx->capture_only_stop = true;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
	} else if (s7d_capture(type) && ctx->session && ctx->sequence_valid && !ctx->retired) {
		ret = s7d_capture_release(ctx);
		if (!ret)
			ctx->capture_only_stop = true;
		else
			s7d_stop(ctx);
	}
	ret = v4l2_m2m_streamoff(file, ctx->fh.m2m_ctx, type);
	ctx->capture_only_stop = false;
	if (ret)
		return ret;
	if (ctx->change_pending && !ctx->retired)
		return 0;
	if (!s7d_queues_idle(ctx)) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		ret = ctx->stop_error;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return ret;
	}
	ret = s7d_reset(ctx, false);
	if (ret) {
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	}
	return ret;
}

static void s7d_frame_ready(void *priv, const struct s7d_codec_frame *frame)
{
	struct s7d_v4l2_context *ctx = priv;
	struct s7d_v4l2_buffer *buffer;
	struct vb2_v4l2_buffer *vbuf;
	unsigned long flags;
	unsigned int i;

	if (frame->surface_id >= S7D_CODEC_MAX_CAPTURE_BUFFERS)
		return;
	spin_lock_irqsave(&ctx->state_lock, flags);
	buffer = ctx->surface[frame->surface_id];
	if (!buffer || buffer->generation != frame->generation ||
	    ctx->retired || !ctx->capture_streaming) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return;
	}
	vbuf = &buffer->m2m.vb;
	vbuf->vb2_buf.timestamp = frame->timestamp_ns;
	vbuf->vb2_buf.copied_timestamp = 1;
	vbuf->flags &= ~(V4L2_BUF_FLAG_TSTAMP_SRC_MASK | V4L2_BUF_FLAG_TIMECODE);
	vbuf->flags |= frame->timestamp_meta.flags &
		       (V4L2_BUF_FLAG_TSTAMP_SRC_MASK | V4L2_BUF_FLAG_TIMECODE);
	if (vbuf->flags & V4L2_BUF_FLAG_TIMECODE)
		vbuf->timecode = frame->timestamp_meta.timecode;
	vbuf->sequence = ctx->capture_sequence++;
	buffer->completion_epoch = ctx->completion_epoch;
	buffer->change_last_epoch = 0;
	vbuf->field = V4L2_FIELD_NONE;
	vbuf->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
			 V4L2_BUF_FLAG_BFRAME | V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_LAST);
	if (frame->flags & S7D_CODEC_FRAME_KEY)
		vbuf->flags |= V4L2_BUF_FLAG_KEYFRAME;
	if (frame->flags & S7D_CODEC_FRAME_P)
		vbuf->flags |= V4L2_BUF_FLAG_PFRAME;
	if (frame->flags & S7D_CODEC_FRAME_B)
		vbuf->flags |= V4L2_BUF_FLAG_BFRAME;
	if (frame->flags & S7D_CODEC_FRAME_ERROR)
		vbuf->flags |= V4L2_BUF_FLAG_ERROR;
	for (i = 0; i < 2; i++)
		vb2_set_plane_payload(&vbuf->vb2_buf, i, frame->bytesused[i]);
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	v4l2_m2m_dst_buf_remove_by_buf(ctx->fh.m2m_ctx, vbuf);
	v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_DONE);
}

static int s7d_reconfigure_stop(struct s7d_v4l2_context *ctx)
{
	struct s7d_codec_session *session;
	struct s7d_v4l2_reconfigure change;
	struct s7d_v4l2_buffer *buffer;
	unsigned long flags;
	unsigned int i;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	mutex_lock(&ctx->retire_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	ret = ctx->retired ? -ESHUTDOWN : !ctx->change_pending ? -ESTALE : 0;
	if (!ret && ctx->change_stopped) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		mutex_unlock(&ctx->retire_lock);
		return 0;
	}
	change = ctx->change;
	if (!ret && (!ctx->active_input || ctx->active_input->token != change.input_token ||
		     !ctx->session || ctx->generation != change.generation ||
		     ctx->generation == U64_MAX || ctx->completion_epoch == U64_MAX))
		ret = -ESTALE;
	if (!ret)
		ctx->capture_stopping = true;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (ret)
		goto unlock;
	cancel_work_sync(&ctx->run_work);
	cancel_work_sync(&ctx->drain_work);
	cancel_delayed_work_sync(&ctx->rebind_work);
	mutex_lock(&ctx->backend_lock);
	ret = ctx->ops.reconfigure_stop(ctx->backend, &change);
	mutex_unlock(&ctx->backend_lock);
	if (ret)
		goto unlock;
	spin_lock_irqsave(&ctx->state_lock, flags);
	for (i = 0; i < S7D_CODEC_MAX_CAPTURE_BUFFERS; i++) {
		buffer = ctx->surface[i];
		if (buffer) {
			buffer->surface = NULL;
			buffer->generation = 0;
			ctx->surface[i] = NULL;
		}
	}
	session = ctx->session;
	ctx->session = NULL;
	ctx->staging = NULL;
	ctx->prepared = false;
	ctx->capture_ready = false;
	ctx->capture_stopping = false;
	ctx->rebind_pending = false;
	ctx->change_stopped = true;
	ctx->last_pending = ctx->capture_streaming;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	s7d_codec_session_put(session);
unlock:
	mutex_unlock(&ctx->retire_lock);
	if (!ret) {
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
		s7d_complete_last(ctx);
	}
	return ret;
}

int s7d_v4l2_reconfigure_result(struct s7d_v4l2_context *ctx,
				const struct s7d_codec_layout *sequence,
				const struct s7d_v4l2_reconfigure *change,
				bool *cancelled)
{
	struct v4l2_event event = { .type = V4L2_EVENT_SOURCE_CHANGE };
	struct s7d_codec_layout layout;
	unsigned long flags;
	int ret;

	if (cancelled)
		*cancelled = false;
	if (!sequence || !change || !change->epoch || !change->input_token ||
	    !change->generation || !ctx->ops.reconfigure_stop ||
	    !ctx->ops.reconfigure_restore)
		return -EINVAL;
	ret = s7d_codec_validate_layout(sequence);
	if (ret)
		return ret;
	layout = *sequence;
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (ctx->retired || !ctx->output_streaming) {
		if (cancelled)
			*cancelled = true;
		ret = -ESHUTDOWN;
	} else if (ctx->change_pending) {
		layout.format = ctx->query_layout.format;
		ret = change->epoch == ctx->change.epoch &&
		      change->input_token == ctx->change.input_token &&
		      change->generation == ctx->change.generation &&
		      !memcmp(&layout, &ctx->query_layout, sizeof(layout)) ? 0 : -EBUSY;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return ret;
	} else if (!ctx->sequence_valid || !ctx->prepared || !ctx->capture_streaming ||
		   !ctx->capture_ready || ctx->initial_pending || ctx->last_pending ||
		   ctx->last_completing || ctx->drain_requested || !ctx->active_input ||
		   ctx->active_input->token != change->input_token ||
		   ctx->generation != change->generation ||
		   (ctx->state != S7D_V4L2_DECODING && ctx->state != S7D_V4L2_DRAINING)) {
		ret = -ESTALE;
	} else if (ctx->generation == U64_MAX || ctx->completion_epoch == U64_MAX) {
		ret = -EOVERFLOW;
	} else {
		layout.format = ctx->layout.format;
		ctx->query_layout = layout;
		ctx->change = *change;
		ctx->change_pending = true;
		ctx->change_stopped = false;
		ctx->change_capture_off = false;
		ctx->change_last_done = false;
		ctx->change_last_dequeued = false;
		ctx->last_dequeued_epoch = 0;
		ctx->change_draining = ctx->state == S7D_V4L2_DRAINING;
		ctx->capture_stopping = true;
		ctx->rebind_pending = false;
		s7d_capture_format(&layout, &ctx->capture);
		ctx->output.width = layout.coded_width;
		ctx->output.height = layout.coded_height;
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (!ret) {
		event.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
		v4l2_event_queue_fh(&ctx->fh, &event);
		mod_delayed_work(system_unbound_wq, &ctx->change_work, 0);
	}
	return ret;
}

int s7d_v4l2_sequence_result(struct s7d_v4l2_context *ctx,
			     const struct s7d_codec_layout *sequence, bool *cancelled)
{
	struct v4l2_event event = { .type = V4L2_EVENT_SOURCE_CHANGE };
	struct s7d_codec_layout layout;
	unsigned long flags;
	int ret;

	if (cancelled)
		*cancelled = false;
	ret = s7d_codec_validate_layout(sequence);
	if (ret)
		return ret;
	layout = *sequence;
	spin_lock_irqsave(&ctx->state_lock, flags);
	layout.format = ctx->layout.format;
	if (!cancelled && (ctx->retired || !ctx->output_streaming)) {
		ret = -ESHUTDOWN;
	} else if ((ctx->sequence_valid || ctx->capture_layout_retained) &&
	    memcmp(&ctx->layout, &layout, sizeof(layout))) {
		ret = -EOPNOTSUPP;
	} else if (!ctx->sequence_valid && ctx->generation == U64_MAX) {
		ret = -EOVERFLOW;
	} else if (ctx->retired || !ctx->output_streaming) {
		if (cancelled)
			*cancelled = true;
		ret = -ESHUTDOWN;
	} else if (ctx->sequence_valid) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return 0;
	} else {
		ctx->layout = layout;
		s7d_capture_format(&layout, &ctx->capture);
		ctx->output.width = layout.coded_width;
		ctx->output.height = layout.coded_height;
		ctx->sequence_valid = true;
		ctx->generation++;
		if (ctx->state != S7D_V4L2_DRAINING)
			ctx->state = S7D_V4L2_CAPTURE_SETUP;
		if (ctx->ops.clear_stream && ctx->capture_streaming) {
			ctx->rebind_pending = true;
			mod_delayed_work(system_unbound_wq, &ctx->rebind_work, 0);
		}
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (!ret) {
		event.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
		v4l2_event_queue_fh(&ctx->fh, &event);
	}
	return ret;
}

int s7d_v4l2_sequence(struct s7d_v4l2_context *ctx,
		      const struct s7d_codec_layout *sequence)
{
	return s7d_v4l2_sequence_result(ctx, sequence, NULL);
}

static int s7d_capture_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s7d_v4l2_context *ctx = container_of(ctrl->handler,
					struct s7d_v4l2_context, controls);
	unsigned long flags;

	spin_lock_irqsave(&ctx->state_lock, flags);
	ctrl->val = ctx->sequence_valid ? s7d_query_layout(ctx).min_capture_buffers : 1;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	return 0;
}

static const struct v4l2_ctrl_ops s7d_ctrl_ops = {
	.g_volatile_ctrl = s7d_capture_ctrl,
};

int s7d_v4l2_subscribe_event(struct s7d_v4l2_context *ctx,
			     const struct v4l2_event_subscription *subscription)
{
	switch (subscription->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(&ctx->fh, subscription);
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(&ctx->fh, subscription, 1, NULL);
	default:
		return v4l2_ctrl_subscribe_event(&ctx->fh, subscription);
	}
}

static int s7d_queue_setup(struct vb2_queue *q, unsigned int *buffers,
			   unsigned int *planes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct s7d_v4l2_context *ctx = vb2_get_drv_priv(q);
	struct v4l2_pix_format_mplane format;
	unsigned long flags;
	unsigned int i, minimum;
	bool valid, retired;

	spin_lock_irqsave(&ctx->state_lock, flags);
	format = q->is_output ? ctx->output : ctx->capture;
	valid = ctx->sequence_valid;
	retired = ctx->retired;
	minimum = q->is_output ? 1 : s7d_query_layout(ctx).min_capture_buffers;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (retired)
		return -ESHUTDOWN;
	if (!q->is_output && !valid)
		return -EAGAIN;
	if (*planes && *planes != format.num_planes)
		return -EINVAL;
	if (*planes) {
		for (i = 0; i < *planes; i++)
			if (sizes[i] < format.plane_fmt[i].sizeimage)
				return -EINVAL;
	} else {
		*planes = format.num_planes;
		for (i = 0; i < *planes; i++)
			sizes[i] = format.plane_fmt[i].sizeimage;
	}
	if (!q->is_output) {
		unsigned int existing = vb2_get_num_buffers(q);

		if (existing >= S7D_CODEC_MAX_CAPTURE_BUFFERS)
			return -ENOSPC;
		if (!existing)
			*buffers = max(*buffers, minimum);
		*buffers = min(*buffers, S7D_CODEC_MAX_CAPTURE_BUFFERS - existing);
		q->min_queued_buffers = minimum;
		q->min_reqbufs_allocation = minimum;
	}
	for (i = 0; i < *planes; i++)
		alloc_devs[i] = ctx->dev;
	return 0;
}

/* Pinning the descriptor does not transfer user ownership or change payload. */
static int s7d_pin_capture(struct s7d_v4l2_context *ctx, struct vb2_buffer *vb)
{
	struct s7d_v4l2_buffer *buffer = s7d_buffer(vb);
	struct s7d_codec_surface_request request = {
		.surface_id = vb->index,
		.num_planes = 2,
		.dma_alignment = S7D_CODEC_STORAGE_ALIGNMENT,
	};
	unsigned long flags;
	unsigned int i;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	request.generation = ctx->generation;
	if (!ctx->session || !request.generation || !ctx->sequence_valid)
		return -EAGAIN;
	if (vb->index >= S7D_CODEC_MAX_CAPTURE_BUFFERS)
		return -ENOSPC;
	if (buffer->surface)
		return buffer->generation == request.generation ? 0 : -EOPNOTSUPP;
	if (vb->num_planes != 2)
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		request.min_size[i] = ctx->layout.size[i];
		if (vb2_plane_size(vb, i) < request.min_size[i])
			return -EINVAL;
	}
	ret = s7d_codec_vb2_pin(ctx->session, vb, &request, &buffer->surface);
	if (ret)
		return ret;
	buffer->generation = request.generation;
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->surface[vb->index] = buffer;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	return 0;
}

static int s7d_buf_prepare(struct vb2_buffer *vb)
{
	struct s7d_v4l2_context *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane format;
	unsigned long flags;
	unsigned int i;
	bool valid, detached;
	int ret;

	spin_lock_irqsave(&ctx->state_lock, flags);
	format = vb->vb2_queue->is_output ? ctx->output : ctx->capture;
	if (!vb->vb2_queue->is_output && ctx->change_pending && !ctx->change_capture_off)
		s7d_capture_format(&ctx->layout, &format);
	ret = ctx->retired ? -ESHUTDOWN : 0;
	valid = ctx->sequence_valid;
	detached = ctx->ops.clear_stream && !ctx->capture_ready;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (ret)
		return ret;
	if (vb->num_planes != format.num_planes)
		return -EINVAL;
	for (i = 0; i < vb->num_planes; i++)
		if (vb2_plane_size(vb, i) < format.plane_fmt[i].sizeimage)
			return -EINVAL;
	if (vb->vb2_queue->is_output) {
		u32 payload = vb2_get_plane_payload(vb, 0);

		if (payload <= vb->planes[0].data_offset || payload > vb2_plane_size(vb, 0))
			return -EINVAL;
		return 0;
	}
	if ((!ctx->session || !ctx->generation || !valid) && !detached)
		return -EAGAIN;
	if (vb->index >= S7D_CODEC_MAX_CAPTURE_BUFFERS)
		return -ENOSPC;
	for (i = 0; i < 2; i++)
		vb2_set_plane_payload(vb, i, 0);
	/* Users may requeue CAPTURE while its new OUTPUT epoch is absent. */
	if (!ctx->session || !ctx->generation || !valid)
		return 0;
	return s7d_pin_capture(ctx, vb);
}

/* Every allocated slot gets a descriptor; only ACTIVE slots grant DMA ownership. */
static int s7d_capture_rebind(struct s7d_v4l2_context *ctx)
{
	struct s7d_v4l2_capture_buffer pool[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct vb2_queue *q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct s7d_codec_layout layout;
	struct s7d_v4l2_buffer *buffer;
	struct vb2_buffer *vb;
	unsigned long flags;
	unsigned int i, count = 0;
	u64 generation;
	bool ready, fresh;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	mutex_lock(&ctx->backend_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	ready = ctx->rebind_pending && !ctx->retired && !ctx->capture_stopping &&
		!ctx->change_pending &&
		ctx->prepared && ctx->output_streaming && ctx->capture_streaming &&
		ctx->sequence_valid && !ctx->initial_pending && ctx->session &&
		!ctx->drain_requested && !ctx->last_pending && !ctx->last_completing &&
		ctx->state != S7D_V4L2_STOPPED;
	layout = ctx->layout;
	generation = ctx->generation;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	ret = 0;
	if (!ready)
		goto unlock;
	for (i = 0; i < q->max_num_buffers; i++) {
		vb = vb2_get_buffer(q, i);
		if (!vb)
			continue;
		if (count == ARRAY_SIZE(pool)) {
			ret = -ENOSPC;
			goto unlock;
		}
		buffer = s7d_buffer(vb);
		fresh = !buffer->surface;
		ret = s7d_pin_capture(ctx, vb);
		if (ret)
			goto unlock;
		if (fresh && vb->state == VB2_BUF_STATE_ACTIVE) {
			ret = s7d_codec_surface_queue(buffer->surface);
			if (ret)
				goto unlock;
		}
		pool[count++] = (struct s7d_v4l2_capture_buffer) {
			.surface = buffer->surface,
			.surface_id = i,
			.generation = generation,
		};
	}
	if (count < layout.min_capture_buffers) {
		ret = -ENOBUFS;
		goto unlock;
	}
	ret = ctx->ops.capture_start(ctx->backend, &layout, generation, pool, count);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto unlock;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (ctx->retired || ctx->state == S7D_V4L2_FAILED || !ctx->prepared ||
	    !ctx->output_streaming || !ctx->capture_streaming || ctx->capture_stopping) {
		ret = -ESHUTDOWN;
	} else {
		ctx->rebind_pending = false;
		ctx->capture_ready = true;
		if (ctx->state != S7D_V4L2_DRAINING)
			ctx->state = S7D_V4L2_DECODING;
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
unlock:
	mutex_unlock(&ctx->backend_lock);
	return ret;
}

static int s7d_reconfigure_start(struct s7d_v4l2_context *ctx)
{
	struct s7d_codec_callbacks callbacks = { .frame_ready = s7d_frame_ready };
	struct vb2_queue *q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct s7d_v4l2_reconfigure change;
	struct s7d_codec_layout layout;
	struct vb2_buffer *vb;
	unsigned long flags;
	u64 generation;
	size_t capacity;
	unsigned int i, count = 0;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	ret = ctx->retired ? -ESHUTDOWN : !ctx->change_pending || !ctx->change_stopped ||
		!ctx->output_streaming || ctx->prepared || ctx->session ||
		!ctx->active_input || ctx->active_input->token != ctx->change.input_token ?
		-ESTALE : 0;
	if (!ret && (ctx->generation == U64_MAX || ctx->completion_epoch == U64_MAX))
		ret = -EOVERFLOW;
	change = ctx->change;
	layout = ctx->query_layout;
	generation = ctx->generation + 1;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (ret)
		return ret;
	for (i = 0; i < q->max_num_buffers; i++) {
		vb = vb2_get_buffer(q, i);
		if (!vb)
			continue;
		if (vb->num_planes != 2 || vb2_plane_size(vb, 0) < layout.size[0] ||
		    vb2_plane_size(vb, 1) < layout.size[1])
			return -ENOBUFS;
		count++;
	}
	if (count < layout.min_capture_buffers)
		return -ENOBUFS;
	if (check_add_overflow((size_t)ctx->output.plane_fmt[0].sizeimage,
			      (size_t)(S7D_H264_ASSEMBLY_MAX_BYTES + SZ_16K +
				       PAGE_SIZE + L1_CACHE_BYTES), &capacity))
		return -EOVERFLOW;
	ret = s7d_codec_session_open(ctx->core, ctx->codec.engine, &callbacks, ctx, &ctx->session);
	if (ret)
		return ret;
	ctx->staging = s7d_codec_dma_alloc(ctx->session, capacity);
	if (IS_ERR(ctx->staging)) {
		ret = PTR_ERR(ctx->staging);
		ctx->staging = NULL;
		goto failed;
	}
	mutex_lock(&ctx->backend_lock);
	ret = ctx->ops.reconfigure_restore(ctx->backend, ctx->session, ctx, ctx->staging,
					   &change, generation);
	mutex_unlock(&ctx->backend_lock);
	if (ret)
		goto failed;
	v4l2_m2m_clear_state(ctx->fh.m2m_ctx);
	vb2_clear_last_buffer_dequeued(q);
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->layout = layout;
	ctx->generation = generation;
	ctx->completion_epoch++;
	ctx->last_dequeued_epoch = 0;
	ctx->last_pending = false;
	ctx->last_completing = false;
	ctx->initial_pending = false;
	ctx->prepared = true;
	ctx->capture_streaming = true;
	ctx->capture_stopping = false;
	ctx->rebind_pending = true;
	ctx->change_pending = false;
	ctx->change_stopped = false;
	ctx->state = ctx->change_draining ? S7D_V4L2_DRAINING : S7D_V4L2_CAPTURE_SETUP;
	ctx->change_draining = false;
	ctx->change_capture_off = false;
	ctx->change_last_done = false;
	ctx->change_last_dequeued = false;
	memset(&ctx->change, 0, sizeof(ctx->change));
	memset(&ctx->query_layout, 0, sizeof(ctx->query_layout));
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	ret = s7d_capture_rebind(ctx);
	if (!ret)
		return 0;
failed:
	s7d_retire(ctx);
	vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
	vb2_queue_error(q);
	return ret < 0 ? ret : -EIO;
}

static void s7d_change_work(struct work_struct *work)
{
	struct s7d_v4l2_context *ctx = container_of(to_delayed_work(work),
					struct s7d_v4l2_context, change_work);
	unsigned long flags;
	bool pending;
	int ret;

	if (!mutex_trylock(&ctx->queue_lock)) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		pending = ctx->change_pending && !ctx->change_stopped && !ctx->retired &&
			ctx->output_streaming;
		if (pending)
			mod_delayed_work(system_unbound_wq, &ctx->change_work, 1);
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	pending = ctx->change_pending && !ctx->change_stopped && !ctx->retired &&
		ctx->output_streaming;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	ret = pending ? s7d_reconfigure_stop(ctx) : 0;
	if (ret) {
		s7d_retire(ctx);
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	}
	mutex_unlock(&ctx->queue_lock);
}

static void s7d_rebind_work(struct work_struct *work)
{
	struct s7d_v4l2_context *ctx = container_of(to_delayed_work(work),
					struct s7d_v4l2_context, rebind_work);
	unsigned long flags;
	bool pending;
	int ret;

	/* STREAMOFF joins us with queue_lock held, so never wait on that lock. */
	if (!mutex_trylock(&ctx->queue_lock)) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		pending = ctx->rebind_pending && !ctx->retired && !ctx->capture_stopping &&
			!ctx->change_pending &&
			ctx->prepared && ctx->output_streaming && ctx->capture_streaming &&
			ctx->sequence_valid && !ctx->initial_pending && !ctx->drain_requested &&
			!ctx->last_pending && !ctx->last_completing &&
			ctx->state != S7D_V4L2_STOPPED;
		if (pending)
			mod_delayed_work(system_unbound_wq, &ctx->rebind_work, 1);
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return;
	}
	ret = s7d_capture_rebind(ctx);
	if (ret) {
		s7d_retire(ctx);
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	}
	mutex_unlock(&ctx->queue_lock);
	if (!ret)
		s7d_device_run(ctx);
}

static void s7d_complete_last(struct s7d_v4l2_context *ctx)
{
	struct vb2_v4l2_buffer *buffer = NULL;
	struct v4l2_event event = { .type = V4L2_EVENT_EOS };
	unsigned long flags;
	bool pending, unbound, reconfigure;
	unsigned int i;
	unsigned int attempts;
	int ret;

	spin_lock_irqsave(&ctx->state_lock, flags);
	pending = ctx->last_pending && !ctx->last_completing &&
		ctx->capture_streaming && !ctx->retired;
	if (pending)
		ctx->last_completing = true;
	unbound = ctx->ops.clear_stream && !ctx->capture_ready;
	reconfigure = ctx->change_pending && ctx->change_stopped;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (!pending)
		return;
	for (attempts = 0; attempts < S7D_CODEC_MAX_CAPTURE_BUFFERS; attempts++) {
		buffer = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buffer)
			break;
		if (unbound && !s7d_buffer(&buffer->vb2_buf)->surface)
			ret = 0;
		else
			ret = s7d_codec_surface_return_queued(s7d_buffer(&buffer->vb2_buf)->surface);
		if (!ret)
			break;
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, buffer);
		buffer = NULL;
		if (ret != -EBUSY)
			break;
	}
	if (!buffer) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->last_completing = false;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->last_pending = false;
	s7d_buffer(&buffer->vb2_buf)->completion_epoch = ctx->completion_epoch;
	s7d_buffer(&buffer->vb2_buf)->change_last_epoch = reconfigure ? ctx->change.epoch : 0;
	ctx->state = S7D_V4L2_STOPPED;
	if (reconfigure)
		ctx->change_last_done = true;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	for (i = 0; i < 2; i++)
		vb2_set_plane_payload(&buffer->vb2_buf, i, 0);
	v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, buffer);
	if (!reconfigure)
		v4l2_event_queue_fh(&ctx->fh, &event);
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->last_completing = false;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
}

static void s7d_buf_queue(struct vb2_buffer *vb)
{
	struct s7d_v4l2_context *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct s7d_v4l2_buffer *buffer = s7d_buffer(vb);
	unsigned long flags;
	bool streaming, retired, last, ready;
	int ret = 0;

	spin_lock_irqsave(&ctx->state_lock, flags);
	retired = ctx->retired;
	streaming = ctx->capture_streaming;
	ready = ctx->capture_ready && ctx->prepared && !ctx->capture_stopping;
	last = ctx->last_pending;
	if (vb->vb2_queue->is_output && !retired) {
		if (ctx->next_input_token == U64_MAX)
			ret = -EOVERFLOW;
		else
			buffer->token = ++ctx->next_input_token;
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (retired || ret) {
		v4l2_m2m_buf_done(&buffer->m2m.vb, VB2_BUF_STATE_ERROR);
		return;
	}
	if (!vb->vb2_queue->is_output) {
		ret = buffer->surface ? s7d_codec_surface_queue(buffer->surface) :
			ctx->ops.clear_stream && !ready ? 0 : -EAGAIN;
		if (ret) {
			v4l2_m2m_buf_done(&buffer->m2m.vb, VB2_BUF_STATE_ERROR);
			return;
		}
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, &buffer->m2m.vb);
	if (vb->vb2_queue->is_output) {
		s7d_device_run(ctx);
		return;
	}
	if (last) {
		s7d_complete_last(ctx);
		return;
	}
	if (streaming && ready) {
		mutex_lock(&ctx->backend_lock);
		ret = ctx->ops.capture_queue(ctx->backend, buffer->surface,
					     vb->index, buffer->generation);
		mutex_unlock(&ctx->backend_lock);
		if (ret) {
			vb2_queue_error(vb->vb2_queue);
			vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
			return;
		}
	}
	s7d_device_run(ctx);
}

static void s7d_sync_initial(struct s7d_v4l2_context *ctx, u64 token)
{
	unsigned long flags;
	int ret;

	lockdep_assert_held(&ctx->queue_lock);
	flush_work(&ctx->run_work);
	mutex_lock(&ctx->backend_lock);
	ret = ctx->ops.sync_initial(ctx->backend, token);
	mutex_unlock(&ctx->backend_lock);
	if (ret) {
		s7d_stop(ctx);
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
		return;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->initial_pending = false;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	ret = s7d_capture_rebind(ctx);
	if (ret) {
		s7d_stop(ctx);
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
		return;
	}
	s7d_device_run(ctx);
}

static int s7d_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct s7d_v4l2_context *ctx = vb2_get_drv_priv(q);
	struct s7d_codec_callbacks callbacks = { .frame_ready = s7d_frame_ready };
	struct s7d_codec_layout layout;
	struct s7d_v4l2_capture_buffer surfaces[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_v4l2_buffer *buffer;
	struct vb2_v4l2_buffer *input;
	unsigned long flags;
	unsigned int i;
	unsigned int num_surfaces = 0;
	size_t capacity;
	u64 initial_token;
	bool retired, valid;
	int ret;

	spin_lock_irqsave(&ctx->state_lock, flags);
	retired = ctx->retired;
	valid = ctx->sequence_valid;
	layout = ctx->layout;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (retired) {
		ret = -EOPNOTSUPP;
		goto return_queued;
	}
	if (q->is_output) {
		input = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
		if (!input) {
			ret = -EINVAL;
			goto return_queued;
		}
		initial_token = s7d_buffer(&input->vb2_buf)->token;
		ret = s7d_codec_session_open(ctx->core, ctx->codec.engine,
					     &callbacks, ctx, &ctx->session);
		if (ret)
			goto return_queued;
		if (check_add_overflow((size_t)ctx->output.plane_fmt[0].sizeimage,
				      (size_t)((ctx->ops.clear_stream ? S7D_H264_ASSEMBLY_MAX_BYTES :
					       S7D_H264_PREFIX_MAX_BYTES) + SZ_16K +
					      PAGE_SIZE + L1_CACHE_BYTES),
				      &capacity)) {
			ret = -EOVERFLOW;
			goto failed;
		}
		ctx->staging = s7d_codec_dma_alloc(ctx->session, capacity);
		if (IS_ERR(ctx->staging)) {
			ret = PTR_ERR(ctx->staging);
			ctx->staging = NULL;
			goto failed;
		}
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->output_streaming = true;
		ctx->state = S7D_V4L2_INITIALIZING;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		mutex_lock(&ctx->backend_lock);
		ret = ctx->ops.prepare(ctx->backend, ctx->session, ctx);
		mutex_unlock(&ctx->backend_lock);
		if (ret)
			goto failed;
		spin_lock_irqsave(&ctx->state_lock, flags);
		if (ctx->retired || (ctx->ops.clear_stream && ctx->state == S7D_V4L2_FAILED)) {
			spin_unlock_irqrestore(&ctx->state_lock, flags);
			ret = -ESHUTDOWN;
			goto failed;
		}
		ctx->initial_token = initial_token;
		ctx->initial_pending = !(ctx->ops.clear_stream && valid);
		ctx->prepared = true;
		if (ctx->ops.clear_stream && valid) {
			ctx->state = S7D_V4L2_CAPTURE_SETUP;
			ctx->rebind_pending = ctx->capture_streaming;
		}
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		if (ctx->ops.clear_stream && valid) {
			ret = s7d_capture_rebind(ctx);
			if (ret)
				goto failed;
			s7d_device_run(ctx);
			return 0;
		}
		s7d_device_run(ctx);
		/* Published input may already be DONE/ERROR; start must not fail now. */
		s7d_sync_initial(ctx, initial_token);
		return 0;
	}
	if (ctx->change_pending) {
		ret = s7d_reconfigure_start(ctx);
		if (ret)
			goto return_queued;
		s7d_device_run(ctx);
		return 0;
	}
	if (!valid || !ctx->session || count < layout.min_capture_buffers) {
		ret = -EAGAIN;
		goto return_queued;
	}
	if (ctx->ops.clear_stream) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		ctx->capture_streaming = true;
		ctx->rebind_pending = true;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		ret = s7d_capture_rebind(ctx);
		if (ret)
			goto failed;
		s7d_device_run(ctx);
		return 0;
	}
	mutex_lock(&ctx->backend_lock);
	for (i = 0; i < S7D_CODEC_MAX_CAPTURE_BUFFERS; i++) {
		buffer = ctx->surface[i];
		if (!buffer || buffer->m2m.vb.vb2_buf.state != VB2_BUF_STATE_ACTIVE)
			continue;
		surfaces[num_surfaces].surface = buffer->surface;
		surfaces[num_surfaces].surface_id = i;
		surfaces[num_surfaces++].generation = buffer->generation;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->capture_streaming = true;
	ctx->state = S7D_V4L2_DECODING;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	ret = ctx->ops.capture_start(ctx->backend, &layout, ctx->generation,
				     surfaces, num_surfaces);
	mutex_unlock(&ctx->backend_lock);
	if (ret)
		goto failed;
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (!ctx->retired)
		ctx->capture_ready = true;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	s7d_device_run(ctx);
	return 0;
failed:
	s7d_stop(ctx);
	if (ctx->ops.clear_stream &&
	    vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
return_queued:
	s7d_return_buffers(ctx, q, VB2_BUF_STATE_QUEUED);
	return ret < 0 ? ret : -EIO;
}

static void s7d_stop_streaming(struct vb2_queue *q)
{
	struct s7d_v4l2_context *ctx = vb2_get_drv_priv(q);
	struct vb2_queue *peer = q->is_output ?
		v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx) :
		v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	unsigned long flags;
	bool seek, capture_streaming;
	int ret;

	if (!q->is_output && ctx->capture_only_stop) {
		s7d_return_buffers(ctx, q, VB2_BUF_STATE_ERROR);
		return;
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	seek = q->is_output && ctx->ops.clear_stream && !ctx->retired &&
		ctx->state != S7D_V4L2_FAILED && !ctx->stop_error && !q->error && !peer->error;
	capture_streaming = ctx->capture_streaming;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	ret = s7d_stop(ctx);
	s7d_return_buffers(ctx, q, VB2_BUF_STATE_ERROR);
	if (seek && !ret) {
		ret = s7d_reset(ctx, capture_streaming);
		if (!ret)
			return;
	}
	if (ret) {
		dev_err(ctx->dev, "decoder stop failed: %d; memory retained\n", ret);
		vb2_queue_error(q);
	}
	if (ret || vb2_is_streaming(peer))
		vb2_queue_error(peer);
}

static void s7d_buf_cleanup(struct vb2_buffer *vb)
{
	struct s7d_v4l2_context *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct s7d_v4l2_buffer *buffer = s7d_buffer(vb);
	unsigned long flags;

	if (!buffer->surface)
		return;
	if (!ctx->retired && !s7d_capture_release(ctx))
		return;
	s7d_stop(ctx);
	spin_lock_irqsave(&ctx->state_lock, flags);
	ctx->surface[vb->index] = NULL;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	buffer->surface = NULL;
}

static const struct vb2_ops s7d_queue_ops = {
	.queue_setup = s7d_queue_setup,
	.buf_prepare = s7d_buf_prepare,
	.buf_queue = s7d_buf_queue,
	.buf_cleanup = s7d_buf_cleanup,
	.start_streaming = s7d_start_streaming,
	.stop_streaming = s7d_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int s7d_queue_init(void *priv, struct vb2_queue *output,
			  struct vb2_queue *capture)
{
	struct s7d_v4l2_context *ctx = priv;
	struct vb2_queue *q[2] = { output, capture };
	unsigned int i;
	int ret;

	for (i = 0; i < 2; i++) {
		q[i]->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		q[i]->io_modes = VB2_MMAP;
		q[i]->drv_priv = ctx;
		q[i]->buf_struct_size = sizeof(struct s7d_v4l2_buffer);
		q[i]->ops = &s7d_queue_ops;
		q[i]->mem_ops = i ? &s7d_codec_memory_memops : &vb2_vmalloc_memops;
		q[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q[i]->lock = &ctx->queue_lock;
		q[i]->dev = ctx->dev;
		q[i]->bidirectional = i == 1;
		q[i]->min_queued_buffers = 1;
		ret = vb2_queue_init(q[i]);
		if (ret) {
			if (i)
				vb2_queue_release(output);
			return ret;
		}
	}
	return 0;
}

static void s7d_run_work(struct work_struct *work)
{
	struct s7d_v4l2_context *ctx = container_of(work, struct s7d_v4l2_context,
						run_work);
	struct vb2_v4l2_buffer *vbuf;
	struct s7d_v4l2_buffer *buffer;
	struct s7d_v4l2_input input;
	unsigned long flags;
	void *source;
	size_t offset;
	bool claimed;
	int ret;

	mutex_lock(&ctx->backend_lock);
	vbuf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!vbuf) {
		mutex_unlock(&ctx->backend_lock);
		return;
	}
	buffer = s7d_buffer(&vbuf->vb2_buf);
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (!s7d_can_submit_locked(ctx, vbuf)) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		mutex_unlock(&ctx->backend_lock);
		return;
	}
	ctx->active_input = buffer;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	offset = vbuf->vb2_buf.planes[0].data_offset;
	source = vb2_plane_vaddr(&vbuf->vb2_buf, 0);
	input.memory = ctx->staging;
	input.bytes = vb2_get_plane_payload(&vbuf->vb2_buf, 0) - offset;
	input.padded_bytes = input.bytes < PAGE_SIZE ?
		PAGE_ALIGN(input.bytes + L1_CACHE_BYTES) :
		ALIGN(input.bytes, 64) + L1_CACHE_BYTES;
	input.token = buffer->token;
	input.timestamp_ns = vbuf->vb2_buf.timestamp;
	input.timestamp_meta = (struct s7d_codec_timestamp_metadata) {
		.flags = vbuf->flags & (V4L2_BUF_FLAG_TSTAMP_SRC_MASK |
				       V4L2_BUF_FLAG_TIMECODE),
	};
	if (input.timestamp_meta.flags & V4L2_BUF_FLAG_TIMECODE)
		input.timestamp_meta.timecode = vbuf->timecode;
	if (!source || input.padded_bytes > s7d_codec_dma_size(ctx->staging) ||
	    s7d_codec_dma_size(ctx->staging) - input.padded_bytes < SZ_16K) {
		ret = -EINVAL;
	} else {
		memcpy(s7d_codec_dma_cpu(ctx->staging), (u8 *)source + offset, input.bytes);
		memset((u8 *)s7d_codec_dma_cpu(ctx->staging) + input.bytes, 0,
		       input.padded_bytes - input.bytes + SZ_16K);
		dma_wmb();
		ret = ctx->ops.submit(ctx->backend, &input);
	}
	mutex_unlock(&ctx->backend_lock);
	if (ret) {
		spin_lock_irqsave(&ctx->state_lock, flags);
		claimed = s7d_claim_input_locked(ctx, input.token, false) == buffer;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		s7d_retire(ctx);
		if (claimed) {
			v4l2_m2m_src_buf_remove_by_buf(ctx->fh.m2m_ctx, vbuf);
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
		}
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	}
}

static int s7d_job_ready(void *priv)
{
	/* M2M supplies buffer helpers; native work owns all scheduling. */
	return 0;
}

static void s7d_job_abort(void *priv)
{
	s7d_stop(priv);
}

const struct v4l2_m2m_ops s7d_v4l2_m2m_ops = {
	.device_run = s7d_device_run,
	.job_ready = s7d_job_ready,
	.job_abort = s7d_job_abort,
};

static void s7d_drain_work(struct work_struct *work)
{
	struct s7d_v4l2_context *ctx = container_of(work, struct s7d_v4l2_context,
						drain_work);
	unsigned long flags;
	struct s7d_v4l2_drain request;
	bool ready;
	int ret;

	mutex_lock(&ctx->backend_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	ready = s7d_can_drain_locked(ctx);
	request.last_input_token = ctx->drain_token;
	request.epoch = ctx->drain_epoch;
	if (ready)
		ctx->drain_requested = true;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	ret = ready ? ctx->ops.drain(ctx->backend, &request) : 0;
	mutex_unlock(&ctx->backend_lock);
	if (ret) {
		s7d_retire(ctx);
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	}
}

int s7d_v4l2_input_done_result(struct s7d_v4l2_context *ctx, u64 token, int status,
			       bool *cancelled)
{
	struct s7d_v4l2_buffer *buffer;
	unsigned long flags;

	if (cancelled)
		*cancelled = false;
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (cancelled && ctx->retired && token && token > ctx->last_consumed_token &&
	    token == ctx->cancelled_input_token) {
		ctx->cancelled_input_token = 0;
		*cancelled = true;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return -ESHUTDOWN;
	}
	buffer = s7d_claim_input_locked(ctx, token, true);
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (!buffer)
		return -EINVAL;
	v4l2_m2m_src_buf_remove_by_buf(ctx->fh.m2m_ctx, &buffer->m2m.vb);
	v4l2_m2m_buf_done(&buffer->m2m.vb, status ? VB2_BUF_STATE_ERROR :
			  VB2_BUF_STATE_DONE);
	s7d_device_run(ctx);
	return 0;
}

int s7d_v4l2_input_done(struct s7d_v4l2_context *ctx, u64 token, int status)
{
	return s7d_v4l2_input_done_result(ctx, token, status, NULL);
}

int s7d_v4l2_error(struct s7d_v4l2_context *ctx, int error)
{
	unsigned long flags;

	if (error >= 0)
		return -EINVAL;
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (ctx->retired) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return -ESHUTDOWN;
	}
	ctx->state = S7D_V4L2_FAILED;
	ctx->prepared = false;
	ctx->capture_ready = false;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
	vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	dev_err(ctx->dev, "decoder failed: %d\n", error);
	return 0;
}

int s7d_v4l2_drain_done_result(struct s7d_v4l2_context *ctx, u64 epoch, u64 token,
			       bool *cancelled)
{
	unsigned long flags;

	if (cancelled)
		*cancelled = false;
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (cancelled && ctx->retired && epoch && epoch == ctx->cancelled_drain_epoch &&
	    ctx->drain_requested && !ctx->active_input && !ctx->last_pending &&
	    !ctx->last_completing && epoch == ctx->drain_epoch &&
	    token == ctx->drain_token && token == ctx->last_consumed_token) {
		ctx->cancelled_drain_epoch = 0;
		*cancelled = true;
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return -ESHUTDOWN;
	}
	if (ctx->retired || ctx->state != S7D_V4L2_DRAINING ||
	    !ctx->drain_requested || ctx->active_input || token != ctx->drain_token ||
	    token != ctx->last_consumed_token || epoch != ctx->drain_epoch ||
	    ctx->last_pending) {
		spin_unlock_irqrestore(&ctx->state_lock, flags);
		return -EINVAL;
	}
	ctx->last_pending = true;
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	s7d_complete_last(ctx);
	return 0;
}

int s7d_v4l2_drain_done(struct s7d_v4l2_context *ctx, u64 epoch, u64 token)
{
	return s7d_v4l2_drain_done_result(ctx, epoch, token, NULL);
}

int s7d_v4l2_decoder_cmd(struct s7d_v4l2_context *ctx,
			 struct v4l2_decoder_cmd *command)
{
	unsigned long flags;
	bool resume = false, reconfigure = false;
	int ret = 0;

	if (command->cmd != V4L2_DEC_CMD_START && command->cmd != V4L2_DEC_CMD_STOP)
		return -EINVAL;
	command->flags = 0;
	if (command->cmd == V4L2_DEC_CMD_STOP)
		command->stop.pts = 0;
	else {
		command->start.speed = 0;
		command->start.format = V4L2_DEC_START_FMT_NONE;
	}
	mutex_lock(&ctx->queue_lock);
	spin_lock_irqsave(&ctx->state_lock, flags);
	if (ctx->retired) {
		ret = -ESHUTDOWN;
	} else if (ctx->state == S7D_V4L2_FAILED) {
		ret = -EIO;
	} else if (ctx->change_pending) {
		if (ctx->last_completing) {
			ret = -EBUSY;
		} else if (command->cmd == V4L2_DEC_CMD_STOP) {
			if (ctx->change_draining)
				ret = -EBUSY;
			else if (ctx->drain_epoch == U64_MAX)
				ret = -EOVERFLOW;
			else {
				ctx->drain_epoch++;
				ctx->drain_token = ctx->next_input_token;
				ctx->drain_requested = false;
				ctx->change_draining = true;
			}
		} else if (!ctx->change_stopped || !ctx->change_last_done ||
			   !ctx->capture_streaming || !ctx->change_last_dequeued) {
			ret = -EBUSY;
		} else {
			reconfigure = true;
		}
	} else if (ctx->state == S7D_V4L2_DRAINING || ctx->last_completing) {
		ret = -EBUSY;
	} else if (command->cmd == V4L2_DEC_CMD_STOP) {
		if (ctx->output_streaming && ctx->capture_streaming &&
		    ctx->state != S7D_V4L2_STOPPED) {
			if (ctx->drain_epoch == U64_MAX) {
				ret = -EOVERFLOW;
			} else {
				ctx->drain_epoch++;
				ctx->drain_token = ctx->next_input_token;
				ctx->drain_requested = false;
				ctx->state = S7D_V4L2_DRAINING;
			}
		}
	} else if (ctx->state == S7D_V4L2_STOPPED) {
		resume = true;
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (reconfigure)
		ret = s7d_reconfigure_start(ctx);
	if (resume) {
		mutex_lock(&ctx->backend_lock);
		ret = ctx->ops.resume(ctx->backend);
		mutex_unlock(&ctx->backend_lock);
		if (!ret) {
			vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
			v4l2_m2m_clear_state(ctx->fh.m2m_ctx);
			spin_lock_irqsave(&ctx->state_lock, flags);
			ctx->state = ctx->capture_ready ? S7D_V4L2_DECODING :
				ctx->sequence_valid ? S7D_V4L2_CAPTURE_SETUP : S7D_V4L2_INITIALIZING;
			ctx->drain_requested = false;
			spin_unlock_irqrestore(&ctx->state_lock, flags);
		}
	}
	mutex_unlock(&ctx->queue_lock);
	if (!ret)
		s7d_device_run(ctx);
	return ret < 0 ? ret : ret ? -EIO : 0;
}

struct s7d_v4l2_context *
s7d_v4l2_context_create(struct device *dev, struct video_device *video,
			struct v4l2_m2m_dev *m2m, struct s7d_codec_core *core,
			const struct s7d_v4l2_codec *codec,
			const struct s7d_v4l2_backend_ops *ops, void *backend)
{
	struct s7d_v4l2_context *ctx;
	struct v4l2_ctrl *control;
	int ret;

	if (!dev || !video || !m2m || !core || !s7d_v4l2_codec_valid(codec))
		return ERR_PTR(-EINVAL);
	if (!ops || !ops->prepare || !ops->submit || !ops->sync_initial ||
	    !ops->capture_start || !ops->capture_stop ||
	    !ops->capture_queue || !ops->drain || !ops->resume || !ops->detach ||
	    !ops->reset || !!ops->reconfigure_stop != !!ops->reconfigure_restore ||
	    (ops->reconfigure_stop && !ops->clear_stream))
		return ERR_PTR(-EOPNOTSUPP);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);
	ctx->dev = get_device(dev);
	ctx->core = core;
	ctx->ops = *ops;
	ctx->codec = *codec;
	ctx->backend = backend;
	mutex_init(&ctx->queue_lock);
	mutex_init(&ctx->backend_lock);
	mutex_init(&ctx->retire_lock);
	spin_lock_init(&ctx->state_lock);
	INIT_WORK(&ctx->run_work, s7d_run_work);
	INIT_WORK(&ctx->drain_work, s7d_drain_work);
	INIT_DELAYED_WORK(&ctx->rebind_work, s7d_rebind_work);
	INIT_DELAYED_WORK(&ctx->change_work, s7d_change_work);
	ctx->completion_epoch = 1;
	ctx->output.width = 1920;
	ctx->output.height = 1080;
	ctx->output.pixelformat = codec->output_format;
	ctx->output.field = V4L2_FIELD_NONE;
	ctx->output.num_planes = 1;
	ctx->output.plane_fmt[0].sizeimage = min_t(u32, S7D_OUTPUT_DEFAULT_SIZE,
						codec->max_input_size);
	ret = s7d_placeholder(codec, 1920, 1080, V4L2_PIX_FMT_NV12M, &ctx->layout);
	if (ret)
		goto free;
	s7d_capture_format(&ctx->layout, &ctx->capture);
	v4l2_ctrl_handler_init(&ctx->controls, 1);
	control = v4l2_ctrl_new_std(&ctx->controls, &s7d_ctrl_ops,
		V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 1, S7D_CODEC_MAX_CAPTURE_BUFFERS, 1, 1);
	if (control)
		control->flags |= V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE;
	if (ctx->controls.error) {
		ret = ctx->controls.error;
		goto free_controls;
	}
	v4l2_fh_init(&ctx->fh, video);
	ctx->fh.ctrl_handler = &ctx->controls;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(m2m, ctx, s7d_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		goto free_controls;
	}
	ctx->fh.m2m_ctx->ignore_cap_streaming = true;
	v4l2_m2m_set_dst_buffered(ctx->fh.m2m_ctx, true);
	v4l2_fh_add(&ctx->fh);
	return ctx;
free_controls:
	v4l2_ctrl_handler_free(&ctx->controls);
free:
	put_device(ctx->dev);
	kfree(ctx);
	return ERR_PTR(ret);
}

int s7d_v4l2_context_shutdown(struct s7d_v4l2_context *ctx)
{
	int ret;

	mutex_lock(&ctx->queue_lock);
	ret = s7d_stop(ctx);
	vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
	vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	mutex_unlock(&ctx->queue_lock);
	return ret;
}

int s7d_v4l2_context_destroy(struct s7d_v4l2_context *ctx)
{
	int ret;

	ret = s7d_v4l2_context_shutdown(ctx);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	if (ctx->session)
		s7d_codec_session_put(ctx->session);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->controls);
	put_device(ctx->dev);
	kfree(ctx);
	return ret;
}

struct v4l2_fh *s7d_v4l2_fh(struct s7d_v4l2_context *ctx)
{
	return &ctx->fh;
}
