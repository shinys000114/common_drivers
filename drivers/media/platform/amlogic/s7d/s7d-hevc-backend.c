// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "s7d-codec-dos.h"
#include "s7d-codec-firmware.h"
#include "s7d-hevc-backend.h"
#include "s7d-hevc-context.h"
#include "s7d-hevc-dpb.h"
#include "s7d-hevc-input.h"
#include "s7d-v4l2.h"
#include "s7d-hevc-picture.h"
#include "s7d-hevc-rpm.h"
#include "s7d-hevc-sequence.h"

#define HEVC_REG(word) ((word) * 4U)
#define HEVC_MAILBOX_CLEAR HEVC_REG(0x3071)
#define HEVC_STATUS HEVC_REG(0x30c0)
#define HEVC_DECODE_INFO HEVC_REG(0x30c1)
#define HEVC_SWAP_TOKEN HEVC_REG(0x30c8)
#define HEVC_WAIT_FLAG HEVC_REG(0x30ce)
#define HEVC_COUNT HEVC_REG(0x30d0)
#define HEVC_MODE2 HEVC_REG(0x30d1)
#define HEVC_NAL_SEARCH HEVC_REG(0x30d2)
#define HEVC_MODE HEVC_REG(0x30d3)
#define HEVC_STOP_POSITION HEVC_REG(0x30d4)
#define HEVC_AUX_ADDRESS HEVC_REG(0x30d5)
#define HEVC_AUX_SIZE HEVC_REG(0x30d6)
#define HEVC_DECODE_SIZE HEVC_REG(0x30d7)
#define HEVC_SHIFT_STATUS HEVC_REG(0x310b)
#define HEVC_SHIFT_BYTES HEVC_REG(0x310d)
#define HEVC_LCU_START HEVC_REG(0x3124)
#define HEVC_CPU_INTERRUPT HEVC_REG(0x3305)
#define HEVC_LMEM_DMA_CONTROL HEVC_REG(0x3350)
#define HEVC_PSCALE_CONTROL HEVC_REG(0x3911)
#define HEVC_TIMEOUT_MS 2000U

struct s7d_hevc_irq_snapshot {
	u32 status;
	u32 lcu;
	u32 shift_status;
	u32 shift_bytes;
	u32 decode_info;
	int error;
};

struct s7d_hevc_job {
	struct s7d_codec_session *session;
	struct s7d_hevc_context *context;
	struct s7d_hevc_input_plan input;
	struct s7d_hevc_slice_info host_slice;
	struct s7d_hevc_slice_info slice;
	struct s7d_hevc_rpm rpm;
	struct s7d_hevc_geometry geometry;
	struct s7d_hevc_picture_plan picture;
	struct s7d_hevc_picture_metadata metadata;
	struct s7d_hevc_irq_snapshot snapshot;
	struct s7d_hevc_decode_result result;
	struct completion wake;
	u64 lease_epoch;
	u64 firmware_epoch;
	u64 write_token;
	u32 opaque_swap;
	u32 headers;
	u32 current_slot;
	int status;
	int irq_error;
	bool snapshot_valid;
	bool finished_cancelled;
	bool core_shutdown;
	bool rpm_valid;
	bool header_armed;
	bool header_waiting;
	bool picture_complete;
	bool acquired;
	bool dpb_begun;
	bool context_begun;
	bool finished;
};

struct s7d_hevc_decoder {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_dos *dos;
	struct s7d_codec_core *core;
	struct s7d_codec_firmware_owner *firmware;
	struct s7d_codec_firmware_job *preload;
	struct regmap *map;
	struct mutex control;
	struct mutex protocol;
	struct completion idle;
	struct completion quiesce_entered;
	spinlock_t irq_lock;
	struct s7d_hevc_job *job;
	struct s7d_codec_session *session;
	struct s7d_codec_session *stopping_session;
	struct s7d_hevc_job *stopping_job;
	u64 stopping_epoch;
	struct s7d_hevc_context *context;
	struct s7d_hevc_dpb *dpb;
	struct s7d_hevc_sps sps;
	struct s7d_codec_layout layout;
	struct s7d_v4l2_capture_buffer capture[S7D_HEVC_MAX_CAPTURE_BUFFERS];
	struct s7d_hevc_surface surface[S7D_HEVC_MAX_CAPTURE_BUFFERS];
	struct s7d_hevc_dma_region mv[S7D_HEVC_MAX_CAPTURE_BUFFERS];
	u64 queue_serial[S7D_HEVC_MAX_CAPTURE_BUFFERS];
	u64 write_token[S7D_HEVC_MAX_CAPTURE_BUFFERS];
	u64 epoch;
	u64 firmware_epoch;
	u64 generation;
	u64 last_input_token;
	u32 captures;
	u32 queued_mask;
	u32 pin_mask;
	u32 output_sequence;
	u32 decode_count;
	u8 start_decoding_flag;
	u8 rps_set_id;
	int irq;
	int release_error;
	int failure;
	bool irq_accept;
	bool irq_enabled;
	bool stop_requested;
	bool closing;
	bool quiesced;
	bool quarantined;
	bool release_pin;
	bool lease_active;
	bool running;
	bool context_initialized;
};

static int hevc_check_stop(struct s7d_hevc_decoder *decoder)
{
	return READ_ONCE(decoder->stop_requested) || READ_ONCE(decoder->closing) ?
		-ECANCELED : 0;
}

static int hevc_job_check_stop(struct s7d_hevc_decoder *decoder,
			       struct s7d_hevc_job *job)
{
	int ret = hevc_check_stop(decoder);

	if (ret)
		job->result.cancelled = true;
	return ret;
}

static int hevc_check_done(const struct s7d_hevc_geometry *geometry,
			   const struct s7d_hevc_picture_done *done)
{
	if (!geometry->ctb_count || !done->max_shift_bytes)
		return -EINVAL;
	if (done->status != S7D_HEVC_PICTURE_DATA_DONE || !done->header_armed ||
	    done->accepted_headers != 1)
		return -EPROTO;
	if (done->error_latched || (done->shift_status & BIT(15)) ||
	    (done->parser_lcu_start & GENMASK(23, 0)) != geometry->ctb_count - 1 ||
	    !done->shift_byte_count || done->shift_byte_count > done->max_shift_bytes)
		return -EBADMSG;
	return 0;
}

static void hevc_finish(struct s7d_hevc_job *job, int error, bool cancelled)
{
	if (job->finished)
		return;
	job->status = error;
	job->finished_cancelled = cancelled;
	job->finished = true;
	complete_all(&job->wake);
}

static irqreturn_t hevc_irq(int irq, void *data)
{
	struct s7d_hevc_decoder *decoder = data;
	struct s7d_hevc_irq_snapshot snapshot = { };
	struct s7d_hevc_job *job;
	unsigned long flags;
	u32 status;
	enum s7d_hevc_event event;
	int ret;

	spin_lock_irqsave(&decoder->irq_lock, flags);
	job = decoder->irq_accept ? decoder->job : NULL;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (!job)
		return IRQ_HANDLED;
	ret = regmap_write(decoder->map, HEVC_MAILBOX_CLEAR, 1);
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_STATUS, &snapshot.status);
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_LCU_START, &snapshot.lcu);
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_SHIFT_STATUS, &snapshot.shift_status);
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_SHIFT_BYTES, &snapshot.shift_bytes);
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_DECODE_INFO, &snapshot.decode_info);
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_STATUS, &status);
	if (!ret && status != snapshot.status)
		ret = -ESTALE;
	snapshot.error = ret;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	job->snapshot = snapshot;
	job->snapshot_valid = true;
	if (!ret)
		ret = s7d_hevc_classify_status(snapshot.status, &event);
	if (!ret && event != S7D_HEVC_EVENT_SLICE_HEADER &&
	    event != S7D_HEVC_EVENT_PICTURE_DATA)
		ret = -ENODATA;
	if (ret && !job->irq_error)
		job->irq_error = ret;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	return IRQ_WAKE_THREAD;
}

static bool hevc_job_stopping(struct s7d_hevc_decoder *decoder,
			      const struct s7d_hevc_job *job)
{
	unsigned long flags;
	bool stopping;

	spin_lock_irqsave(&decoder->irq_lock, flags);
	stopping = decoder->stopping_session == job->session &&
		decoder->stopping_job == job &&
		decoder->stopping_epoch == job->lease_epoch;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	return stopping;
}

static bool hevc_core_blocked(struct s7d_hevc_decoder *decoder)
{
	struct s7d_hevc_job *job = decoder->job;

	return job && (job->core_shutdown || hevc_job_stopping(decoder, job));
}

static int hevc_core_result(struct s7d_hevc_decoder *decoder, int error)
{
	if (error == -ESHUTDOWN && decoder->job)
		decoder->job->core_shutdown = true;
	return error;
}

static int hevc_apply_releases(struct s7d_hevc_decoder *decoder,
			       const struct s7d_hevc_dpb_actions *actions)
{
	u32 mask = actions->surface_release_mask | actions->discard_mask;
	u32 slot;
	int ret;

	if (actions->generation != decoder->generation ||
	    (actions->surface_release_mask & actions->discard_mask) ||
	    actions->mv_release_mask != mask || (mask & ~decoder->pin_mask))
		return -ESTALE;
	for (slot = 0; slot < decoder->captures; slot++) {
		if (!(mask & BIT(slot)))
			continue;
		if (hevc_core_blocked(decoder))
			return hevc_core_result(decoder, -ESHUTDOWN);
		if (actions->discard_mask & BIT(slot)) {
			if (actions->discard_write_token[slot] != decoder->write_token[slot])
				return -ESTALE;
			ret = hevc_core_result(decoder, s7d_codec_surface_discard(decoder->capture[slot].surface,
						decoder->write_token[slot]));
		} else {
			ret = hevc_core_result(decoder, s7d_codec_surface_ref_put(decoder->capture[slot].surface));
		}
		if (ret)
			return ret;
		decoder->pin_mask &= ~BIT(slot);
		if (actions->discard_mask & BIT(slot)) {
			unsigned long flags;

			spin_lock_irqsave(&decoder->irq_lock, flags);
			decoder->queued_mask |= BIT(slot);
			spin_unlock_irqrestore(&decoder->irq_lock, flags);
		}
	}
	return s7d_hevc_dpb_release_committed(decoder->dpb, actions);
}

static int hevc_emit(struct s7d_hevc_decoder *decoder, bool drain,
		     bool one, u32 *published)
{
	struct s7d_hevc_output_plan out;
	struct s7d_hevc_dpb_actions actions;
	struct s7d_codec_frame frame;
	int ret;

	for (;;) {
		ret = s7d_hevc_dpb_peek_output(decoder->dpb, drain, &out);
		if (ret == -EAGAIN || ret == -ENOENT)
			return 0;
		if (ret)
			return ret;
		if (out.generation != decoder->generation || out.slot >= decoder->captures ||
		    !(decoder->pin_mask & BIT(out.slot)) ||
		    decoder->write_token[out.slot] != out.write_token)
			return -ESTALE;
		if (decoder->output_sequence == U32_MAX)
			return -EOVERFLOW;
		frame = (struct s7d_codec_frame) {
			.surface_id = decoder->capture[out.slot].surface_id,
			.generation = out.generation, .write_token = out.write_token,
			.timestamp_ns = out.metadata.timestamp_ns,
			.timestamp_meta = out.metadata.timestamp_meta,
			.sequence = decoder->output_sequence, .flags = out.flags,
			.bytesused = { decoder->layout.size[0], decoder->layout.size[1] },
		};
		if (hevc_core_blocked(decoder))
			return hevc_core_result(decoder, -ESHUTDOWN);
		ret = hevc_core_result(decoder, s7d_codec_session_frame_ready(decoder->session, &frame));
		if (ret)
			return ret;
		decoder->output_sequence++;
		(*published)++;
		ret = s7d_hevc_dpb_output_committed(decoder->dpb, &out, &actions);
		if (!ret)
			ret = hevc_apply_releases(decoder, &actions);
		if (ret || one)
			return ret;
	}
}

static int hevc_header(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job,
			bool *cancelled)
{
	struct s7d_hevc_picture_buffers buffers = {
		.count = decoder->captures, .surface = decoder->surface, .mv = decoder->mv,
	};
	u8 raw[S7D_HEVC_RPM_BYTES];
	unsigned long flags;
	u64 serial;
	u32 status, lmem, mask, slot;
	int ret;

	*cancelled = false;
	if (job->headers || job->header_armed)
		return -EOPNOTSUPP;
	ret = regmap_read(decoder->map, HEVC_STATUS, &status);
	if (!ret && status != S7D_HEVC_SLICE_HEADER_DONE)
		ret = -ESTALE;
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_LMEM_DMA_CONTROL, &lmem);
	if (!ret && (lmem & BIT(15)))
		ret = -EBUSY;
	if (!ret && !job->rpm_valid) {
		dma_rmb();
		ret = s7d_hevc_context_snapshot_rpm(job->context, raw, sizeof(raw));
		if (!ret)
			ret = s7d_hevc_rpm_parse(raw, sizeof(raw), true, &job->rpm);
		if (!ret)
			ret = s7d_hevc_rpm_check_sequence(&job->rpm, &decoder->sps, &job->geometry);
		if (!ret)
			ret = s7d_hevc_rpm_decode_slice(&job->rpm, &decoder->sps,
						&job->host_slice, &job->slice);
		if (!ret)
			job->rpm_valid = true;
	}
	if (ret)
		return ret;
	for (;;) {
		spin_lock_irqsave(&decoder->irq_lock, flags);
		mask = decoder->queued_mask;
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
		ret = s7d_hevc_dpb_plan(decoder->dpb, &job->slice, &job->metadata,
					mask, &job->picture);
		if (ret)
			return ret;
		if (job->picture.kind == S7D_HEVC_PLAN_SKIP_RASL) {
			job->result.skipped = true;
			hevc_finish(job, 0, false);
			return 0;
		}
		if (job->picture.kind == S7D_HEVC_PLAN_BUMP) {
			u32 before = job->result.published_frames;

			ret = hevc_emit(decoder, true, true, &job->result.published_frames);
			if (ret || before == job->result.published_frames)
				return ret ? ret : -EPROTO;
			continue;
		}
		slot = job->picture.current_slot;
		if (slot >= decoder->captures || job->picture.generation != decoder->generation)
			return -ESTALE;
		spin_lock_irqsave(&decoder->irq_lock, flags);
		serial = decoder->queue_serial[slot];
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
		if (hevc_core_blocked(decoder))
			return hevc_core_result(decoder, -ESHUTDOWN);
		ret = hevc_core_result(decoder, s7d_codec_surface_acquire(decoder->capture[slot].surface,
						&job->write_token));
		if (ret == -EAGAIN) {
			spin_lock_irqsave(&decoder->irq_lock, flags);
			if (serial == decoder->queue_serial[slot])
				decoder->queued_mask &= ~BIT(slot);
			spin_unlock_irqrestore(&decoder->irq_lock, flags);
			continue;
		}
		if (ret)
			return ret;
		job->current_slot = slot;
		job->acquired = true;
		decoder->write_token[slot] = job->write_token;
		spin_lock_irqsave(&decoder->irq_lock, flags);
		decoder->queued_mask &= ~BIT(slot);
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
		if (hevc_core_blocked(decoder))
			return hevc_core_result(decoder, -ESHUTDOWN);
		ret = hevc_core_result(decoder, s7d_codec_surface_ref_get(decoder->capture[slot].surface));
		if (ret)
			return ret;
		decoder->pin_mask |= BIT(slot);
		ret = s7d_hevc_dpb_begin(decoder->dpb, &job->picture, job->write_token);
		if (ret)
			return ret;
		job->dpb_begun = true;
		break;
	}
	ret = regmap_read(decoder->map, HEVC_STATUS, &status);
	if (!ret && status != S7D_HEVC_SLICE_HEADER_DONE)
		ret = -ESTALE;
	if (!ret)
		ret = s7d_hevc_picture_configure(decoder->map,
			s7d_hevc_context_workspace(job->context), &job->rpm,
			&decoder->sps, &decoder->layout, &buffers, &job->slice, &job->picture);
	if (!ret)
		ret = regmap_update_bits(decoder->map, HEVC_WAIT_FLAG, BIT(1), BIT(1));
	if (!ret) {
		ret = hevc_check_stop(decoder);
		*cancelled = ret == -ECANCELED;
	}
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_STATUS, &status);
	if (!ret && status != S7D_HEVC_SLICE_HEADER_DONE)
		ret = -ESTALE;
	if (ret)
		return ret;
	dma_wmb();
	ret = regmap_write(decoder->map, HEVC_STATUS, 0x05);
	if (!ret)
		ret = regmap_write(decoder->map, HEVC_CPU_INTERRUPT, 4);
	if (!ret) {
		job->headers = 1;
		job->header_armed = true;
		job->header_waiting = false;
	}
	return ret;
}

static irqreturn_t hevc_irq_thread(int irq, void *data)
{
	struct s7d_hevc_decoder *decoder = data;
	struct s7d_hevc_irq_snapshot snapshot;
	struct s7d_hevc_picture_done done;
	struct s7d_hevc_job *job;
	enum s7d_hevc_event event;
	unsigned long flags;
	bool cancelled = false;
	int ret;

	mutex_lock(&decoder->protocol);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	job = decoder->irq_accept ? decoder->job : NULL;
	if (job)
		snapshot = job->snapshot;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (!job || job->finished)
		goto out;
	ret = snapshot.error;
	if (!ret)
		ret = s7d_hevc_classify_status(snapshot.status, &event);
	if (!ret) {
		ret = hevc_check_stop(decoder);
		cancelled = ret == -ECANCELED;
	}
	if (ret)
		goto failed;
	if (event == S7D_HEVC_EVENT_SLICE_HEADER) {
		ret = hevc_header(decoder, job, &cancelled);
		if (ret == -EAGAIN) {
			job->header_waiting = true;
			complete_all(&job->wake);
			goto out;
		}
		if (ret)
			goto failed;
		goto out;
	}
	if (event != S7D_HEVC_EVENT_PICTURE_DATA) {
		ret = -ENODATA;
		goto failed;
	}
	done = (struct s7d_hevc_picture_done) {
		.status = snapshot.status,
		.parser_lcu_start = snapshot.lcu,
		.shift_status = snapshot.shift_status,
		.shift_byte_count = snapshot.shift_bytes,
		.max_shift_bytes = job->input.max_shift_bytes,
		.accepted_headers = job->headers,
		.header_armed = job->header_armed,
	};
	ret = hevc_check_done(&job->geometry, &done);
	if (ret)
		goto failed;
	job->picture_complete = true;
	hevc_finish(job, 0, false);
	goto out;
failed:
	if (!cancelled && !job->core_shutdown) {
		spin_lock_irqsave(&decoder->irq_lock, flags);
		if (!job->irq_error)
			job->irq_error = ret;
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
	}
	hevc_finish(job, ret, cancelled);
out:
	mutex_unlock(&decoder->protocol);
	return IRQ_HANDLED;
}

static int hevc_hold(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_hevc_decoder *decoder = priv;
	unsigned long flags;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	if (READ_ONCE(decoder->closing) || decoder->quarantined || decoder->release_error)
		return -ESHUTDOWN;
	if (decoder->epoch == U64_MAX)
		return -EOVERFLOW;
	ret = s7d_codec_dos_hold(decoder->dos, engine);
	if (!ret) {
		decoder->epoch++;
		decoder->firmware_epoch = 0;
		decoder->quiesced = false;
		decoder->lease_active = true;
		decoder->failure = 0;
		decoder->decode_count = 0;
		spin_lock_irqsave(&decoder->irq_lock, flags);
		decoder->stop_requested = false;
		decoder->stopping_session = NULL;
		decoder->stopping_job = NULL;
		decoder->stopping_epoch = 0;
		reinit_completion(&decoder->quiesce_entered);
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
	}
	return ret;
}

static int hevc_quiesce_locked(struct s7d_hevc_decoder *decoder,
			       enum s7d_codec_engine engine)
{
	unsigned long flags;
	bool disable;
	int ret, halted;

	lockdep_assert_held(&decoder->control);
	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	if (decoder->quarantined)
		return -EIO;
	if (decoder->quiesced)
		return 0;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->irq_accept = false;
	disable = decoder->irq_enabled;
	decoder->irq_enabled = false;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	ret = s7d_codec_dos_prepare_access(decoder->dos, engine);
	if (!ret)
		ret = s7d_codec_dos_hevc_irq_mask(decoder->dos, false);
	if (disable)
		disable_irq_nosync(decoder->irq);
	synchronize_irq(decoder->irq);
	mutex_lock(&decoder->protocol);
	if (!ret) {
		halted = s7d_codec_dos_halt(decoder->dos, engine);
		if (halted)
			ret = halted;
	}
	if (!ret && decoder->job)
		decoder->job->opaque_swap = 0;
	decoder->quiesced = !ret;
	decoder->quarantined = !!ret;
	mutex_unlock(&decoder->protocol);
	return ret;
}

static int hevc_quiesce(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_hevc_decoder *decoder = priv;
	unsigned long flags;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->stopping_session = decoder->session;
	decoder->stopping_job = decoder->job;
	decoder->stopping_epoch = decoder->epoch;
	decoder->stop_requested = true;
	complete_all(&decoder->quiesce_entered);
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	s7d_hevc_decoder_request_stop(decoder);
	mutex_lock(&decoder->control);
	while (decoder->running) {
		s7d_hevc_decoder_request_stop(decoder);
		mutex_unlock(&decoder->control);
		wait_for_completion(&decoder->idle);
		mutex_lock(&decoder->control);
	}
	ret = hevc_quiesce_locked(decoder, engine);
	mutex_unlock(&decoder->control);
	return ret;
}

static void hevc_release(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_hevc_decoder *decoder = priv;
	int ret;

	mutex_lock(&decoder->control);
	ret = engine != S7D_CODEC_ENGINE_HEVC || !decoder->quiesced ?
		-EBUSY : s7d_codec_dos_release(decoder->dos, engine);
	if (!ret) {
		decoder->lease_active = false;
		mutex_unlock(&decoder->control);
		return;
	}
	decoder->release_error = ret;
	if (!decoder->release_pin && try_module_get(decoder->owner))
		decoder->release_pin = true;
	dev_err(decoder->dev, "HEVC release failed: %d; owner retained\n", ret);
	mutex_unlock(&decoder->control);
}

static const struct s7d_codec_resource_ops hevc_resource_ops = {
	.hold = hevc_hold,
	.quiesce = hevc_quiesce,
	.release = hevc_release,
};

static int hevc_preload(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job)
{
	struct s7d_codec_firmware_job *preload;
	unsigned long flags;
	bool queued;
	int ret;

	if (!decoder->firmware_epoch) {
		ret = s7d_codec_firmware_next_epoch(decoder->firmware,
						    &decoder->firmware_epoch);
		if (ret)
			return ret;
	}
	job->firmware_epoch = decoder->firmware_epoch;
	ret = s7d_codec_firmware_prepare_for_engine(decoder->firmware, job->session,
			job->firmware_epoch, S7D_CODEC_ENGINE_HEVC,
			S7D_CODEC_FIRMWARE_HEVC, &preload);
	if (ret || !preload)
		return ret ? ret : hevc_job_check_stop(decoder, job);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->preload = preload;
	if (decoder->stop_requested || decoder->closing)
		s7d_codec_firmware_job_stop(preload);
	queued = s7d_codec_firmware_job_queue(preload);
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (queued) {
		mutex_unlock(&decoder->control);
		ret = s7d_codec_firmware_job_join_result(preload, job->session,
				job->firmware_epoch, &job->result.cancelled);
		mutex_lock(&decoder->control);
		s7d_codec_firmware_retire(decoder->firmware, preload);
	} else {
		decoder->quarantined = true;
		return -EIO;
	}
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->preload = NULL;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	mutex_unlock(&decoder->control);
	s7d_codec_firmware_job_release_custody(preload);
	s7d_codec_firmware_job_put(preload);
	mutex_lock(&decoder->control);
	if (decoder->job != job || job->lease_epoch != decoder->epoch)
		return -ESTALE;
	return ret ? ret : hevc_job_check_stop(decoder, job);
}

static int hevc_protocol(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job)
{
	const struct s7d_hevc_workspace *workspace =
		s7d_hevc_context_workspace(job->context);
	const struct reg_sequence regs[] = {
		{ HEVC_PSCALE_CONTROL, 0 },
		{ HEVC_COUNT, decoder->decode_count },
		{ HEVC_MODE2, decoder->rps_set_id },
		{ HEVC_NAL_SEARCH, BIT(21) | BIT(22) | 4 },
		{ HEVC_MODE, 0x80000001 | ((u32)decoder->start_decoding_flag << 16) },
		{ HEVC_STOP_POSITION, 0 },
		{ HEVC_AUX_ADDRESS, workspace->aux.address },
		{ HEVC_AUX_SIZE, ((workspace->aux_prefix_bytes >> 4) << 16) |
				(workspace->aux_suffix_bytes >> 4) },
		{ HEVC_WAIT_FLAG, 0 },
	};
	int ret;

	ret = s7d_hevc_workspace_program(decoder->map, workspace);
	if (!ret)
		ret = s7d_codec_dos_hevc_init(decoder->dos);
	if (!ret)
		ret = s7d_hevc_protocol_configure(decoder->map);
	if (!ret)
		ret = regmap_multi_reg_write(decoder->map, regs, ARRAY_SIZE(regs));
	return ret;
}

static int hevc_start(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job)
{
	const struct reg_sequence regs[] = {
		{ HEVC_STATUS, 0xff },
		{ HEVC_SHIFT_BYTES, 0 },
		{ HEVC_DECODE_SIZE, job->input.decode_size },
		{ HEVC_COUNT, decoder->decode_count },
	};
	unsigned long flags;
	int ret;

	ret = hevc_job_check_stop(decoder, job);
	if (!ret)
		ret = regmap_write(decoder->map, HEVC_MAILBOX_CLEAR, 1);
	if (ret)
		return ret;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->irq_accept = true;
	decoder->irq_enabled = true;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	enable_irq(decoder->irq);
	ret = s7d_codec_dos_hevc_irq_mask(decoder->dos, true);
	if (!ret)
		ret = hevc_job_check_stop(decoder, job);
	if (!ret)
		ret = s7d_codec_dos_hevc_input_enable(decoder->dos);
	if (!ret)
		ret = regmap_multi_reg_write(decoder->map, regs, ARRAY_SIZE(regs));
	if (!ret) {
		dma_wmb();
		ret = s7d_codec_dos_start(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	}
	return ret;
}

void s7d_hevc_decoder_request_stop(struct s7d_hevc_decoder *decoder)
{
	unsigned long flags;

	if (!decoder)
		return;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->stop_requested = true;
	if (decoder->preload)
		s7d_codec_firmware_job_stop(decoder->preload);
	if (decoder->job) {
		complete_all(&decoder->job->wake);
	}
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
}

static int hevc_validate_capture(const struct s7d_codec_layout *layout,
				 const struct s7d_v4l2_capture_buffer *capture,
				 u64 generation, u32 count,
				 struct s7d_hevc_surface *surface)
{
	dma_addr_t address;
	size_t bytes;
	u32 i, j;
	int ret;

	if (!generation || !capture || count < layout->min_capture_buffers ||
	    count > S7D_HEVC_MAX_CAPTURE_BUFFERS)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!capture[i].surface || capture[i].generation != generation ||
		    capture[i].surface_id >= S7D_CODEC_MAX_CAPTURE_BUFFERS)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (capture[i].surface == capture[j].surface ||
			    capture[i].surface_id == capture[j].surface_id)
				return -EINVAL;
		for (j = 0; j < 2; j++) {
			ret = s7d_codec_surface_dma(capture[i].surface, j, &address, &bytes);
			if (ret)
				return ret;
			if (!IS_ALIGNED(address, S7D_CODEC_STORAGE_ALIGNMENT) ||
			    address > U32_MAX || bytes > U32_MAX || bytes < layout->size[j])
				return -ERANGE;
			surface[i].address[j] = address;
			surface[i].bytes[j] = bytes;
		}
	}
	return 0;
}

int s7d_hevc_decoder_bind(struct s7d_hevc_decoder *decoder,
			 struct s7d_codec_session *session,
			 const struct s7d_hevc_sps *sps,
			 const struct s7d_codec_layout *layout,
			 u64 generation, u64 parameter_epoch,
			 const struct s7d_v4l2_capture_buffer *capture, u32 count)
{
	struct s7d_codec_layout checked;
	unsigned long flags;
	u32 i;
	int ret;

	if (!decoder || !session || !sps || !layout || !parameter_epoch)
		return -EINVAL;
	mutex_lock(&decoder->control);
	if (decoder->closing || decoder->running || decoder->session || decoder->job ||
	    decoder->quarantined || decoder->failure || !decoder->lease_active) {
		ret = decoder->failure ? decoder->failure : -EBUSY;
		goto out;
	}
	ret = s7d_hevc_build_layout(sps, S7D_CODEC_CAPTURE_NV12M, 0, &checked);
	if (!ret && memcmp(&checked, layout, sizeof(checked)))
		ret = -EINVAL;
	if (!ret)
		ret = hevc_validate_capture(layout, capture, generation, count, decoder->surface);
	if (ret)
		goto out;
	decoder->dpb = kzalloc(sizeof(*decoder->dpb), GFP_KERNEL);
	if (!decoder->dpb) {
		ret = -ENOMEM;
		goto out;
	}
	ret = s7d_hevc_dpb_init(decoder->dpb, sps, generation, parameter_epoch, count);
	if (ret)
		goto free_dpb;
	decoder->context = s7d_hevc_context_create_stream(session, sps,
			S7D_HEVC_INPUT_MAX_BYTES, count, decoder->epoch);
	if (IS_ERR(decoder->context)) {
		ret = PTR_ERR(decoder->context);
		decoder->context = NULL;
		goto free_dpb;
	}
	decoder->session = session;
	decoder->sps = *sps;
	decoder->layout = *layout;
	decoder->generation = generation;
	decoder->captures = count;
	memcpy(decoder->capture, capture, sizeof(*capture) * count);
	for (i = 0; i < count; i++) {
		ret = s7d_hevc_context_mv_slot(decoder->context, decoder->epoch,
					      i, &decoder->mv[i]);
		if (ret) {
			decoder->failure = ret;
			goto out;
		}
	}
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->queued_mask = GENMASK(count - 1, 0);
	for (i = 0; i < count; i++)
		decoder->queue_serial[i] = 1;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	ret = 0;
	goto out;
free_dpb:
	kfree(decoder->dpb);
	decoder->dpb = NULL;
out:
	mutex_unlock(&decoder->control);
	return ret;
}

int s7d_hevc_decoder_capture_queue(struct s7d_hevc_decoder *decoder,
				  struct s7d_codec_surface *surface,
				  u64 surface_id, u64 generation)
{
	unsigned long flags;
	u32 slot;
	int ret = -ESTALE;

	if (!decoder || !surface || !generation)
		return -EINVAL;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	for (slot = 0; slot < decoder->captures; slot++) {
		if (decoder->capture[slot].surface != surface ||
		    decoder->capture[slot].surface_id != surface_id ||
		    decoder->capture[slot].generation != generation)
			continue;
		if (decoder->queue_serial[slot] == U64_MAX) {
			ret = -EOVERFLOW;
			break;
		}
		decoder->queue_serial[slot]++;
		decoder->queued_mask |= BIT(slot);
		if (decoder->job)
			complete_all(&decoder->job->wake);
		ret = 0;
		break;
	}
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	return ret;
}

static int hevc_job_halt(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job)
{
	int ret, marked;

	ret = hevc_quiesce_locked(decoder, S7D_CODEC_ENGINE_HEVC);
	if (job->context_begun) {
		marked = s7d_hevc_context_mark_idle(job->context, job->lease_epoch,
				job->metadata.input_token, ret, true);
		if (!ret)
			ret = marked;
		if (!ret)
			job->context_begun = false;
	}
	if (ret)
		decoder->quarantined = true;
	return ret;
}

static int hevc_joined_error(struct s7d_hevc_decoder *decoder,
			     struct s7d_hevc_job *job)
{
	struct s7d_hevc_picture_done done;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&decoder->irq_lock, flags);
	ret = job->irq_error;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (ret)
		return ret;
	if (job->finished && job->status && !job->finished_cancelled && !job->core_shutdown)
		return job->status;
	if (!job->snapshot_valid || job->snapshot.status != S7D_HEVC_PICTURE_DATA_DONE)
		return 0;
	done = (struct s7d_hevc_picture_done) {
		.status = job->snapshot.status, .parser_lcu_start = job->snapshot.lcu,
		.shift_status = job->snapshot.shift_status,
		.shift_byte_count = job->snapshot.shift_bytes,
		.max_shift_bytes = job->input.max_shift_bytes,
		.accepted_headers = job->headers, .header_armed = job->header_armed,
	};
	return hevc_check_done(&job->geometry, &done);
}

static int hevc_join_core_stop(struct s7d_hevc_decoder *decoder,
			       struct s7d_hevc_job *job)
{
	lockdep_assert_held(&decoder->control);
	lockdep_assert_not_held(&decoder->protocol);
	if (hevc_job_stopping(decoder, job))
		return 0;
	mutex_unlock(&decoder->control);
	wait_for_completion_timeout(&decoder->quiesce_entered,
				    msecs_to_jiffies(HEVC_TIMEOUT_MS));
	mutex_lock(&decoder->control);
	if (decoder->job != job || decoder->epoch != job->lease_epoch ||
	    decoder->session != job->session)
		return -ESTALE;
	return hevc_job_stopping(decoder, job) ? 0 : -ESHUTDOWN;
}

static int hevc_discard_current(struct s7d_hevc_decoder *decoder,
				struct s7d_hevc_job *job)
{
	struct s7d_hevc_dpb_actions actions;
	u32 slot = job->current_slot;
	int ret;

	if (hevc_core_blocked(decoder))
		return hevc_core_result(decoder, -ESHUTDOWN);
	if (!job->acquired && !job->dpb_begun)
		return 0;
	if (job->acquired) {
		ret = hevc_core_result(decoder, s7d_codec_surface_written(decoder->capture[slot].surface, job->write_token));
		if (ret)
			return ret;
		job->acquired = false;
	}
	if (job->dpb_begun) {
		ret = s7d_hevc_dpb_abort(decoder->dpb, job->picture.plan_id, slot,
					job->write_token, true, &actions);
		if (!ret)
			ret = hevc_apply_releases(decoder, &actions);
		if (!ret)
			job->dpb_begun = false;
		return ret;
	}
	if (!(decoder->pin_mask & BIT(slot)))
		return -EPROTO;
	ret = hevc_core_result(decoder, s7d_codec_surface_discard(decoder->capture[slot].surface, job->write_token));
	if (!ret) {
		unsigned long flags;

		decoder->pin_mask &= ~BIT(slot);
		spin_lock_irqsave(&decoder->irq_lock, flags);
		decoder->queued_mask |= BIT(slot);
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
	}
	return ret;
}

static int hevc_wait_picture(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(HEVC_TIMEOUT_MS);
	bool cancelled;
	int ret;

	for (;;) {
		if (READ_ONCE(job->header_waiting)) {
			wait_for_completion(&job->wake);
		} else {
			if (time_after_eq(jiffies, deadline) ||
			    !wait_for_completion_timeout(&job->wake, deadline - jiffies))
				return -ETIMEDOUT;
		}
		reinit_completion(&job->wake);
		mutex_lock(&decoder->protocol);
		if (job->finished) {
			ret = job->status;
			job->result.cancelled = job->finished_cancelled;
			mutex_unlock(&decoder->protocol);
			return ret;
		}
		ret = hevc_check_stop(decoder);
		if (ret) {
			job->result.cancelled = true;
			mutex_unlock(&decoder->protocol);
			return ret;
		}
		if (job->header_waiting) {
			ret = hevc_header(decoder, job, &cancelled);
			if (ret && cancelled)
				job->result.cancelled = true;
			if (ret == -EAGAIN)
				ret = 0;
			else if (!ret && job->header_armed)
				deadline = jiffies + msecs_to_jiffies(HEVC_TIMEOUT_MS);
		} else {
			ret = 0;
		}
		mutex_unlock(&decoder->protocol);
		if (ret)
			return ret;
	}
}

int s7d_hevc_decoder_decode(struct s7d_hevc_decoder *decoder,
			   struct s7d_codec_session *session,
			   const struct s7d_hevc_input_result *input,
			   u64 input_token, u64 timestamp_ns,
			   const struct s7d_codec_timestamp_metadata *timestamp_meta,
			   struct s7d_hevc_decode_result *result)
{
	struct s7d_hevc_dpb_actions actions;
	struct s7d_hevc_context_au au;
	struct s7d_hevc_job *job;
	unsigned long flags;
	int ret, stopped, cleanup, fault;

	if (!decoder || !session || !input || !input->data || !input->bytes ||
	    !input_token || !timestamp_meta || !result ||
	    input->kind != S7D_HEVC_INPUT_PICTURE)
		return -EINVAL;
	*result = (struct s7d_hevc_decode_result) { };
	mutex_lock(&decoder->control);
	if (decoder->closing || decoder->running || decoder->job || decoder->quarantined ||
	    decoder->failure || decoder->release_error || decoder->session != session ||
	    !decoder->context || !decoder->dpb || !decoder->lease_active) {
		ret = decoder->failure ? decoder->failure : -EBUSY;
		goto out;
	}
	if (input_token <= decoder->last_input_token || decoder->decode_count == U32_MAX) {
		ret = -ESTALE;
		goto out;
	}
	ret = hevc_check_stop(decoder);
	if (ret) {
		result->cancelled = true;
		goto out;
	}
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out;
	}
	init_completion(&job->wake);
	job->status = -EINPROGRESS;
	job->current_slot = S7D_HEVC_NO_SLOT;
	job->session = session;
	job->context = decoder->context;
	job->host_slice = input->slice;
	job->lease_epoch = decoder->epoch;
	job->metadata = (struct s7d_hevc_picture_metadata) {
		.input_token = input_token, .timestamp_ns = timestamp_ns,
		.timestamp_meta = *timestamp_meta,
	};
	s7d_codec_session_get(session);
	reinit_completion(&decoder->idle);
	decoder->running = true;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	if (decoder->stop_requested || decoder->closing) {
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
		result->cancelled = true;
		ret = -ECANCELED;
		s7d_codec_session_put(job->session);
		kfree(job);
		decoder->running = false;
		complete_all(&decoder->idle);
		goto out;
	}
	decoder->job = job;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	ret = hevc_quiesce_locked(decoder, S7D_CODEC_ENGINE_HEVC);
	if (!ret && !decoder->context_initialized) {
		ret = s7d_hevc_context_mark_idle(job->context, decoder->epoch, 0, 0, true);
		if (!ret)
			decoder->context_initialized = true;
	}
	au = (struct s7d_hevc_context_au) {
		.data = input->data, .bytes = input->bytes, .sps = &decoder->sps,
		.lease_epoch = decoder->epoch, .input_token = input_token,
	};
	if (!ret)
		ret = s7d_hevc_context_stage_au(job->context, &au, &job->input);
	if (!ret)
		ret = s7d_hevc_context_begin(job->context, decoder->epoch, input_token);
	if (ret)
		goto stop;
	job->context_begun = true;
	decoder->quiesced = false;
	ret = s7d_codec_dos_prepare(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_hevc_context_program_input(job->context, decoder->map);
	if (!ret)
		ret = s7d_codec_dos_port_enable(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = hevc_preload(decoder, job);
	if (!ret)
		ret = s7d_codec_firmware_load(decoder->dev, S7D_CODEC_ENGINE_HEVC,
					      S7D_CODEC_FIRMWARE_HEVC);
	if (ret) {
		s7d_codec_firmware_invalidate(decoder->firmware);
		goto stop;
	}
	ret = regmap_read(decoder->map, HEVC_SWAP_TOKEN, &job->opaque_swap);

	mutex_lock(&decoder->protocol);
	if (!ret)
		ret = hevc_protocol(decoder, job);
	if (!ret)
		ret = hevc_start(decoder, job);
	mutex_unlock(&decoder->protocol);
	if (!ret)
		ret = hevc_wait_picture(decoder, job);
stop:
	stopped = hevc_job_halt(decoder, job);
	fault = stopped ? READ_ONCE(job->irq_error) : hevc_joined_error(decoder, job);
	if (fault) {
		ret = fault;
		job->result.cancelled = false;
	}
	if (stopped) {
		dev_err(decoder->dev, "HEVC input %llu error %d, halt unproved %d; DMA retained\n",
			input_token, ret, stopped);
		if (!ret || job->result.cancelled)
			ret = stopped;
		job->result.cancelled = false;
		goto retained;
	}
	if (!ret)
		ret = hevc_job_check_stop(decoder, job);
	if (hevc_job_stopping(decoder, job) && !ret) {
		ret = -ECANCELED;
		job->result.cancelled = true;
	}
	if (!ret && !job->picture_complete && !job->result.skipped)
		ret = -EPROTO;
	if (!ret && job->result.skipped) {
		ret = s7d_hevc_dpb_skip_committed(decoder->dpb, &job->picture);
	} else if (!ret) {
		ret = hevc_core_result(decoder, s7d_codec_surface_written(decoder->capture[job->current_slot].surface,
						job->write_token));
		if (!ret) {
			job->acquired = false;
			ret = s7d_hevc_dpb_complete(decoder->dpb, job->picture.plan_id,
				job->current_slot, job->write_token, true, true, &actions);
		}
		if (!ret) {
			job->dpb_begun = false;
			ret = hevc_apply_releases(decoder, &actions);
		}
		if (!ret)
			ret = hevc_emit(decoder, false, false, &job->result.published_frames);
	}
	if (ret && !job->core_shutdown && !hevc_job_stopping(decoder, job)) {
		cleanup = hevc_discard_current(decoder, job);
		if (cleanup && (!ret || job->result.cancelled)) {
			ret = cleanup;
			job->result.cancelled = false;
		}
	}
	if (job->core_shutdown) {
		cleanup = hevc_join_core_stop(decoder, job);
		if (cleanup) {
			decoder->quarantined = true;
			job->result.cancelled = false;
			ret = ret ? ret : cleanup;
			goto retained;
		}
		if (!fault && ret == -ESHUTDOWN) {
			ret = -ECANCELED;
			job->result.cancelled = true;
		}
	}
	if (!ret) {
		decoder->last_input_token = input_token;
		decoder->decode_count++;
		if (job->picture_complete) {
			decoder->start_decoding_flag |= job->snapshot.decode_info & 0xff;
			decoder->rps_set_id = (job->snapshot.decode_info >> 8) & 0xff;
		}
	}
	*result = job->result;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->job = NULL;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	s7d_codec_session_put(job->session);
	kfree(job);
	if (ret && !result->cancelled)
		decoder->failure = ret;
	decoder->running = false;
	complete_all(&decoder->idle);
	goto out;
retained:
	*result = job->result;
	decoder->failure = ret;
	decoder->running = false;
	complete_all(&decoder->idle);
out:
	mutex_unlock(&decoder->control);
	return ret;
}

int s7d_hevc_decoder_drain(struct s7d_hevc_decoder *decoder,
			  struct s7d_codec_session *session, u32 *published, bool *cancelled)
{
	int ret;

	if (!decoder || !session || !published || !cancelled)
		return -EINVAL;
	*published = 0;
	*cancelled = false;
	mutex_lock(&decoder->control);
	if (decoder->session != session || decoder->running || decoder->job ||
	    decoder->quarantined || decoder->failure || !decoder->quiesced) {
		ret = decoder->failure ? decoder->failure : -EBUSY;
		goto out;
	}
	ret = hevc_check_stop(decoder);
	if (ret)
		*cancelled = true;
	if (!ret)
		ret = hevc_emit(decoder, true, false, published);
	if (ret && !*cancelled)
		decoder->failure = ret;
out:
	mutex_unlock(&decoder->control);
	return ret;
}

int s7d_hevc_decoder_stream_close(struct s7d_hevc_decoder *decoder,
				 struct s7d_codec_session *session)
{
	unsigned long flags;
	int ret;

	if (!decoder || !session)
		return -EINVAL;
	mutex_lock(&decoder->control);
	if (!decoder->session) {
		ret = 0;
		goto out;
	}
	if (decoder->session != session || decoder->running || decoder->job ||
	    decoder->quarantined) {
		ret = -EBUSY;
		goto out;
	}
	ret = hevc_quiesce_locked(decoder, S7D_CODEC_ENGINE_HEVC);
	if (!ret && !decoder->context_initialized) {
		ret = s7d_hevc_context_mark_idle(decoder->context, decoder->epoch, 0, 0, true);
		if (!ret)
			decoder->context_initialized = true;
	}
	if (!ret)
		ret = s7d_hevc_context_destroy_stream(decoder->context, decoder->epoch);
	if (ret)
		goto out;
	decoder->context = NULL;
	decoder->context_initialized = false;
	kfree(decoder->dpb);
	decoder->dpb = NULL;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->captures = 0;
	decoder->queued_mask = 0;
	memset(decoder->capture, 0, sizeof(decoder->capture));
	memset(decoder->queue_serial, 0, sizeof(decoder->queue_serial));
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	decoder->session = NULL;
	decoder->generation = 0;
	decoder->last_input_token = 0;
	decoder->pin_mask = 0;
	decoder->output_sequence = 0;
	decoder->decode_count = 0;
	decoder->start_decoding_flag = 0;
	decoder->rps_set_id = 0;
	memset(decoder->write_token, 0, sizeof(decoder->write_token));
	ret = 0;
out:
	mutex_unlock(&decoder->control);
	return ret;
}


struct s7d_hevc_decoder *
s7d_hevc_decoder_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			struct module *owner)
{
	struct s7d_hevc_decoder *decoder;
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
	decoder->firmware = borrowed_firmware;
	mutex_init(&decoder->control);
	mutex_init(&decoder->protocol);
	init_completion(&decoder->idle);
	init_completion(&decoder->quiesce_entered);
	complete_all(&decoder->idle);
	spin_lock_init(&decoder->irq_lock);
	decoder->map = s7d_codec_dos_regmap(borrowed_dos, S7D_CODEC_ENGINE_HEVC);
	if (IS_ERR_OR_NULL(decoder->map)) {
		ret = decoder->map ? PTR_ERR(decoder->map) : -ENODEV;
		goto free;
	}
	decoder->irq = s7d_codec_dos_irq(borrowed_dos, S7D_CODEC_ENGINE_HEVC);
	if (decoder->irq < 0) {
		ret = decoder->irq;
		goto free;
	}
	ret = request_threaded_irq(decoder->irq, hevc_irq, hevc_irq_thread,
				   IRQF_NO_AUTOEN | IRQF_ONESHOT, dev_name(dev), decoder);
	if (ret)
		goto free;
	decoder->core = s7d_codec_core_create(dev, owner, &hevc_resource_ops, decoder, 32);
	if (IS_ERR(decoder->core)) {
		ret = PTR_ERR(decoder->core);
		goto free_irq;
	}
	return decoder;
free_irq:
	free_irq(decoder->irq, decoder);
free:
	put_device(decoder->dev);
	kfree(decoder);
	return ERR_PTR(ret);
}

struct s7d_codec_core *s7d_hevc_decoder_core(struct s7d_hevc_decoder *decoder)
{
	return decoder ? decoder->core : NULL;
}

int s7d_hevc_decoder_destroy(struct s7d_hevc_decoder *decoder)
{
	unsigned long flags;
	int ret;

	if (!decoder)
		return -EINVAL;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->closing = true;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	s7d_hevc_decoder_request_stop(decoder);
	mutex_lock(&decoder->control);
	if (decoder->job || decoder->session || decoder->context || decoder->dpb ||
	    decoder->quarantined || decoder->release_error ||
	    decoder->release_pin) {
		ret = -EBUSY;
		goto out;
	}
	ret = s7d_codec_core_destroy(decoder->core);
	if (ret)
		goto out;
	free_irq(decoder->irq, decoder);
	mutex_unlock(&decoder->control);
	put_device(decoder->dev);
	kfree(decoder);
	return 0;
out:
	mutex_unlock(&decoder->control);
	return ret;
}
