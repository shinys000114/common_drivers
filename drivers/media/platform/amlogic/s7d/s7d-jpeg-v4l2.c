// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-vmalloc.h>

#include "s7d-codec-session.h"
#include "s7d-jpeg-backend.h"
#include "s7d-jpeg-v4l2.h"

#define JPEG_MIN_DIMENSION 16
#define JPEG_MAX_WIDTH 1920
#define JPEG_MAX_HEIGHT 1080
#define JPEG_MIN_CAPTURE (2 * PAGE_SIZE)
#define JPEG_MAX_CAPTURE SZ_16M

struct s7d_jpeg_client {
	struct list_head node;
	struct s7d_jpeg_v4l2 *device;
	struct s7d_codec_session *session;
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler controls;
	struct work_struct work;
	/* Serializes drain state with worker completion. */
	spinlock_t state_lock;
	struct v4l2_pix_format_mplane output;
	struct v4l2_pix_format_mplane capture;
	u32 output_sequence;
	u32 capture_sequence;
	u32 quality;
	int error;
	bool output_streaming;
	bool capture_streaming;
	bool closing;
	bool aborted;
};

struct s7d_jpeg_v4l2 {
	struct device *dev;
	struct s7d_jpeg_encoder *encoder;
	struct s7d_codec_core *core;
	struct v4l2_device v4l2;
	struct video_device *video;
	struct v4l2_m2m_dev *m2m;
	/* Serializes both vb2 queues, ioctls and client lifetime. */
	struct mutex lock;
	struct completion video_released;
	struct list_head clients;
	struct list_head quarantine;
	bool registered;
	bool disconnected;
};

static struct s7d_jpeg_client *jpeg_client(void *priv)
{
	return container_of(priv, struct s7d_jpeg_client, fh);
}

static bool jpeg_output(enum v4l2_buf_type type)
{
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static bool jpeg_capture(enum v4l2_buf_type type)
{
	return type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
}

static void jpeg_colour(struct v4l2_pix_format_mplane *format)
{
	format->colorspace = V4L2_COLORSPACE_JPEG;
	format->ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_SRGB;
	format->field = V4L2_FIELD_NONE;
	format->num_planes = 1;
}

static void jpeg_raw_format(struct v4l2_pix_format_mplane *format)
{
	u32 fourcc = format->pixelformat == V4L2_PIX_FMT_NV21 ?
		V4L2_PIX_FMT_NV21 : V4L2_PIX_FMT_NV12;
	u32 width = clamp_t(u32, format->width, JPEG_MIN_DIMENSION, JPEG_MAX_WIDTH) & ~1;
	u32 height = clamp_t(u32, format->height, JPEG_MIN_DIMENSION, JPEG_MAX_HEIGHT) & ~1;

	memset(format, 0, sizeof(*format));
	format->pixelformat = fourcc;
	format->width = width;
	format->height = height;
	jpeg_colour(format);
	format->plane_fmt[0].bytesperline = ALIGN(width, 32);
	format->plane_fmt[0].sizeimage = ALIGN(width, 32) * height * 3 / 2;
}

static void jpeg_coded_format(struct s7d_jpeg_client *ctx,
			      struct v4l2_pix_format_mplane *format)
{
	u32 size = format->plane_fmt[0].sizeimage;

	if (!size)
		size = PAGE_ALIGN(ctx->output.width * ctx->output.height * 3 + SZ_64K);
	size = clamp_t(u32, size, JPEG_MIN_CAPTURE, JPEG_MAX_CAPTURE);
	memset(format, 0, sizeof(*format));
	format->pixelformat = V4L2_PIX_FMT_JPEG;
	format->width = ctx->output.width;
	format->height = ctx->output.height;
	jpeg_colour(format);
	format->plane_fmt[0].sizeimage = PAGE_ALIGN(size);
}

static int jpeg_querycap(struct file *file, void *priv,
			 struct v4l2_capability *capability)
{
	strscpy(capability->driver, "s7d-jpeg", sizeof(capability->driver));
	strscpy(capability->card, "S7D JPEG Encoder", sizeof(capability->card));
	return 0;
}

static int jpeg_enum_format(struct file *file, void *priv,
			    struct v4l2_fmtdesc *format)
{
	if (jpeg_output(format->type)) {
		if (format->index > 1)
			return -EINVAL;
		format->pixelformat = format->index ? V4L2_PIX_FMT_NV21 : V4L2_PIX_FMT_NV12;
		format->flags = 0;
		return 0;
	}
	if (!jpeg_capture(format->type) || format->index)
		return -EINVAL;
	format->pixelformat = V4L2_PIX_FMT_JPEG;
	format->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int jpeg_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *sizes)
{
	if (sizes->index || (sizes->pixel_format != V4L2_PIX_FMT_JPEG &&
			    sizes->pixel_format != V4L2_PIX_FMT_NV12 &&
			    sizes->pixel_format != V4L2_PIX_FMT_NV21))
		return -EINVAL;
	sizes->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	sizes->stepwise.min_width = JPEG_MIN_DIMENSION;
	sizes->stepwise.max_width = JPEG_MAX_WIDTH;
	sizes->stepwise.step_width = 2;
	sizes->stepwise.min_height = JPEG_MIN_DIMENSION;
	sizes->stepwise.max_height = JPEG_MAX_HEIGHT;
	sizes->stepwise.step_height = 2;
	return 0;
}

static int jpeg_try_format(struct file *file, void *priv, struct v4l2_format *format)
{
	struct s7d_jpeg_client *ctx = jpeg_client(priv);

	if (jpeg_output(format->type))
		jpeg_raw_format(&format->fmt.pix_mp);
	else if (jpeg_capture(format->type))
		jpeg_coded_format(ctx, &format->fmt.pix_mp);
	else
		return -EINVAL;
	return 0;
}

static int jpeg_set_format(struct file *file, void *priv, struct v4l2_format *format)
{
	struct s7d_jpeg_client *ctx = jpeg_client(priv);
	int ret;

	if (vb2_is_busy(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	ret = jpeg_try_format(file, priv, format);
	if (ret)
		return ret;
	if (jpeg_output(format->type)) {
		ctx->output = format->fmt.pix_mp;
		ctx->capture.plane_fmt[0].sizeimage = 0;
		jpeg_coded_format(ctx, &ctx->capture);
	} else {
		ctx->capture = format->fmt.pix_mp;
	}
	return 0;
}

static int jpeg_get_format(struct file *file, void *priv, struct v4l2_format *format)
{
	struct s7d_jpeg_client *ctx = jpeg_client(priv);

	if (jpeg_output(format->type))
		format->fmt.pix_mp = ctx->output;
	else if (jpeg_capture(format->type))
		format->fmt.pix_mp = ctx->capture;
	else
		return -EINVAL;
	return 0;
}

static void jpeg_eos(struct s7d_jpeg_client *ctx)
{
	const struct v4l2_event event = { .type = V4L2_EVENT_EOS };

	v4l2_event_queue_fh(&ctx->fh, &event);
}

static int jpeg_encoder_cmd(struct file *file, void *priv,
			    struct v4l2_encoder_cmd *command)
{
	struct s7d_jpeg_client *ctx = jpeg_client(priv);
	unsigned long flags;
	bool eos;
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, command);
	if (ret)
		return ret;
	if (!ctx->output_streaming || !ctx->capture_streaming)
		return 0;
	if (ctx->error || ctx->closing)
		return ctx->error ?: -ESHUTDOWN;
	spin_lock_irqsave(&ctx->state_lock, flags);
	ret = v4l2_m2m_encoder_cmd(file, ctx->fh.m2m_ctx, command);
	eos = !ret && command->cmd == V4L2_ENC_CMD_STOP &&
		v4l2_m2m_has_stopped(ctx->fh.m2m_ctx);
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (eos)
		jpeg_eos(ctx);
	if (!ret && command->cmd == V4L2_ENC_CMD_START) {
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	}
	return ret;
}

static int jpeg_subscribe(struct v4l2_fh *fh,
			  const struct v4l2_event_subscription *subscription)
{
	if (subscription->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, subscription, 1, NULL);
	return v4l2_ctrl_subscribe_event(fh, subscription);
}

static int jpeg_control(struct v4l2_ctrl *control)
{
	struct s7d_jpeg_client *ctx = container_of(control->handler,
						struct s7d_jpeg_client, controls);

	if (control->id != V4L2_CID_JPEG_COMPRESSION_QUALITY)
		return -EINVAL;
	WRITE_ONCE(ctx->quality, control->val);
	return 0;
}

static const struct v4l2_ctrl_ops jpeg_control_ops = {
	.s_ctrl = jpeg_control,
};

static void jpeg_unused_frame(void *priv, const struct s7d_codec_frame *frame)
{
}

static const struct s7d_codec_callbacks jpeg_callbacks = {
	.frame_ready = jpeg_unused_frame,
};

static void jpeg_return_buffers(struct s7d_jpeg_client *ctx,
				enum v4l2_buf_type type, enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *buffer;

	for (;;) {
		buffer = jpeg_output(type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buffer)
			break;
		if (!jpeg_output(type))
			vb2_set_plane_payload(&buffer->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(buffer, state);
	}
}

static int jpeg_retire(struct s7d_jpeg_client *ctx)
{
	int ret;

	if (!ctx->session)
		return 0;
	ret = s7d_jpeg_encoder_stop(ctx->device->encoder, ctx->session);
	if (ret)
		return ret;
	ret = s7d_codec_session_stop(ctx->session);
	if (ret)
		return ret;
	s7d_codec_session_put(ctx->session);
	ctx->session = NULL;
	return 0;
}

static int jpeg_queue_setup(struct vb2_queue *queue, unsigned int *count,
			     unsigned int *planes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct s7d_jpeg_client *ctx = vb2_get_drv_priv(queue);
	u32 size = jpeg_output(queue->type) ? ctx->output.plane_fmt[0].sizeimage :
		ctx->capture.plane_fmt[0].sizeimage;

	if (*planes)
		return *planes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = size;
	return 0;
}

static int jpeg_buffer_prepare(struct vb2_buffer *vb)
{
	struct s7d_jpeg_client *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *buffer = to_vb2_v4l2_buffer(vb);
	u32 size = jpeg_output(vb->type) ? ctx->output.plane_fmt[0].sizeimage :
		ctx->capture.plane_fmt[0].sizeimage;

	if (vb->num_planes != 1 || vb2_plane_size(vb, 0) < size ||
	    vb->planes[0].data_offset)
		return -EINVAL;
	if (jpeg_output(vb->type)) {
		if (vb2_get_plane_payload(vb, 0) < size)
			return -EINVAL;
		if (buffer->field != V4L2_FIELD_NONE && buffer->field != V4L2_FIELD_ANY)
			return -EINVAL;
	} else {
		vb2_set_plane_payload(vb, 0, 0);
	}
	buffer->field = V4L2_FIELD_NONE;
	return 0;
}

static void jpeg_buffer_queue(struct vb2_buffer *vb)
{
	struct s7d_jpeg_client *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *buffer = to_vb2_v4l2_buffer(vb);
	unsigned long flags;
	bool last;

	spin_lock_irqsave(&ctx->state_lock, flags);
	last = !jpeg_output(vb->type) && v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx);
	if (last) {
		buffer->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
	} else {
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, buffer);
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (last) {
		vb2_set_plane_payload(vb, 0, 0);
		v4l2_m2m_buf_done(buffer, VB2_BUF_STATE_DONE);
		jpeg_eos(ctx);
	}
}

static int jpeg_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct s7d_jpeg_client *ctx = vb2_get_drv_priv(queue);
	unsigned long flags;
	int ret = 0;

	if (ctx->closing || ctx->device->disconnected || ctx->session || ctx->error) {
		ret = ctx->error ?: -ESHUTDOWN;
		goto return_buffers;
	}
	if (jpeg_output(queue->type))
		WRITE_ONCE(ctx->output_streaming, true);
	else
		WRITE_ONCE(ctx->capture_streaming, true);
	if (ctx->output_streaming && ctx->capture_streaming) {
		ret = s7d_codec_session_open(ctx->device->core, S7D_CODEC_ENGINE_HCODEC,
					     &jpeg_callbacks, NULL, &ctx->session);
		if (ret)
			goto clear_streaming;
		ctx->output_sequence = 0;
		ctx->capture_sequence = 0;
		WRITE_ONCE(ctx->aborted, false);
	}
	spin_lock_irqsave(&ctx->state_lock, flags);
	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, queue);
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	return 0;
clear_streaming:
	if (jpeg_output(queue->type))
		WRITE_ONCE(ctx->output_streaming, false);
	else
		WRITE_ONCE(ctx->capture_streaming, false);
return_buffers:
	jpeg_return_buffers(ctx, queue->type, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void jpeg_stop_streaming(struct vb2_queue *queue)
{
	struct s7d_jpeg_client *ctx = vb2_get_drv_priv(queue);
	unsigned long flags;
	int ret;

	if (jpeg_output(queue->type))
		WRITE_ONCE(ctx->output_streaming, false);
	else
		WRITE_ONCE(ctx->capture_streaming, false);
	/* M2M has joined the job; the worker never needs this queue mutex. */
	flush_work(&ctx->work);
	ret = jpeg_retire(ctx);
	WRITE_ONCE(ctx->error, ret);
	jpeg_return_buffers(ctx, queue->type, VB2_BUF_STATE_ERROR);
	spin_lock_irqsave(&ctx->state_lock, flags);
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, queue);
	spin_unlock_irqrestore(&ctx->state_lock, flags);
}

static const struct vb2_ops jpeg_queue_ops = {
	.queue_setup = jpeg_queue_setup,
	.buf_prepare = jpeg_buffer_prepare,
	.buf_queue = jpeg_buffer_queue,
	.start_streaming = jpeg_start_streaming,
	.stop_streaming = jpeg_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int jpeg_queue_init(void *priv, struct vb2_queue *output,
			   struct vb2_queue *capture)
{
	struct s7d_jpeg_client *ctx = priv;
	unsigned int i;
	int ret;

	for (i = 0; i < 2; i++) {
		struct vb2_queue *queue = i ? capture : output;

		queue->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		queue->io_modes = VB2_MMAP;
		queue->drv_priv = ctx;
		queue->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		queue->ops = &jpeg_queue_ops;
		queue->mem_ops = &vb2_vmalloc_memops;
		queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		queue->lock = &ctx->device->lock;
		queue->dev = ctx->device->dev;
		ret = vb2_queue_init(queue);
		if (ret)
			return ret;
	}
	return 0;
}

static void jpeg_run(struct work_struct *work)
{
	struct s7d_jpeg_client *ctx = container_of(work, struct s7d_jpeg_client, work);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(m2m);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(m2m);
	struct s7d_jpeg_encode_result result = { };
	struct s7d_jpeg_source source = {
		.format = ctx->output.pixelformat,
		.width = ctx->output.width,
		.height = ctx->output.height,
		.stride = ctx->output.plane_fmt[0].bytesperline,
	};
	unsigned long flags;
	bool last = false;
	int ret = -ESHUTDOWN;

	if (!src || !dst)
		goto finish;
	if (!READ_ONCE(ctx->aborted) && !READ_ONCE(ctx->closing) && ctx->session) {
		source.data = vb2_plane_vaddr(&src->vb2_buf, 0);
		source.bytes = vb2_get_plane_payload(&src->vb2_buf, 0);
		ret = s7d_jpeg_encoder_encode(ctx->device->encoder, ctx->session, &source,
					      vb2_plane_vaddr(&dst->vb2_buf, 0),
					      vb2_plane_size(&dst->vb2_buf, 0),
					      READ_ONCE(ctx->quality), &result);
	}
	if (!ret && (!result.bytesused ||
		     result.bytesused > vb2_plane_size(&dst->vb2_buf, 0)))
		ret = -EOVERFLOW;
	spin_lock_irqsave(&ctx->state_lock, flags);
	src = v4l2_m2m_src_buf_remove(m2m);
	dst = v4l2_m2m_dst_buf_remove(m2m);
	if (src && dst) {
		last = v4l2_m2m_is_last_draining_src_buf(m2m, src);
		if (last)
			v4l2_m2m_mark_stopped(m2m);
	}
	spin_unlock_irqrestore(&ctx->state_lock, flags);
	if (src) {
		src->sequence = ctx->output_sequence++;
		if (dst) {
			v4l2_m2m_buf_copy_metadata(src, dst, true);
			dst->sequence = ctx->capture_sequence++;
			dst->flags &= ~(V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME);
			dst->flags |= V4L2_BUF_FLAG_KEYFRAME;
			if (last)
				dst->flags |= V4L2_BUF_FLAG_LAST;
			vb2_set_plane_payload(&dst->vb2_buf, 0, ret ? 0 : result.bytesused);
			v4l2_m2m_buf_done(dst, ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		}
		v4l2_m2m_buf_done(src, ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	} else if (dst) {
		vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
	}
	if (last)
		jpeg_eos(ctx);
finish:
	if (ret && !READ_ONCE(ctx->aborted) && !READ_ONCE(ctx->closing)) {
		WRITE_ONCE(ctx->error, ret);
		vb2_queue_error(v4l2_m2m_get_src_vq(m2m));
		vb2_queue_error(v4l2_m2m_get_dst_vq(m2m));
	}
	v4l2_m2m_job_finish(ctx->device->m2m, m2m);
}

static void jpeg_device_run(void *priv)
{
	struct s7d_jpeg_client *ctx = priv;

	schedule_work(&ctx->work);
}

static int jpeg_job_ready(void *priv)
{
	struct s7d_jpeg_client *ctx = priv;

	return READ_ONCE(ctx->session) && !READ_ONCE(ctx->error) &&
		!READ_ONCE(ctx->closing) && !READ_ONCE(ctx->aborted) &&
		!v4l2_m2m_has_stopped(ctx->fh.m2m_ctx);
}

static void jpeg_job_abort(void *priv)
{
	struct s7d_jpeg_client *ctx = priv;

	WRITE_ONCE(ctx->aborted, true);
	s7d_jpeg_encoder_request_stop(ctx->device->encoder);
}

static const struct v4l2_m2m_ops jpeg_m2m_ops = {
	.device_run = jpeg_device_run,
	.job_ready = jpeg_job_ready,
	.job_abort = jpeg_job_abort,
};

static const struct v4l2_ioctl_ops jpeg_ioctl_ops = {
	.vidioc_querycap = jpeg_querycap,
	.vidioc_enum_fmt_vid_out = jpeg_enum_format,
	.vidioc_enum_fmt_vid_cap = jpeg_enum_format,
	.vidioc_enum_framesizes = jpeg_enum_framesizes,
	.vidioc_try_fmt_vid_out_mplane = jpeg_try_format,
	.vidioc_try_fmt_vid_cap_mplane = jpeg_try_format,
	.vidioc_s_fmt_vid_out_mplane = jpeg_set_format,
	.vidioc_s_fmt_vid_cap_mplane = jpeg_set_format,
	.vidioc_g_fmt_vid_out_mplane = jpeg_get_format,
	.vidioc_g_fmt_vid_cap_mplane = jpeg_get_format,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = jpeg_encoder_cmd,
	.vidioc_subscribe_event = jpeg_subscribe,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int jpeg_open(struct file *file)
{
	struct s7d_jpeg_v4l2 *device = video_drvdata(file);
	struct s7d_jpeg_client *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->device = device;
	spin_lock_init(&ctx->state_lock);
	INIT_WORK(&ctx->work, jpeg_run);
	ctx->output.width = 640;
	ctx->output.height = 480;
	jpeg_raw_format(&ctx->output);
	jpeg_coded_format(ctx, &ctx->capture);
	mutex_lock(&device->lock);
	if (device->disconnected || !device->registered ||
	    !list_empty(&device->quarantine)) {
		ret = -ENODEV;
		goto unlock;
	}
	v4l2_fh_init(&ctx->fh, device->video);
	v4l2_ctrl_handler_init(&ctx->controls, 1);
	v4l2_ctrl_new_std(&ctx->controls, &jpeg_control_ops,
			  V4L2_CID_JPEG_COMPRESSION_QUALITY, 1, 100, 1, 75);
	if (ctx->controls.error) {
		ret = ctx->controls.error;
		goto free_controls;
	}
	ctx->fh.ctrl_handler = &ctx->controls;
	ret = v4l2_ctrl_handler_setup(&ctx->controls);
	if (ret)
		goto free_controls;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(device->m2m, ctx, jpeg_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto free_controls;
	}
	v4l2_fh_add(&ctx->fh);
	list_add_tail(&ctx->node, &device->clients);
	file->private_data = &ctx->fh;
	mutex_unlock(&device->lock);
	return 0;
free_controls:
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_fh_exit(&ctx->fh);
unlock:
	mutex_unlock(&device->lock);
	kfree(ctx);
	return ret;
}

static int jpeg_release(struct file *file)
{
	struct s7d_jpeg_v4l2 *device = video_drvdata(file);
	struct s7d_jpeg_client *ctx = jpeg_client(file->private_data);
	int ret;

	mutex_lock(&device->lock);
	WRITE_ONCE(ctx->closing, true);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	flush_work(&ctx->work);
	ret = jpeg_retire(ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->controls);
	file->private_data = NULL;
	if (ret) {
		ctx->fh.m2m_ctx = NULL;
		list_move_tail(&ctx->node, &device->quarantine);
	} else {
		list_del(&ctx->node);
		kfree(ctx);
	}
	mutex_unlock(&device->lock);
	return ret;
}

static const struct v4l2_file_operations jpeg_file_ops = {
	.owner = THIS_MODULE,
	.open = jpeg_open,
	.release = jpeg_release,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = v4l2_compat_ioctl32,
#endif
};

static void jpeg_video_release(struct video_device *video)
{
	struct s7d_jpeg_v4l2 *device = video_get_drvdata(video);

	mutex_lock(&device->lock);
	device->video = NULL;
	video_device_release(video);
	complete_all(&device->video_released);
	mutex_unlock(&device->lock);
}

struct s7d_jpeg_v4l2 *
s7d_jpeg_v4l2_create(struct device *dev, struct s7d_jpeg_encoder *encoder)
{
	struct s7d_jpeg_v4l2 *device;
	int ret;

	if (!dev || !encoder)
		return ERR_PTR(-EINVAL);
	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return ERR_PTR(-ENOMEM);
	device->dev = get_device(dev);
	device->encoder = encoder;
	device->core = s7d_jpeg_encoder_core(encoder);
	if (IS_ERR_OR_NULL(device->core)) {
		ret = device->core ? PTR_ERR(device->core) : -EINVAL;
		goto free;
	}
	mutex_init(&device->lock);
	init_completion(&device->video_released);
	complete_all(&device->video_released);
	INIT_LIST_HEAD(&device->clients);
	INIT_LIST_HEAD(&device->quarantine);
	strscpy(device->v4l2.name, "s7d-jpeg", sizeof(device->v4l2.name));
	ret = v4l2_device_register(dev, &device->v4l2);
	if (ret)
		goto free;
	device->m2m = v4l2_m2m_init(&jpeg_m2m_ops);
	if (IS_ERR(device->m2m)) {
		ret = PTR_ERR(device->m2m);
		v4l2_device_unregister(&device->v4l2);
		goto free;
	}
	return device;
free:
	put_device(device->dev);
	kfree(device);
	return ERR_PTR(ret);
}

int s7d_jpeg_v4l2_register(struct s7d_jpeg_v4l2 *device)
{
	struct video_device *video;
	bool free_video = false;
	int ret;

	mutex_lock(&device->lock);
	if (device->registered || device->disconnected || device->video) {
		ret = device->registered ? -EALREADY : -ENODEV;
		goto unlock;
	}
	video = video_device_alloc();
	if (!video) {
		ret = -ENOMEM;
		goto unlock;
	}
	strscpy(video->name, "S7D JPEG Encoder", sizeof(video->name));
	video->fops = &jpeg_file_ops;
	video->ioctl_ops = &jpeg_ioctl_ops;
	video->release = jpeg_video_release;
	video->lock = &device->lock;
	video->v4l2_dev = &device->v4l2;
	video->vfl_dir = VFL_DIR_M2M;
	video->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(video, device);
	reinit_completion(&device->video_released);
	device->video = video;
	mutex_unlock(&device->lock);
	ret = video_register_device(video, VFL_TYPE_VIDEO, -1);
	mutex_lock(&device->lock);
	if (ret) {
		device->disconnected = true;
		if (device->video == video && !video->dev.release)
			free_video = true;
	} else {
		device->registered = true;
	}
	mutex_unlock(&device->lock);
	if (free_video)
		jpeg_video_release(video);
	return ret;
unlock:
	mutex_unlock(&device->lock);
	return ret;
}

void s7d_jpeg_v4l2_unregister(struct s7d_jpeg_v4l2 *device)
{
	struct video_device *video = NULL;

	mutex_lock(&device->lock);
	device->disconnected = true;
	if (device->registered) {
		device->registered = false;
		video = device->video;
	}
	mutex_unlock(&device->lock);
	if (video)
		video_unregister_device(video);
}

int s7d_jpeg_v4l2_shutdown(struct s7d_jpeg_v4l2 *device)
{
	struct s7d_jpeg_client *ctx, *next;
	int ret = 0, stopped;

	s7d_jpeg_v4l2_unregister(device);
	mutex_lock(&device->lock);
	list_for_each_entry(ctx, &device->clients, node) {
		WRITE_ONCE(ctx->closing, true);
		v4l2_m2m_streamoff(NULL, ctx->fh.m2m_ctx,
				   V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		v4l2_m2m_streamoff(NULL, ctx->fh.m2m_ctx,
				   V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		flush_work(&ctx->work);
		stopped = jpeg_retire(ctx);
		if (stopped && !ret)
			ret = stopped;
	}
	list_for_each_entry_safe(ctx, next, &device->quarantine, node) {
		stopped = jpeg_retire(ctx);
		if (stopped) {
			if (!ret)
				ret = stopped;
			continue;
		}
		list_del(&ctx->node);
		kfree(ctx);
	}
	mutex_unlock(&device->lock);
	return ret;
}

int s7d_jpeg_v4l2_destroy(struct s7d_jpeg_v4l2 *device)
{
	bool busy;

	s7d_jpeg_v4l2_unregister(device);
	mutex_lock(&device->lock);
	busy = !list_empty(&device->clients) || !list_empty(&device->quarantine);
	mutex_unlock(&device->lock);
	if (busy)
		return -EBUSY;
	wait_for_completion(&device->video_released);
	mutex_lock(&device->lock);
	mutex_unlock(&device->lock);
	v4l2_m2m_release(device->m2m);
	v4l2_device_unregister(&device->v4l2);
	put_device(device->dev);
	kfree(device);
	return 0;
}
