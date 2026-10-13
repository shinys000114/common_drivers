// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "s7d-codec-dos.h"
#include "s7d-h264-backend.h"
#include "s7d-h264-capture.h"
#include "s7d-h264-context.h"
#include "s7d-h264-dpb.h"
#include "s7d-h264-input.h"
#include "s7d-h264-mailbox.h"
#include "s7d-h264-picture-hw.h"
#include "s7d-h264-picture-snapshot.h"
#include "s7d-v4l2-device.h"
#include "s7d-vdec-runtime.h"

#define DOS_REG(word)		((word) * 4U)
#define H264_STATUS		DOS_REG(0x09d3)
#define H264_SLICE_DONE		0x01U
#define H264_PICTURE_DONE		0x02U
#define H264_WRRSP_REQUEST		0x13U
#define H264_WRRSP_DONE		0x14U
#define H264_SEI_READY		0x53U
#define H264_SEI_DONE		0x54U
#define H264_SEARCH_HEAD		0xf0U
#define H264_DECODE_SLICE		0xf1U
#define H264_CONFIG_DONE		0xf2U
#define H264_DECODE_NEWPICTURE	0xf3U
#define H264_EXTRA_SURFACES	2U
#define H264_TIMEOUT_MS		2000U

#define COMMAND_SUBMIT	BIT(0)
#define COMMAND_PICTURE	BIT(1)
#define COMMAND_DRAIN	BIT(2)
#define COMMAND_ERROR	BIT(3)

struct s7d_h264_event {
	u64 token;
	u32 status;
};

struct s7d_h264_backend {
	struct list_head node;
	struct s7d_h264_decoder *decoder;
	struct s7d_codec_session *session;
	struct s7d_v4l2_context *frontend;
	struct s7d_h264_context *context;
	struct s7d_h264_input *stream;
	struct s7d_h264_au_info au;
	struct s7d_h264_dpb *dpb;
	struct s7d_h264_mv_buffer mv;
	struct s7d_h264_sequence sequence;
	struct s7d_codec_layout layout;
	struct s7d_h264_capture_binding capture[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_h264_capture_descriptor descriptor[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_h264_picture_plan picture;
	struct s7d_h264_slice slice;
	struct s7d_h264_event irq_event;
	struct s7d_h264_event event;
	struct s7d_h264_mailbox_config config_packet;
	u8 slice_rpm[S7D_H264_MAILBOX_RPM_BYTES];
	struct s7d_v4l2_input input;
	struct s7d_v4l2_drain drain;
	struct work_struct protocol_work;
	struct work_struct executor_work;
	struct delayed_work timeout_work;
	struct mutex notifications; /* Joins frontend pointer users. */
	spinlock_t event_lock; /* Hard IRQ snapshot mailbox. */
	u64 generation;
	u64 last_submitted;
	u64 initial_token;
	u64 previous_input;
	u64 previous_write;
	u64 park_epoch;
	u32 config_scratch1;
	u32 captures;
	u32 pictures;
	unsigned long commands;
	unsigned long timeout_deadline;
	int failure;
	bool closing;
	bool dispatch_blocked;
	bool event_pending;
	bool input_pending;
	bool input_preflight;
	bool input_picture;
	bool input_staging;
	bool initial_complete;
	bool capture_blocked;
	bool hw_entered;
	bool hardware_started;
	bool sequence_valid;
	bool config_waiting;
	bool slice_waiting;
	bool drain_pending;
	bool drain_notified;
	bool timeout_armed;
};

struct s7d_h264_decoder {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_dos *dos;
	struct s7d_vdec_runtime *runtime;
	struct s7d_codec_core *core;
	struct regmap *map;
	struct mutex transaction; /* Shared with runtime input/DPB park proof. */
	struct mutex clients; /* Factory, shutdown and instance lifetime. */
	struct list_head instances;
	struct s7d_h264_backend *active;
	struct workqueue_struct *protocol_queue;
	struct workqueue_struct *executor_queue;
	struct s7d_v4l2_backend_factory factory;
	bool shutdown;
};

static int backend_check(struct s7d_h264_backend *backend)
{
	if (backend->closing || backend->decoder->shutdown)
		return -ESHUTDOWN;
	if (backend->failure)
		return backend->failure;
	return 0;
}

static void backend_command(struct s7d_h264_backend *backend, unsigned long command)
{
	lockdep_assert_held(&backend->decoder->transaction);
	if (backend->closing || backend->decoder->shutdown)
		return;
	backend->commands |= command;
	queue_work(backend->decoder->executor_queue, &backend->executor_work);
}

static void backend_error(struct s7d_h264_backend *backend, int error)
{
	lockdep_assert_held(&backend->decoder->transaction);
	if (!backend->failure) {
		backend->failure = error < 0 ? error : -EIO;
		dev_err(backend->decoder->dev,
			"H.264 err=%d token=%llu last=%#x hw=%d/%d cfg=%d slice=%d cap=%u\n",
			backend->failure, (unsigned long long)backend->input.token,
			backend->event.status, backend->hw_entered, backend->hardware_started,
			backend->config_waiting, backend->slice_waiting, backend->captures);
	}
	backend->dispatch_blocked = true;
	backend->timeout_armed = false;
	if (backend->dpb)
		s7d_h264_dpb_fail(backend->dpb, backend->failure);
	backend_command(backend, COMMAND_ERROR);
}

static int backend_status(struct s7d_h264_decoder *decoder, u32 expected)
{
	u32 status;
	int ret = regmap_read(decoder->map, H264_STATUS, &status);

	return ret ? ret : status == expected ? 0 : -ESTALE;
}

static void backend_timeout_arm(struct s7d_h264_backend *backend)
{
	lockdep_assert_held(&backend->decoder->transaction);
	backend->timeout_deadline = jiffies + msecs_to_jiffies(H264_TIMEOUT_MS);
	backend->timeout_armed = true;
	mod_delayed_work(backend->decoder->executor_queue, &backend->timeout_work,
			 msecs_to_jiffies(H264_TIMEOUT_MS));
}

static void backend_timeout_disarm(struct s7d_h264_backend *backend)
{
	lockdep_assert_held(&backend->decoder->transaction);
	backend->timeout_armed = false;
	cancel_delayed_work(&backend->timeout_work);
}

static int backend_ack(struct s7d_h264_backend *backend, u32 expected, u32 action)
{
	int ret = backend_check(backend);

	if (ret)
		return ret;
	if (backend->dispatch_blocked)
		return -ESHUTDOWN;
	ret = backend_status(backend->decoder, expected);
	if (ret)
		return ret;
	dma_wmb();
	ret = regmap_write(backend->decoder->map, H264_STATUS, action);
	if (!ret)
		backend_timeout_arm(backend);
	return ret;
}

static int backend_notify_sequence(struct s7d_h264_backend *backend)
{
	int ret = -ESHUTDOWN;

	mutex_lock(&backend->notifications);
	if (!READ_ONCE(backend->closing) && !READ_ONCE(backend->decoder->shutdown) &&
	    backend->frontend)
		ret = s7d_v4l2_sequence(backend->frontend, &backend->sequence.layout);
	mutex_unlock(&backend->notifications);
	return ret;
}

static int backend_notify_input(struct s7d_h264_backend *backend, u64 token, int status)
{
	int ret = -ESHUTDOWN;

	mutex_lock(&backend->notifications);
	if (!READ_ONCE(backend->closing) && !READ_ONCE(backend->decoder->shutdown) &&
	    backend->frontend)
		ret = s7d_v4l2_input_done(backend->frontend, token, status);
	mutex_unlock(&backend->notifications);
	return ret;
}

static int backend_notify_drain(struct s7d_h264_backend *backend)
{
	int ret = -ESHUTDOWN;

	mutex_lock(&backend->notifications);
	if (!READ_ONCE(backend->closing) && !READ_ONCE(backend->decoder->shutdown) &&
	    backend->frontend)
		ret = s7d_v4l2_drain_done(backend->frontend, backend->drain.epoch,
					  backend->drain.last_input_token);
	mutex_unlock(&backend->notifications);
	return ret;
}

static void backend_notify_error(struct s7d_h264_backend *backend, int error)
{
	mutex_lock(&backend->notifications);
	if (!READ_ONCE(backend->closing) && !READ_ONCE(backend->decoder->shutdown) &&
	    backend->frontend)
		s7d_v4l2_error(backend->frontend, error);
	mutex_unlock(&backend->notifications);
}

static void backend_normalize_sps(struct s7d_h264_sps *sps)
{
	if (!sps->bitstream_restriction) {
		sps->num_reorder_frames = 0;
		sps->max_dec_frame_buffering = 0;
	}
}

static bool backend_same_sps(const struct s7d_h264_sps *a,
			     const struct s7d_h264_sps *b, bool firmware)
{
	if (a->mb_width != b->mb_width || a->mb_height != b->mb_height ||
	    a->crop_left != b->crop_left || a->crop_right != b->crop_right ||
	    a->crop_top != b->crop_top || a->crop_bottom != b->crop_bottom ||
	    a->profile_idc != b->profile_idc || a->level_idc != b->level_idc ||
	    a->max_num_ref_frames != b->max_num_ref_frames ||
	    a->chroma_format_idc != b->chroma_format_idc ||
	    a->bit_depth_luma != b->bit_depth_luma ||
	    a->bit_depth_chroma != b->bit_depth_chroma ||
	    a->frame_mbs_only != b->frame_mbs_only ||
	    a->separate_colour_plane != b->separate_colour_plane ||
	    a->bitstream_restriction != b->bitstream_restriction)
		return false;
	if (a->bitstream_restriction &&
	    (a->num_reorder_frames != b->num_reorder_frames ||
	     a->max_dec_frame_buffering != b->max_dec_frame_buffering))
		return false;
	/* The selected RPM does not report colour or constraint_set3. */
	return firmware || (a->constraint_set3 == b->constraint_set3 &&
			    !memcmp(&a->colour, &b->colour, sizeof(a->colour)));
}

static int backend_set_host_sequence(struct s7d_h264_backend *backend,
				     const struct s7d_h264_sps *sps, bool *notify)
{
	struct s7d_h264_sequence sequence = { .sps = *sps };
	int ret;

	lockdep_assert_held(&backend->decoder->transaction);
	if ((sps->profile_idc == 66 || sps->profile_idc == 77) && sps->level_idc == 11)
		return -EOPNOTSUPP;
	backend_normalize_sps(&sequence.sps);
	ret = s7d_h264_build_layout(&sequence.sps, S7D_CODEC_CAPTURE_NV12M,
				    H264_EXTRA_SURFACES, &sequence.layout);
	if (ret)
		return ret;
	backend->sequence = sequence;
	backend->sequence_valid = true;
	*notify = true;
	return 0;
}

static int backend_reconcile_config(struct s7d_h264_backend *backend,
				    struct s7d_h264_sequence *sequence)
{
	struct s7d_codec_layout layout;
	int ret;

	if (!backend->sequence_valid || !backend->captures || !backend->input_preflight ||
	    !backend->input_picture)
		return -ESTALE;
	backend_normalize_sps(&sequence->sps);
	sequence->sps.colour = backend->sequence.sps.colour;
	if (!backend_same_sps(&backend->sequence.sps, &sequence->sps, true) ||
	    !backend_same_sps(&backend->sequence.sps, &backend->au.sps, false))
		return -EBADMSG;
	ret = s7d_h264_build_layout(&sequence->sps, backend->layout.format,
				    H264_EXTRA_SURFACES, &layout);
	if (ret)
		return ret;
	return memcmp(&layout, &backend->layout, sizeof(layout)) ? -EBADMSG : 0;
}

static int backend_capture_setup(struct s7d_h264_backend *backend)
{
	struct s7d_h264_decoder *decoder = backend->decoder;
	struct s7d_h264_capture_plane planes[2];
	u32 i, j, slots = max(1U, backend->sequence.sps.max_num_ref_frames) + 1;
	int ret;

	lockdep_assert_held(&decoder->transaction);
	ret = backend_status(decoder, S7D_H264_CONFIG_REQUEST);
	if (ret)
		return ret;
	if (!backend->mv.memory) {
		ret = s7d_h264_context_allocate_mv(backend->context, &backend->layout,
						 slots, &backend->mv);
		if (ret)
			return ret;
	}
	for (i = 0; i < backend->captures; i++) {
		for (j = 0; j < 2; j++) {
			ret = s7d_codec_surface_dma(backend->capture[i].surface, j,
						   &planes[j].dma, &planes[j].bytes);
			if (ret)
				return ret;
		}
		ret = s7d_h264_build_capture_descriptor(&backend->layout, i, planes,
						      &backend->descriptor[i]);
		if (ret)
			return ret;
		ret = s7d_h264_capture_program(decoder->map, &backend->layout, i, planes);
		if (ret)
			return ret;
	}
	ret = s7d_h264_capture_set_format(decoder->map, backend->layout.format);
	if (ret)
		return ret;
	if (!backend->dpb) {
		ret = s7d_h264_dpb_create(backend->session, &backend->sequence.sps,
					 &backend->layout, backend->capture,
					 backend->captures, slots, &backend->dpb);
		if (ret)
			return ret;
	}
	ret = s7d_h264_context_set_sequence(backend->context, decoder->map,
					    &backend->sequence.sps, &backend->layout,
					    backend->config_scratch1, slots,
					    backend->captures);
	if (ret)
		return ret;
	backend->config_waiting = false;
	return backend_ack(backend, S7D_H264_CONFIG_REQUEST, H264_CONFIG_DONE);
}

static int backend_slice_identity(struct s7d_h264_backend *backend)
{
	int ret = s7d_h264_input_get_au(backend->stream, backend->input.token, &backend->au);

	if (ret)
		return ret;
	if (backend->au.coded_width != backend->layout.coded_width ||
	    backend->au.coded_height != backend->layout.coded_height ||
	    backend->au.idr != backend->slice.idr ||
	    (backend->au.idr && backend->au.idr_pic_id != backend->slice.idr_pic_id))
		return -EBADMSG;
	return 0;
}

static int backend_slice(struct s7d_h264_backend *backend, bool retry)
{
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	if (!backend->dpb || !backend->captures || !backend->input_pending)
		return -ESTALE;
	ret = backend_status(decoder, H264_SLICE_DONE);
	if (ret)
		return ret;
	if (!retry) {
		ret = s7d_h264_mailbox_read_slice(decoder->map, backend->context,
						 backend->slice_rpm, sizeof(backend->slice_rpm));
		if (ret)
			return ret;
		ret = s7d_h264_parse_slice(backend->slice_rpm, sizeof(backend->slice_rpm),
					   &backend->slice);
		if (ret)
			return ret;
	}
	ret = backend_slice_identity(backend);
	if (ret)
		return ret;
	ret = s7d_h264_dpb_prepare(backend->dpb, &backend->slice, backend->input.token,
				  backend->input.timestamp_ns, &backend->picture);
	if (ret == -EAGAIN) {
		backend->slice_waiting = true;
		backend_timeout_disarm(backend);
		return 0;
	}
	if (ret)
		return ret;
	if (backend->picture.generation != backend->generation ||
	    backend->picture.slot >= backend->captures ||
	    backend->capture[backend->picture.slot].surface != backend->picture.surface ||
	    backend->capture[backend->picture.slot].surface_id != backend->picture.surface_id ||
	    backend->picture.input_token != backend->input.token)
		return -ESTALE;
	ret = s7d_h264_input_confirm_slice(backend->stream, backend->input.token,
					   &backend->slice, backend->picture.write_token);
	if (ret)
		return ret;
	ret = s7d_h264_picture_program(decoder->map, &backend->layout,
					&backend->picture, &backend->mv,
					&backend->descriptor[backend->picture.slot]);
	if (ret)
		return ret;
	ret = s7d_h264_context_save_slice(backend->context, decoder->map,
					  backend->input.token, backend->picture.write_token);
	if (ret)
		return ret;
	backend->slice_waiting = false;
	return backend_ack(backend, H264_SLICE_DONE, backend->picture.new_picture ?
			   H264_DECODE_NEWPICTURE : H264_DECODE_SLICE);
}

static bool backend_event_pop(struct s7d_h264_backend *backend)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&backend->event_lock, flags);
	pending = backend->event_pending;
	if (pending) {
		backend->event = backend->irq_event;
		backend->event_pending = false;
	}
	spin_unlock_irqrestore(&backend->event_lock, flags);
	return pending;
}

static void backend_protocol_work(struct work_struct *work)
{
	struct s7d_h264_backend *backend = container_of(work, struct s7d_h264_backend,
						     protocol_work);
	struct s7d_h264_decoder *decoder = backend->decoder;
	struct s7d_h264_sequence sequence;
	struct s7d_h264_config_snapshot *snapshot;
	int ret;

	for (;;) {
		mutex_lock(&decoder->transaction);
		if (backend_check(backend) || backend->dispatch_blocked) {
			mutex_unlock(&decoder->transaction);
			return;
		}
		if (backend->slice_waiting) {
			ret = backend_slice(backend, true);
			if (ret)
				backend_error(backend, ret);
			mutex_unlock(&decoder->transaction);
			return;
		}
		if (!backend_event_pop(backend)) {
			mutex_unlock(&decoder->transaction);
			return;
		}
		ret = !backend->input_pending || backend->event.token != backend->input.token ?
			-ESTALE : backend_status(decoder, backend->event.status);
		if (ret)
			goto processed;
		switch (backend->event.status) {
		case S7D_H264_CONFIG_REQUEST:
			ret = s7d_h264_mailbox_read_config(decoder->map, backend->context,
							 &backend->config_packet);
			if (ret)
				break;
			snapshot = &backend->config_packet.config;
			ret = s7d_h264_parse_config(snapshot, S7D_CODEC_CAPTURE_NV12M,
						    H264_EXTRA_SURFACES, &sequence);
			if (ret)
				break;
			ret = s7d_h264_input_get_au(backend->stream, backend->input.token,
						  &backend->au);
			if (ret)
				break;
			ret = backend_reconcile_config(backend, &sequence);
			if (ret)
				break;
			ret = s7d_h264_input_confirm_config(backend->stream, backend->input.token,
						    snapshot->scratch1);
			if (ret)
				break;
			backend->config_scratch1 = snapshot->scratch1;
			backend->config_waiting = true;
			backend_timeout_disarm(backend);
			if (backend->captures)
				ret = backend_capture_setup(backend);
			break;
		case H264_SLICE_DONE:
			ret = backend_slice(backend, false);
			break;
		case H264_PICTURE_DONE:
			if (!backend->dpb || backend->slice_waiting ||
			    backend->picture.input_token != backend->input.token) {
				ret = -ESTALE;
				break;
			}
			ret = s7d_h264_input_confirm_picture(backend->stream, backend->input.token,
							     backend->picture.write_token);
			if (!ret) {
				backend_timeout_disarm(backend);
				backend_command(backend, COMMAND_PICTURE);
			}
			break;
		case H264_WRRSP_REQUEST:
			ret = backend_ack(backend, H264_WRRSP_REQUEST, H264_WRRSP_DONE);
			break;
		case H264_SEI_READY:
			ret = backend_ack(backend, H264_SEI_READY, H264_SEI_DONE);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
processed:
		if (ret)
			backend_error(backend, ret);
		mutex_unlock(&decoder->transaction);
		if (ret)
			return;
	}
}

static int backend_stage(void *priv, struct s7d_codec_session *session,
			 struct regmap *map)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;
	int ret;

	mutex_lock(&decoder->transaction);
	backend = decoder->active;
	ret = !backend || backend->session != session || map != decoder->map ?
		-EXDEV : backend_check(backend);
	if (!ret && (!backend->input_pending || !backend->input_preflight ||
		     !backend->input_picture || !backend->captures))
		ret = -ESTALE;
	if (!ret) {
		backend->input_staging = true;
		ret = s7d_h264_input_prepare(backend->stream, backend->input.token);
	}
	if (!ret)
		ret = s7d_h264_input_get_au(backend->stream, backend->input.token,
					  &backend->au);
	if (!ret && backend->sequence_valid &&
	    !backend_same_sps(&backend->sequence.sps, &backend->au.sps, false))
		ret = -EOPNOTSUPP;
	if (!ret && !backend->hardware_started)
		ret = s7d_h264_context_begin(backend->context, backend->input.token);
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_configure(void *priv, struct regmap *map)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;
	int ret;

	mutex_lock(&decoder->transaction);
	backend = decoder->active;
	ret = !backend || map != decoder->map ? -EXDEV : backend_check(backend);
	if (!ret)
		ret = s7d_h264_context_initialize(backend->context, map);
	if (!ret)
		ret = s7d_h264_input_configure(backend->stream, backend->input.token);
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_enable_input(void *priv, struct regmap *map)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;
	int ret;

	mutex_lock(&decoder->transaction);
	backend = decoder->active;
	ret = !backend || map != decoder->map ? -EXDEV : backend_check(backend);
	if (!ret)
		ret = s7d_h264_input_enable(backend->stream, backend->input.token);
	if (!ret)
		WRITE_ONCE(backend->dispatch_blocked, false);
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_kick(void *priv, struct regmap *map)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;
	int ret;

	mutex_lock(&decoder->transaction);
	backend = decoder->active;
	ret = !backend || map != decoder->map ? -EXDEV : backend_check(backend);
	if (!ret)
		ret = regmap_write(map, H264_STATUS, H264_SEARCH_HEAD);
	if (!ret)
		backend_timeout_arm(backend);
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_irq_snapshot(void *priv, struct regmap *map)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend = READ_ONCE(decoder->active);
	unsigned long flags;
	u32 status;
	int ret;

	if (!backend || map != decoder->map || READ_ONCE(decoder->shutdown) ||
	    READ_ONCE(backend->closing) || READ_ONCE(backend->dispatch_blocked) ||
	    !READ_ONCE(backend->input_pending))
		return -ESHUTDOWN;
	spin_lock_irqsave(&backend->event_lock, flags);
	if (backend->event_pending) {
		ret = -EOVERFLOW;
		goto out;
	}
	ret = regmap_read(map, H264_STATUS, &status);
	if (ret)
		goto out;
	backend->irq_event.status = status;
	backend->irq_event.token = READ_ONCE(backend->input.token);
	ret = backend_status(decoder, status);
	if (!ret) {
		backend->event_pending = true;
		ret = 1;
	}
out:
	spin_unlock_irqrestore(&backend->event_lock, flags);
	return ret;
}

static int backend_irq_thread(void *priv, struct regmap *map, int error)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;

	mutex_lock(&decoder->transaction);
	backend = decoder->active;
	if (backend && map == decoder->map && !backend->closing && !decoder->shutdown) {
		if (error)
			backend_error(backend, error);
		else if (!backend->dispatch_blocked)
			queue_work(decoder->protocol_queue, &backend->protocol_work);
	}
	mutex_unlock(&decoder->transaction);
	return 0;
}

static void backend_block(void *priv)
{
	struct s7d_h264_decoder *decoder = priv;

	lockdep_assert_held(&decoder->transaction);
	if (decoder->active)
		WRITE_ONCE(decoder->active->dispatch_blocked, true);
}

static int backend_join_protocol(void *priv)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend = READ_ONCE(decoder->active);
	unsigned long flags;

	lockdep_assert_not_held(&decoder->transaction);
	if (!backend)
		return 0;
	cancel_work_sync(&backend->protocol_work);
	spin_lock_irqsave(&backend->event_lock, flags);
	backend->event_pending = false;
	spin_unlock_irqrestore(&backend->event_lock, flags);
	return 0;
}

static int backend_save(void *priv, struct regmap *map,
			struct s7d_codec_session *session, u64 input, u64 write)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;
	int ret;

	mutex_lock(&decoder->transaction);
	backend = decoder->active;
	ret = !backend || backend->session != session || map != decoder->map ?
		-EXDEV : backend_check(backend);
	if (!ret)
		ret = s7d_h264_context_save_completion(backend->context, map, input, write);
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_restore(void *priv, struct regmap *map,
			   struct s7d_codec_session *session, u64 input, u64 write,
			   u64 epoch, u64 next)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;
	int ret;

	mutex_lock(&decoder->transaction);
	backend = decoder->active;
	ret = !backend || backend->session != session || map != decoder->map ?
		-EXDEV : backend_check(backend);
	if (!ret && (next != backend->input.token || input != backend->previous_input ||
		     write != backend->previous_write || epoch != backend->park_epoch))
		ret = -ESTALE;
	if (!ret)
		ret = s7d_h264_context_restore(backend->context, map, input, write, epoch, next);
	if (!ret)
		ret = s7d_h264_input_configure(backend->stream, next);
	mutex_unlock(&decoder->transaction);
	return ret;
}

static const struct s7d_vdec_runtime_backend_ops runtime_ops = {
	.stage = backend_stage,
	.configure = backend_configure,
	.enable_input = backend_enable_input,
	.kick = backend_kick,
	.irq_snapshot = backend_irq_snapshot,
	.irq_thread = backend_irq_thread,
	.block = backend_block,
	.drain = backend_join_protocol,
	.save_context = backend_save,
	.restore_context = backend_restore,
};

static int backend_run_submit(struct s7d_h264_backend *backend)
{
	struct s7d_h264_decoder *decoder = backend->decoder;
	struct s7d_h264_input_return returned;
	struct s7d_h264_preflight preflight;
	u64 token;
	bool notify = false, headers = false, start = false, resume = false;
	int ret;

	lockdep_assert_not_held(&decoder->transaction);
	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret && backend->capture_blocked) {
		mutex_unlock(&decoder->transaction);
		return 0;
	}
	token = backend->input.token;
	if (!ret && !backend->input_pending)
		ret = -ESTALE;
	if (!ret && !backend->input_preflight) {
		ret = s7d_h264_input_preflight(backend->stream, &backend->input, &preflight);
		if (!ret) {
			backend->input_preflight = true;
			backend->input_picture = preflight.kind == S7D_H264_INPUT_PICTURE;
		}
		if (!ret && preflight.sequence_ready && !backend->sequence_valid)
			ret = backend_set_host_sequence(backend, &preflight.sps, &notify);
		if (!ret && backend->input_picture) {
			ret = s7d_h264_input_get_au(backend->stream, token, &backend->au);
			if (!ret && (!backend->sequence_valid ||
				     !backend_same_sps(&backend->sequence.sps,
						       &backend->au.sps, false)))
				ret = -EOPNOTSUPP;
		} else if (!ret && preflight.kind == S7D_H264_INPUT_HEADERS) {
			ret = s7d_h264_input_accept_headers(backend->stream, token, &returned);
			if (!ret && (returned.token != token || returned.status))
				ret = -ESTALE;
			if (!ret) {
				backend->input_pending = false;
				backend->input_preflight = false;
				headers = true;
			}
		} else if (!ret) {
			ret = -EINVAL;
		}
	}
	if (!ret && !headers && backend->captures) {
		if (!backend->input_picture || !backend->sequence_valid) {
			ret = -ESTALE;
		} else {
			resume = backend->hardware_started;
			backend->hw_entered = true;
			start = true;
		}
	}
	mutex_unlock(&decoder->transaction);
	if (ret)
		return ret;
	if (notify) {
		ret = backend_notify_sequence(backend);
		if (ret)
			return ret;
	}
	if (headers)
		return backend_notify_input(backend, token, 0);
	if (!start)
		return 0;
	if (resume)
		return s7d_vdec_runtime_resume(decoder->runtime, backend->session,
					       backend->previous_input, backend->previous_write,
					       backend->park_epoch, backend->input.token);
	ret = s7d_vdec_runtime_prepare(decoder->runtime, backend->session);
	if (!ret)
		ret = s7d_vdec_runtime_start(decoder->runtime);
	if (!ret) {
		mutex_lock(&decoder->transaction);
		backend->hardware_started = true;
		mutex_unlock(&decoder->transaction);
	}
	return ret;
}

static int backend_complete_picture(struct s7d_h264_backend *backend)
{
	struct s7d_h264_decoder *decoder = backend->decoder;
	struct s7d_h264_input_observation observation;
	struct s7d_h264_input_return returned;
	struct s7d_h264_dpb_state state;
	u64 input, write, epoch;
	u32 slot;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	input = backend->picture.input_token;
	write = backend->picture.write_token;
	slot = backend->picture.slot;
	if (!ret)
		ret = s7d_h264_input_get_picture(backend->stream, backend->session,
						 input, write, &observation);
	if (!ret)
		ret = s7d_h264_dpb_get_state(backend->dpb, backend->session, &state);
	if (!ret && (state.failure || state.active_slot < 0 || (u32)state.active_slot != slot ||
		     state.generation != backend->generation ||
		     state.surface_id != backend->picture.surface_id ||
		     state.write_token != write || state.input_token != input))
		ret = state.failure ? state.failure : -ESTALE;
	mutex_unlock(&decoder->transaction);
	if (ret)
		return ret;
	ret = s7d_vdec_runtime_park(decoder->runtime, backend->session, backend->stream,
				    backend->dpb, input, write, &epoch);
	if (ret)
		return ret;
	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret)
		ret = s7d_h264_dpb_picture_done(backend->dpb, slot, write, 0);
	if (!ret) {
		backend->pictures++;
		ret = s7d_h264_context_confirm_park(backend->context, decoder->runtime,
						  input, write, epoch, backend->pictures);
	}
	if (!ret)
		ret = s7d_h264_input_return_after_park(backend->stream, decoder->runtime,
						     input, write, epoch, &returned);
	if (!ret && (returned.token != input || returned.status))
		ret = -ESTALE;
	if (!ret) {
		backend->previous_input = input;
		backend->previous_write = write;
		backend->park_epoch = epoch;
		backend->input_pending = false;
		backend->input_preflight = false;
		backend->input_picture = false;
		backend->input_staging = false;
	}
	mutex_unlock(&decoder->transaction);
	return ret ? ret : backend_notify_input(backend, input, 0);
}

static int backend_complete_drain(struct s7d_h264_backend *backend)
{
	struct s7d_h264_decoder *decoder = backend->decoder;
	struct s7d_h264_dpb_state state;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret && (!backend->drain_pending || backend->input_pending ||
		     !backend->sequence_valid || !backend->captures))
		ret = -ESTALE;
	if (!ret && !backend->hw_entered) {
		if (backend->hardware_started || backend->input_staging ||
		    backend->input_preflight ||
		    backend->dpb || backend->pictures || backend->previous_input ||
		    backend->previous_write || backend->park_epoch || backend->config_waiting ||
		    backend->slice_waiting || backend->timeout_armed || !backend->dispatch_blocked)
			ret = -ESTALE;
		else
			ret = s7d_h264_input_check_header_drain(backend->stream, &backend->drain);
	} else if (!ret && (!backend->dpb || !backend->park_epoch)) {
		ret = -ESTALE;
	} else if (!ret) {
		ret = s7d_h264_input_check_drain(backend->stream, decoder->runtime,
						 &backend->drain);
		if (!ret)
			ret = s7d_h264_dpb_flush_output(backend->dpb);
		if (!ret)
			ret = s7d_h264_dpb_get_state(backend->dpb, backend->session, &state);
		if (!ret && (state.failure || state.active_slot >= 0 ||
			     state.complete_unpublished ||
			     state.generation != backend->generation ||
			     state.reference_count > backend->sequence.sps.max_num_ref_frames))
			ret = state.failure ? state.failure : -ESTALE;
	}
	if (!ret)
		backend->drain_notified = true;
	mutex_unlock(&decoder->transaction);
	if (ret)
		return ret;
	return backend_notify_drain(backend);
}

static int backend_return_input(struct s7d_h264_backend *backend, int status)
{
	struct s7d_h264_input_return returned;
	int ret;

	lockdep_assert_held(&backend->decoder->transaction);
	if (!backend->input_preflight)
		return 0;
	if (backend->input_staging) {
		ret = s7d_h264_input_return_after_quiesce(backend->stream,
							backend->decoder->runtime,
							backend->input.token, status, &returned);
		if (ret != -ESTALE)
			goto returned;
	}
	ret = s7d_h264_input_cancel_preflight(backend->stream, backend->input.token,
					    status, &returned);
returned:
	if (!ret && (returned.token != backend->input.token || returned.status != status))
		ret = -ESTALE;
	if (!ret) {
		backend->input_preflight = false;
		backend->input_picture = false;
		backend->input_staging = false;
	}
	return ret;
}

static void backend_stop_error(struct s7d_h264_backend *backend)
{
	struct s7d_h264_decoder *decoder = backend->decoder;
	u64 token;
	int ret, status;

	mutex_lock(&decoder->transaction);
	status = backend->failure ? backend->failure : -EIO;
	token = backend->input.token;
	mutex_unlock(&decoder->transaction);
	backend_notify_error(backend, status);
	ret = s7d_codec_session_stop(backend->session);
	if (ret) {
		dev_err(decoder->dev, "H.264 stop failed: %d; backing retained\n", ret);
		return;
	}
	mutex_lock(&decoder->transaction);
	ret = backend_return_input(backend, status);
	if (ret) {
		mutex_unlock(&decoder->transaction);
		return;
	}
	backend->input_pending = false;
	backend->input_staging = false;
	mutex_unlock(&decoder->transaction);
	if (token)
		backend_notify_input(backend, token, status);
}

static void backend_executor_work(struct work_struct *work)
{
	struct s7d_h264_backend *backend = container_of(work, struct s7d_h264_backend,
						     executor_work);
	struct s7d_h264_decoder *decoder = backend->decoder;
	unsigned long command;
	int ret;

	for (;;) {
		mutex_lock(&decoder->transaction);
		if (backend->closing || decoder->shutdown || !backend->commands) {
			mutex_unlock(&decoder->transaction);
			return;
		}
		if (backend->commands & COMMAND_ERROR) {
			backend->commands = 0;
			mutex_unlock(&decoder->transaction);
			backend_stop_error(backend);
			return;
		}
		command = backend->commands & -backend->commands;
		backend->commands &= ~command;
		mutex_unlock(&decoder->transaction);
		if (command == COMMAND_SUBMIT)
			ret = backend_run_submit(backend);
		else if (command == COMMAND_PICTURE)
			ret = backend_complete_picture(backend);
		else if (command == COMMAND_DRAIN)
			ret = backend_complete_drain(backend);
		else
			ret = -EINVAL;
		if (ret) {
			mutex_lock(&decoder->transaction);
			backend_error(backend, ret);
			mutex_unlock(&decoder->transaction);
		} else if (command == COMMAND_SUBMIT) {
			mutex_lock(&decoder->transaction);
			if (backend->input.token == backend->initial_token &&
			    backend->last_submitted == backend->initial_token &&
			    !backend->captures && !backend->hw_entered)
				backend->initial_complete = true;
			mutex_unlock(&decoder->transaction);
		}
	}
}

static void backend_timeout_work(struct work_struct *work)
{
	struct s7d_h264_backend *backend = container_of(to_delayed_work(work),
						     struct s7d_h264_backend, timeout_work);
	struct s7d_h264_decoder *decoder = backend->decoder;

	mutex_lock(&decoder->transaction);
	if (!backend_check(backend) && backend->timeout_armed && backend->input_pending &&
	    !backend->dispatch_blocked && !backend->config_waiting && !backend->slice_waiting) {
		if (time_before(jiffies, backend->timeout_deadline))
			mod_delayed_work(decoder->executor_queue, &backend->timeout_work,
					 backend->timeout_deadline - jiffies);
		else
			backend_error(backend, -ETIMEDOUT);
	}
	mutex_unlock(&decoder->transaction);
}

static int backend_prepare(void *priv, struct s7d_codec_session *session,
			   struct s7d_v4l2_context *frontend)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret && (backend->session || decoder->active || !session || !frontend))
		ret = -EBUSY;
	if (ret)
		goto out;
	s7d_codec_session_get(session);
	backend->session = session;
	backend->frontend = frontend;
	WRITE_ONCE(decoder->active, backend);
	backend->context = s7d_h264_context_create(session);
	if (IS_ERR(backend->context)) {
		ret = PTR_ERR(backend->context);
		backend->context = NULL;
		goto failed;
	}
	backend->stream = s7d_h264_input_create(decoder->dos, session);
	if (IS_ERR(backend->stream)) {
		ret = PTR_ERR(backend->stream);
		backend->stream = NULL;
		goto failed;
	}
	goto out;
failed:
	backend->failure = ret;
out:
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_submit(void *priv, const struct s7d_v4l2_input *input)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret && (!input || !input->memory || !input->bytes || !input->token ||
		     !backend->session || !backend->stream || !backend->context ||
		     decoder->active != backend))
		ret = -EINVAL;
	if (!ret && (backend->input_pending || backend->drain_pending ||
		     input->token <= backend->last_submitted))
		ret = -EBUSY;
	if (!ret) {
		backend->input = *input;
		if (!backend->initial_token)
			backend->initial_token = input->token;
		backend->last_submitted = input->token;
		backend->input_pending = true;
		backend_command(backend, COMMAND_SUBMIT);
	}
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_initial_check(struct s7d_h264_backend *backend, u64 token)
{
	int ret = backend_check(backend);

	lockdep_assert_held(&backend->decoder->transaction);
	if (ret)
		return ret;
	if (!token || token != backend->initial_token || token != backend->last_submitted ||
	    token != backend->input.token || !backend->session ||
	    backend->decoder->active != backend)
		return -ESTALE;
	if (backend->captures || backend->hw_entered || backend->hardware_started)
		return -EBUSY;
	return 0;
}

static int backend_sync_initial(void *priv, u64 token)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	lockdep_assert_not_held(&decoder->transaction);
	lockdep_assert_not_held(&backend->notifications);
	mutex_lock(&decoder->transaction);
	ret = backend_initial_check(backend, token);
	mutex_unlock(&decoder->transaction);
	if (ret)
		return ret;
	flush_work(&backend->executor_work);
	mutex_lock(&decoder->transaction);
	ret = backend_initial_check(backend, token);
	if (!ret && !backend->initial_complete)
		ret = -EAGAIN;
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_capture_start(void *priv, const struct s7d_codec_layout *layout,
				 u64 generation,
				 const struct s7d_v4l2_capture_buffer *buffers, unsigned int count)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	struct s7d_h264_capture_plane planes[2];
	struct s7d_codec_layout checked;
	u32 i, j;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret && (!layout || !buffers || !generation ||
		     !backend->sequence_valid || backend->captures ||
		     count > S7D_CODEC_MAX_CAPTURE_BUFFERS))
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = s7d_h264_build_layout(&backend->sequence.sps, layout->format,
				     H264_EXTRA_SURFACES, &checked);
	if (ret)
		goto out;
	if (memcmp(&checked, layout, sizeof(checked)) || count < checked.min_capture_buffers) {
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i < count; i++) {
		if (!buffers[i].surface || buffers[i].generation != generation ||
		    buffers[i].surface_id >= S7D_CODEC_MAX_CAPTURE_BUFFERS) {
			ret = -EINVAL;
			goto out;
		}
		for (j = 0; j < i; j++)
			if (buffers[i].surface == buffers[j].surface ||
			    buffers[i].surface_id == buffers[j].surface_id) {
				ret = -EINVAL;
				goto out;
			}
		for (j = 0; j < 2; j++) {
			ret = s7d_codec_surface_dma(buffers[i].surface, j,
						   &planes[j].dma, &planes[j].bytes);
			if (ret)
				goto out;
		}
		ret = s7d_h264_build_capture_descriptor(&checked, i, planes,
						      &backend->descriptor[i]);
		if (ret)
			goto out;
	}
	for (i = 0; i < count; i++)
		backend->capture[i] = (struct s7d_h264_capture_binding) {
			.surface = buffers[i].surface,
			.surface_id = buffers[i].surface_id,
			.generation = generation,
		};
	backend->captures = count;
	backend->generation = generation;
	backend->layout = checked;
	backend->capture_blocked = false;
	if (backend->config_waiting) {
		ret = backend_capture_setup(backend);
		if (ret)
			backend_error(backend, ret);
	} else if (backend->input_pending) {
		backend_command(backend, COMMAND_SUBMIT);
	}
out:
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_capture_stop_check(struct s7d_h264_backend *backend, u64 generation)
{
	int ret = backend_check(backend);

	lockdep_assert_held(&backend->decoder->transaction);
	if (ret)
		return ret;
	if (!generation || !backend->session || !backend->stream ||
	    !backend->context || backend->decoder->active != backend)
		return -ESTALE;
	if (backend->hw_entered || backend->hardware_started || backend->dpb ||
	    backend->mv.memory || backend->pictures || backend->config_waiting ||
	    backend->slice_waiting || backend->drain_pending || backend->timeout_armed ||
	    backend->previous_input || backend->previous_write || backend->park_epoch ||
	    (backend->commands & ~COMMAND_SUBMIT))
		return -EOPNOTSUPP;
	if (backend->captures && backend->generation != generation)
		return -ESTALE;
	return 0;
}

static int backend_capture_stop(void *priv, u64 generation)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	lockdep_assert_not_held(&decoder->transaction);
	lockdep_assert_not_held(&backend->notifications);
	mutex_lock(&decoder->transaction);
	ret = backend_capture_stop_check(backend, generation);
	if (!ret)
		backend->capture_blocked = true;
	mutex_unlock(&decoder->transaction);
	if (ret)
		return ret;
	cancel_delayed_work_sync(&backend->timeout_work);
	cancel_work_sync(&backend->protocol_work);
	cancel_work_sync(&backend->executor_work);
	ret = s7d_vdec_runtime_join_unprepared(decoder->runtime);
	if (ret)
		return ret;
	mutex_lock(&backend->notifications);
	mutex_unlock(&backend->notifications);
	mutex_lock(&decoder->transaction);
	ret = backend_capture_stop_check(backend, generation);
	if (!ret) {
		memset(backend->capture, 0, sizeof(backend->capture));
		memset(backend->descriptor, 0, sizeof(backend->descriptor));
		backend->captures = 0;
		backend->generation = 0;
	}
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_capture_queue(void *priv, struct s7d_codec_surface *surface,
				 u64 surface_id, u64 generation)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	u32 i;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (ret)
		goto out;
	for (i = 0; i < backend->captures; i++)
		if (backend->capture[i].surface == surface &&
		    backend->capture[i].surface_id == surface_id &&
		    backend->capture[i].generation == generation)
			break;
	if (i == backend->captures) {
		ret = -ESTALE;
		goto out;
	}
	if (backend->slice_waiting && !backend->dispatch_blocked)
		queue_work(decoder->protocol_queue, &backend->protocol_work);
out:
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_drain(void *priv, const struct s7d_v4l2_drain *request)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret && (!request || !backend->stream || !backend->sequence_valid ||
		     !backend->captures ||
		     backend->drain_pending || backend->input_pending))
		ret = -EBUSY;
	if (!ret)
		ret = s7d_h264_input_begin_drain(backend->stream, request);
	if (!ret) {
		backend->drain = *request;
		backend->drain_pending = true;
		backend->drain_notified = false;
		backend_command(backend, COMMAND_DRAIN);
	}
	mutex_unlock(&decoder->transaction);
	return ret;
}

static int backend_resume(void *priv)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	mutex_lock(&decoder->transaction);
	ret = backend_check(backend);
	if (!ret && (!backend->drain_pending || !backend->drain_notified))
		ret = -ESTALE;
	if (!ret)
		ret = s7d_h264_input_end_drain(backend->stream, &backend->drain);
	if (!ret) {
		backend->drain_pending = false;
		backend->drain_notified = false;
	}
	mutex_unlock(&decoder->transaction);
	return ret;
}

static void backend_detach(void *priv)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;

	mutex_lock(&decoder->transaction);
	WRITE_ONCE(backend->closing, true);
	WRITE_ONCE(backend->dispatch_blocked, true);
	backend->timeout_armed = false;
	backend->commands = 0;
	if (decoder->active == backend && backend->session)
		s7d_vdec_runtime_request_preload_stop(decoder->runtime, backend->session);
	mutex_unlock(&decoder->transaction);
	cancel_delayed_work_sync(&backend->timeout_work);
	cancel_work_sync(&backend->protocol_work);
	cancel_work_sync(&backend->executor_work);
	mutex_lock(&backend->notifications);
	backend->frontend = NULL;
	mutex_unlock(&backend->notifications);
}

static int backend_release_epoch(struct s7d_h264_backend *backend)
{
	struct s7d_h264_decoder *decoder = backend->decoder;
	struct s7d_h264_context *context;
	struct s7d_codec_session *session;
	int ret;

	lockdep_assert_held(&decoder->clients);
	if (backend->session) {
		ret = s7d_codec_session_stop(backend->session);
		if (ret)
			return ret;
	}
	mutex_lock(&decoder->transaction);
	ret = backend_return_input(backend, -ECANCELED);
	mutex_unlock(&decoder->transaction);
	if (ret)
		return ret;
	if (backend->dpb) {
		ret = s7d_h264_dpb_close(backend->dpb);
		if (ret)
			return ret;
		backend->dpb = NULL;
	}
	if (backend->stream) {
		ret = s7d_h264_input_destroy(backend->stream);
		if (ret)
			return ret;
		backend->stream = NULL;
	}
	mutex_lock(&decoder->transaction);
	if (decoder->active == backend)
		WRITE_ONCE(decoder->active, NULL);
	mutex_unlock(&decoder->transaction);
	context = backend->context;
	backend->context = NULL;
	if (context)
		s7d_h264_context_release(context);
	session = backend->session;
	backend->session = NULL;
	if (session)
		s7d_codec_session_put(session);
	return 0;
}

static int backend_reset(void *priv)
{
	struct s7d_h264_backend *backend = priv;
	struct s7d_h264_decoder *decoder = backend->decoder;
	int ret;

	mutex_lock(&decoder->clients);
	if (decoder->shutdown || !READ_ONCE(backend->closing)) {
		ret = decoder->shutdown ? -ESHUTDOWN : -EBUSY;
		goto out;
	}
	ret = backend_release_epoch(backend);
	if (ret)
		goto out;
	mutex_lock(&decoder->transaction);
	backend->frontend = NULL;
	memset(&backend->au, 0, sizeof(backend->au));
	memset(&backend->mv, 0, sizeof(backend->mv));
	memset(&backend->sequence, 0, sizeof(backend->sequence));
	memset(&backend->layout, 0, sizeof(backend->layout));
	memset(backend->capture, 0, sizeof(backend->capture));
	memset(backend->descriptor, 0, sizeof(backend->descriptor));
	memset(&backend->picture, 0, sizeof(backend->picture));
	memset(&backend->slice, 0, sizeof(backend->slice));
	memset(&backend->irq_event, 0, sizeof(backend->irq_event));
	memset(&backend->event, 0, sizeof(backend->event));
	memset(&backend->config_packet, 0, sizeof(backend->config_packet));
	memset(backend->slice_rpm, 0, sizeof(backend->slice_rpm));
	memset(&backend->input, 0, sizeof(backend->input));
	memset(&backend->drain, 0, sizeof(backend->drain));
	backend->generation = 0;
	backend->last_submitted = 0;
	backend->initial_token = 0;
	backend->previous_input = 0;
	backend->previous_write = 0;
	backend->park_epoch = 0;
	backend->config_scratch1 = 0;
	backend->captures = 0;
	backend->pictures = 0;
	backend->commands = 0;
	backend->timeout_deadline = 0;
	backend->failure = 0;
	backend->dispatch_blocked = true;
	backend->event_pending = false;
	backend->input_pending = false;
	backend->input_preflight = false;
	backend->input_picture = false;
	backend->input_staging = false;
	backend->initial_complete = false;
	backend->capture_blocked = false;
	backend->hw_entered = false;
	backend->hardware_started = false;
	backend->sequence_valid = false;
	backend->config_waiting = false;
	backend->slice_waiting = false;
	backend->drain_pending = false;
	backend->drain_notified = false;
	backend->timeout_armed = false;
	WRITE_ONCE(backend->closing, false);
	mutex_unlock(&decoder->transaction);
out:
	mutex_unlock(&decoder->clients);
	return ret;
}

static const struct s7d_v4l2_backend_ops frontend_ops = {
	.prepare = backend_prepare,
	.submit = backend_submit,
	.sync_initial = backend_sync_initial,
	.capture_start = backend_capture_start,
	.capture_stop = backend_capture_stop,
	.capture_queue = backend_capture_queue,
	.drain = backend_drain,
	.resume = backend_resume,
	.detach = backend_detach,
	.reset = backend_reset,
};

static int backend_factory_ready(void *priv)
{
	struct s7d_h264_decoder *decoder = priv;
	int ret;

	mutex_lock(&decoder->clients);
	ret = decoder->shutdown || !decoder->core || !decoder->runtime ? -ESHUTDOWN : 0;
	mutex_unlock(&decoder->clients);
	return ret;
}

static void *backend_factory_create(void *priv)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend;

	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&decoder->clients);
	if (decoder->shutdown || !try_module_get(decoder->owner)) {
		mutex_unlock(&decoder->clients);
		kfree(backend);
		return ERR_PTR(-ESHUTDOWN);
	}
	backend->decoder = decoder;
	backend->dispatch_blocked = true;
	mutex_init(&backend->notifications);
	spin_lock_init(&backend->event_lock);
	INIT_WORK(&backend->protocol_work, backend_protocol_work);
	INIT_WORK(&backend->executor_work, backend_executor_work);
	INIT_DELAYED_WORK(&backend->timeout_work, backend_timeout_work);
	list_add_tail(&backend->node, &decoder->instances);
	mutex_unlock(&decoder->clients);
	return backend;
}

static void backend_factory_destroy(void *priv, void *instance)
{
	struct s7d_h264_decoder *decoder = priv;
	struct s7d_h264_backend *backend = instance;
	int ret;

	mutex_lock(&decoder->clients);
	backend_detach(backend);
	ret = backend_release_epoch(backend);
	if (ret)
		goto retained;
	list_del(&backend->node);
	mutex_unlock(&decoder->clients);
	module_put(decoder->owner);
	kfree(backend);
	return;
retained:
	dev_err(decoder->dev, "H.264 instance release failed: %d; references retained\n", ret);
	mutex_unlock(&decoder->clients);
}

struct s7d_h264_decoder *
s7d_h264_decoder_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			struct module *owner)
{
	struct s7d_h264_decoder *decoder;
	int ret;

	if (!dev || IS_ERR_OR_NULL(borrowed_dos) || IS_ERR_OR_NULL(borrowed_firmware))
		return ERR_PTR(-EINVAL);
#ifdef MODULE
	if (owner != THIS_MODULE)
		return ERR_PTR(-EINVAL);
#endif
	decoder = kzalloc(sizeof(*decoder), GFP_KERNEL);
	if (!decoder)
		return ERR_PTR(-ENOMEM);
	decoder->dev = get_device(dev);
	decoder->owner = owner;
	decoder->dos = borrowed_dos;
	decoder->map = s7d_codec_dos_regmap(borrowed_dos, S7D_CODEC_ENGINE_VDEC);
	if (IS_ERR_OR_NULL(decoder->map)) {
		ret = decoder->map ? PTR_ERR(decoder->map) : -ENODEV;
		goto free;
	}
	mutex_init(&decoder->transaction);
	mutex_init(&decoder->clients);
	INIT_LIST_HEAD(&decoder->instances);
	decoder->protocol_queue = alloc_ordered_workqueue("s7d-h264-protocol", WQ_MEM_RECLAIM);
	if (!decoder->protocol_queue) {
		ret = -ENOMEM;
		goto free;
	}
	decoder->executor_queue = alloc_ordered_workqueue("s7d-h264-executor", WQ_MEM_RECLAIM);
	if (!decoder->executor_queue) {
		ret = -ENOMEM;
		goto destroy_protocol;
	}
	decoder->runtime = s7d_vdec_runtime_create(dev, owner, borrowed_dos,
						 borrowed_firmware, &runtime_ops,
						 decoder, &decoder->transaction);
	if (IS_ERR(decoder->runtime)) {
		ret = PTR_ERR(decoder->runtime);
		goto destroy_executor;
	}
	decoder->core = s7d_codec_core_create(dev, owner, s7d_vdec_runtime_resource_ops(),
					     decoder->runtime, 32);
	if (IS_ERR(decoder->core)) {
		ret = PTR_ERR(decoder->core);
		s7d_vdec_runtime_destroy(decoder->runtime);
		goto destroy_executor;
	}
	decoder->factory = (struct s7d_v4l2_backend_factory) {
		.owner = owner,
		.codec = {
			.engine = S7D_CODEC_ENGINE_VDEC,
			.output_format = V4L2_PIX_FMT_H264,
			.max_width = 4096, .max_height = 2304,
			.max_input_size = 16U * 1024U * 1024U, .nv21 = true,
		},
		.ops = &frontend_ops,
		.ready = backend_factory_ready,
		.create = backend_factory_create,
		.destroy = backend_factory_destroy,
	};
	return decoder;
destroy_executor:
	destroy_workqueue(decoder->executor_queue);
destroy_protocol:
	destroy_workqueue(decoder->protocol_queue);
free:
	put_device(decoder->dev);
	kfree(decoder);
	return ERR_PTR(ret);
}

struct s7d_codec_core *s7d_h264_decoder_core(struct s7d_h264_decoder *decoder)
{
	return decoder->core;
}

const struct s7d_v4l2_backend_factory *
s7d_h264_decoder_factory(struct s7d_h264_decoder *decoder)
{
	return &decoder->factory;
}

int s7d_h264_decoder_suspend_check(struct s7d_h264_decoder *decoder)
{
	int ret;

	mutex_lock(&decoder->clients);
	ret = decoder->active ? -EBUSY : s7d_vdec_runtime_suspend_check(decoder->runtime);
	mutex_unlock(&decoder->clients);
	return ret;
}

int s7d_h264_decoder_shutdown(struct s7d_h264_decoder *decoder)
{
	struct s7d_h264_backend *backend;
	int ret = 0, stopped;

	mutex_lock(&decoder->clients);
	WRITE_ONCE(decoder->shutdown, true);
	list_for_each_entry(backend, &decoder->instances, node) {
		backend_detach(backend);
		if (!backend->session)
			continue;
		stopped = s7d_codec_session_stop(backend->session);
		if (stopped && !ret)
			ret = stopped;
	}
	mutex_unlock(&decoder->clients);
	return ret;
}

int s7d_h264_decoder_destroy(struct s7d_h264_decoder *decoder)
{
	int ret;

	mutex_lock(&decoder->clients);
	if (!list_empty(&decoder->instances)) {
		mutex_unlock(&decoder->clients);
		return -EBUSY;
	}
	WRITE_ONCE(decoder->shutdown, true);
	if (decoder->core) {
		ret = s7d_codec_core_destroy(decoder->core);
		if (ret)
			goto retained;
		decoder->core = NULL;
	}
	ret = s7d_vdec_runtime_destroy(decoder->runtime);
	if (ret)
		goto retained;
	mutex_unlock(&decoder->clients);
	destroy_workqueue(decoder->executor_queue);
	destroy_workqueue(decoder->protocol_queue);
	put_device(decoder->dev);
	kfree(decoder);
	return 0;
retained:
	mutex_unlock(&decoder->clients);
	return ret;
}
