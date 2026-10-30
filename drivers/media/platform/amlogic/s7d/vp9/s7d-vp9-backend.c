// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "../common/s7d-codec-dos.h"
#include "../common/s7d-codec-firmware.h"
#include "../common/s7d-hevc-engine.h"
#include "../common/s7d-v4l2.h"
#include "s7d-vp9-backend.h"
#include "s7d-vp9-dma-context.h"
#include "s7d-vp9-input.h"
#include "s7d-vp9-inter-picture.h"
#include "s7d-vp9-protocol.h"

#define VP9_REG(word) ((word) * 4U)
#define VP9_MAILBOX_CLEAR VP9_REG(0x3071)
#define VP9_STATUS VP9_REG(0x30c0)
#define VP9_ADAPT_PROB VP9_REG(0x30c3)
#define VP9_COUNT VP9_REG(0x30d6)
#define VP9_SIZE VP9_REG(0x30d7)
#define VP9_SHIFT_BYTES VP9_REG(0x310d)
#define VP9_HEADER_DONE 0xf0U
#define VP9_PICTURE_DONE 0x0aU
#define VP9_ACTION_DONE 0xffU
#define VP9_TIMEOUT_MS 2000U

struct vp9_snapshot {
	u32 status;
	u32 shift_bytes;
	u32 adapt;
	int error;
};

struct vp9_job {
	struct s7d_codec_session *session;
	struct s7d_vp9_input_bank *bank;
	struct s7d_vp9_input *prepared;
	struct s7d_vp9_input_plan input;
	struct s7d_vp9_header_prefix header;
	struct s7d_vp9_dpb_plan picture;
	struct s7d_vp9_rpm rpm;
	struct vp9_snapshot snapshot;
	struct completion wake;
	u64 epoch;
	u64 token;
	u64 write_token;
	u64 timestamp_ns;
	struct s7d_codec_timestamp_metadata timestamp_meta;
	u32 current_slot;
	int status;
	bool rpm_valid;
	bool staged;
	bool context_begun;
	bool acquired;
	bool pinned;
	bool planned;
	bool header_waiting;
	bool header_armed;
	bool picture_complete;
	bool published;
	bool finished;
};

struct s7d_vp9_decoder {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_dos *dos;
	struct s7d_codec_firmware_owner *firmware;
	struct s7d_codec_firmware_job *preload;
	struct s7d_hevc_engine *engine;
	struct s7d_codec_core *core;
	struct regmap *map;
	struct mutex control; /* Stream and synchronous executor lifetime. */
	struct mutex protocol; /* Firmware pause and picture programming. */
	spinlock_t irq_lock; /* Job publication, stop and QBUF wake. */
	struct completion idle;
	struct vp9_job *job;
	struct s7d_codec_session *session;
	struct s7d_vp9_dma_context *context;
	struct s7d_vp9_dpb dpb;
	struct s7d_codec_layout layout;
	struct s7d_v4l2_capture_buffer capture[S7D_VP9_MAX_PHYSICAL];
	struct s7d_vp9_hw_surface surface[S7D_VP9_MAX_PHYSICAL];
	struct s7d_vp9_region mv[2];
	u64 queue_serial[S7D_VP9_MAX_PHYSICAL];
	u64 content_token[S7D_VP9_MAX_PHYSICAL];
	u64 epoch;
	u64 firmware_epoch;
	u64 binding;
	u64 generation;
	u64 last_token;
	u32 captures;
	u32 queued_mask;
	u32 pin_mask;
	u32 hidden_mask;
	u32 decode_count;
	u32 output_sequence;
	u32 mv_bank;
	int failure;
	int release_error;
	bool previous_valid;
	bool previous_keyframe;
	bool previous_intra_only;
	bool previous_show_frame;
	bool firmware_loaded;
	bool context_ready;
	bool irq_accept;
	bool stopping;
	bool running;
	bool closing;
	bool quiesced;
	bool quarantined;
	bool lease_active;
	bool release_pin;
};

static int vp9_stop_check(struct s7d_vp9_decoder *decoder)
{
	return READ_ONCE(decoder->stopping) || READ_ONCE(decoder->closing) ?
		-ECANCELED : 0;
}

static bool vp9_cookie_matches(struct s7d_vp9_decoder *decoder,
			       struct vp9_job *job, const struct s7d_hevc_irq_cookie *cookie)
{
	return job && cookie->session == job->session && cookie->binding == decoder->binding &&
		cookie->lease_epoch == job->epoch && cookie->input_token == job->token;
}

static irqreturn_t vp9_irq(void *priv, const struct s7d_hevc_irq_cookie *cookie)
{
	struct s7d_vp9_decoder *decoder = priv;
	struct vp9_snapshot snapshot = { };
	struct vp9_job *job;
	unsigned long flags;
	u32 fresh;
	int ret;

	spin_lock_irqsave(&decoder->irq_lock, flags);
	job = decoder->irq_accept ? decoder->job : NULL;
	if (!vp9_cookie_matches(decoder, job, cookie))
		job = NULL;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (!job)
		return IRQ_HANDLED;
	ret = regmap_write(decoder->map, VP9_MAILBOX_CLEAR, 1);
	if (!ret)
		ret = regmap_read(decoder->map, VP9_STATUS, &snapshot.status);
	if (!ret && (snapshot.status == VP9_ACTION_DONE || !snapshot.status ||
		     snapshot.status == 5))
		return IRQ_HANDLED;
	if (!ret)
		ret = regmap_read(decoder->map, VP9_SHIFT_BYTES, &snapshot.shift_bytes);
	if (!ret)
		ret = regmap_read(decoder->map, VP9_ADAPT_PROB, &snapshot.adapt);
	if (!ret)
		ret = regmap_read(decoder->map, VP9_STATUS, &fresh);
	if (!ret && fresh != snapshot.status)
		ret = -ESTALE;
	snapshot.error = ret;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	job->snapshot = snapshot;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	return IRQ_WAKE_THREAD;
}

static int vp9_acquire_surface(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	unsigned long flags;
	u32 available, slot, owned;
	u64 serial;
	int ret;

	ret = s7d_vp9_dpb_owned_mask(&decoder->dpb, &owned);
	if (ret || owned != decoder->pin_mask)
		return ret ?: -EPROTO;
	for (;;) {
		spin_lock_irqsave(&decoder->irq_lock, flags);
		available = decoder->queued_mask & ~decoder->pin_mask;
		slot = available ? __ffs(available) : S7D_VP9_NO_PHYSICAL;
		serial = available ? decoder->queue_serial[slot] : 0;
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
		if (!available)
			return -EAGAIN;
		ret = vp9_stop_check(decoder);
		if (ret)
			return ret;
		ret = s7d_codec_surface_acquire(decoder->capture[slot].surface, &job->write_token);
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
		decoder->content_token[slot] = 0;
		job->acquired = true;
		spin_lock_irqsave(&decoder->irq_lock, flags);
		decoder->queued_mask &= ~BIT(slot);
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
		ret = s7d_codec_surface_ref_get(decoder->capture[slot].surface);
		if (ret)
			return ret;
		decoder->pin_mask |= BIT(slot);
		job->pinned = true;
		return 0;
	}
}

static int vp9_acquire_current(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	int ret = vp9_acquire_surface(decoder, job);

	if (!ret)
		ret = s7d_vp9_dpb_begin(&decoder->dpb, &job->header,
					job->current_slot, job->write_token, &job->picture);
	if (!ret)
		job->planned = true;
	return ret;
}

static void vp9_normalize_rpm(struct vp9_job *job)
{
	job->rpm.word[6] = job->header.intra_only;
	job->rpm.word[8] = job->header.reset_frame_context;
	job->rpm.word[9] = job->header.refresh_flags;
	job->rpm.word[10] = job->header.width;
	job->rpm.word[11] = job->header.height;
	job->rpm.word[7] = 1;
	job->rpm.word[12] = job->header.render_width;
	job->rpm.word[13] = job->header.render_height;
}

static int vp9_header(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	struct s7d_vp9_picture_frame frame = {
		.workspace = s7d_vp9_dma_context_workspace(decoder->context),
		.header = &job->header, .rpm = &job->rpm, .layout = &decoder->layout,
		.surface = decoder->surface, .physical_count = decoder->captures,
		.coded_frames = 1, .current_mv = decoder->mv[decoder->mv_bank],
		.previous_mv = decoder->mv[decoder->mv_bank ^ 1],
		.previous_valid = decoder->previous_valid,
		.previous_keyframe = decoder->previous_keyframe,
		.previous_intra_only = decoder->previous_intra_only,
		.previous_show_frame = decoder->previous_show_frame,
		.previous_width = decoder->layout.coded_width,
		.previous_height = decoder->layout.coded_height,
	};
	u32 status;
	int ret;

	if (job->header_armed)
		return -EOPNOTSUPP;
	ret = vp9_stop_check(decoder);
	if (!ret)
		ret = regmap_read(decoder->map, VP9_STATUS, &status);
	if (!ret && status != VP9_HEADER_DONE)
		ret = -ESTALE;
	if (!ret && !job->rpm_valid) {
		ret = s7d_vp9_dma_context_snapshot_rpm(decoder->context, job->epoch,
						      job->token, &job->rpm);
		if (!ret)
			ret = s7d_vp9_input_check_rpm(job->prepared, &job->rpm);
		if (!ret) {
			vp9_normalize_rpm(job);
			job->rpm_valid = true;
		}
	}
	if (!ret)
		ret = vp9_acquire_current(decoder, job);
	if (ret)
		return ret;
	frame.current_physical = job->current_slot;
	memcpy(frame.logical_to_physical, job->picture.logical_to_physical,
	       sizeof(frame.logical_to_physical));
	ret = s7d_vp9_inter_picture_configure(decoder->map, &frame);
	if (!ret)
		ret = vp9_stop_check(decoder);
	if (!ret)
		ret = regmap_read(decoder->map, VP9_STATUS, &status);
	if (!ret && status != VP9_HEADER_DONE)
		ret = -ESTALE;
	if (!ret) {
		dma_wmb();
		ret = regmap_write(decoder->map, VP9_STATUS, 5);
	}
	if (!ret) {
		job->header_armed = true;
		job->header_waiting = false;
	}
	return ret;
}

static irqreturn_t vp9_irq_thread(void *priv, const struct s7d_hevc_irq_cookie *cookie)
{
	struct s7d_vp9_decoder *decoder = priv;
	struct vp9_snapshot snapshot;
	struct vp9_job *job;
	unsigned long flags;
	int ret;

	mutex_lock(&decoder->protocol);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	job = decoder->irq_accept ? decoder->job : NULL;
	if (!vp9_cookie_matches(decoder, job, cookie))
		job = NULL;
	if (job)
		snapshot = job->snapshot;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (!job || job->finished)
		goto out;
	ret = snapshot.error ?: vp9_stop_check(decoder);
	if (!ret && (snapshot.adapt & 0xff) == 0xfd)
		ret = -EOPNOTSUPP;
	if (!ret && snapshot.status == VP9_HEADER_DONE) {
		ret = vp9_header(decoder, job);
		if (ret == -EAGAIN) {
			job->header_waiting = true;
			complete_all(&job->wake);
			goto out;
		}
		if (!ret)
			goto out;
	} else if (!ret && snapshot.status == VP9_PICTURE_DONE) {
		if (!job->header_armed || !job->planned || !snapshot.shift_bytes ||
		    snapshot.shift_bytes > job->input.max_shift_bytes)
			ret = -EPROTO;
		else
			job->picture_complete = true;
	} else if (!ret) {
		ret = -EBADMSG;
	}
	job->status = ret;
	job->finished = true;
	complete_all(&job->wake);
out:
	mutex_unlock(&decoder->protocol);
	return IRQ_HANDLED;
}

static const struct s7d_hevc_irq_ops vp9_irq_ops = {
	.irq = vp9_irq, .thread = vp9_irq_thread,
};

static int vp9_halt_locked(struct s7d_vp9_decoder *decoder)
{
	unsigned long flags;
	int ret, joined;

	lockdep_assert_held(&decoder->control);
	if (decoder->quarantined)
		return -EIO;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->irq_accept = false;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	ret = s7d_codec_dos_prepare_access(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_codec_dos_hevc_irq_mask(decoder->dos, false);
	if (decoder->binding) {
		joined = s7d_hevc_engine_disarm(decoder->engine, decoder->binding);
		if (!ret)
			ret = joined;
	}
	mutex_lock(&decoder->protocol);
	if (!ret && !decoder->quiesced)
		ret = s7d_codec_dos_halt(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	decoder->quiesced = !ret;
	mutex_unlock(&decoder->protocol);
	if (ret) {
		decoder->quarantined = true;
		if (decoder->binding)
			s7d_hevc_engine_quarantine(decoder->engine, decoder->binding);
	}
	return ret;
}

void s7d_vp9_decoder_request_stop(struct s7d_vp9_decoder *decoder)
{
	unsigned long flags;

	if (!decoder)
		return;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->stopping = true;
	if (decoder->preload)
		s7d_codec_firmware_job_stop(decoder->preload);
	if (decoder->job)
		complete_all(&decoder->job->wake);
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
}

static int vp9_hold(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_vp9_decoder *decoder = priv;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	if (READ_ONCE(decoder->closing) || decoder->quarantined || decoder->release_error)
		ret = -ESHUTDOWN;
	else if (decoder->epoch == U64_MAX)
		ret = -EOVERFLOW;
	else
		ret = s7d_codec_dos_hold(decoder->dos, engine);
	if (!ret) {
		decoder->epoch++;
		decoder->lease_active = true;
		decoder->quiesced = false;
		decoder->stopping = false;
		decoder->firmware_loaded = false;
		decoder->firmware_epoch = 0;
		decoder->failure = 0;
	}
	return ret;
}

static int vp9_unbind_locked(struct s7d_vp9_decoder *decoder)
{
	int ret;

	if (!decoder->binding)
		return 0;
	ret = s7d_hevc_engine_unbind(decoder->engine, decoder->binding);
	if (!ret)
		decoder->binding = 0;
	else
		decoder->quarantined = true;
	return ret;
}

static int vp9_quiesce(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_vp9_decoder *decoder = priv;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	s7d_vp9_decoder_request_stop(decoder);
	mutex_lock(&decoder->control);
	while (decoder->running) {
		mutex_unlock(&decoder->control);
		wait_for_completion(&decoder->idle);
		mutex_lock(&decoder->control);
	}
	ret = vp9_halt_locked(decoder);
	if (!ret)
		ret = vp9_unbind_locked(decoder);
	decoder->firmware_loaded = false;
	mutex_unlock(&decoder->control);
	return ret;
}

static void vp9_release(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_vp9_decoder *decoder = priv;
	int ret;

	mutex_lock(&decoder->control);
	ret = engine != S7D_CODEC_ENGINE_HEVC || !decoder->quiesced ? -EBUSY :
		s7d_codec_dos_release(decoder->dos, engine);
	if (!ret)
		decoder->lease_active = false;
	else {
		decoder->release_error = ret;
		if (!decoder->release_pin && try_module_get(decoder->owner))
			decoder->release_pin = true;
		dev_err(decoder->dev, "VP9 release failed: %d; owner retained\n", ret);
	}
	mutex_unlock(&decoder->control);
}

static const struct s7d_codec_resource_ops vp9_resource_ops = {
	.hold = vp9_hold, .quiesce = vp9_quiesce, .release = vp9_release,
};

int s7d_vp9_decoder_bind(struct s7d_vp9_decoder *decoder,
			 struct s7d_codec_session *session,
			 const struct s7d_codec_layout *layout, u64 generation,
			 const struct s7d_v4l2_capture_buffer *capture, u32 count)
{
	struct s7d_vp9_region region[2];
	unsigned long flags;
	u32 i, j;
	int ret;

	if (!decoder || !session || !layout || !capture || !generation)
		return -EINVAL;
	mutex_lock(&decoder->control);
	if (decoder->closing || decoder->running || decoder->session || decoder->quarantined ||
	    decoder->failure || !decoder->lease_active) {
		ret = decoder->failure ?: -EBUSY;
		goto out;
	}
	ret = s7d_codec_validate_layout(layout);
	if (!ret)
		ret = s7d_vp9_dpb_init(&decoder->dpb, generation, count);
	if (ret)
		goto out;
	decoder->context = s7d_vp9_dma_context_create_stream(session, layout->coded_width,
		layout->coded_height, S7D_VP9_INPUT_MAX_BYTES, decoder->epoch);
	if (IS_ERR(decoder->context)) {
		ret = PTR_ERR(decoder->context);
		decoder->context = NULL;
		goto out;
	}
	decoder->session = session;
	decoder->layout = *layout;
	decoder->generation = generation;
	for (i = 0; i < count; i++) {
		if (!capture[i].surface || capture[i].generation != generation) {
			ret = -EINVAL;
			goto failed;
		}
		for (j = 0; j < i; j++)
			if (capture[j].surface == capture[i].surface ||
			    capture[j].surface_id == capture[i].surface_id) {
				ret = -EINVAL;
				goto failed;
			}
		ret = s7d_vp9_dma_context_bind_surface(decoder->context, decoder->epoch,
						       capture[i].surface, layout, region);
		if (ret)
			goto failed;
		decoder->surface[i] = (struct s7d_vp9_hw_surface) {
			.plane = { region[0], region[1] }, .width = layout->coded_width,
			.height = layout->coded_height, .valid = true,
		};
	}
	for (i = 0; i < 2; i++) {
		ret = s7d_vp9_dma_context_mv_slot(decoder->context, decoder->epoch, i,
						 &decoder->mv[i]);
		if (ret)
			goto failed;
	}
	ret = s7d_hevc_engine_bind(decoder->engine, session, decoder->epoch,
				  &vp9_irq_ops, decoder, &decoder->binding);
	if (ret)
		goto failed;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->captures = count;
	memcpy(decoder->capture, capture, sizeof(*capture) * count);
	decoder->queued_mask = GENMASK(count - 1, 0);
	for (i = 0; i < count; i++)
		decoder->queue_serial[i] = 1;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	ret = 0;
	goto out;
failed:
	decoder->failure = ret;
out:
	mutex_unlock(&decoder->control);
	return ret;
}

int s7d_vp9_decoder_capture_queue(struct s7d_vp9_decoder *decoder,
				  struct s7d_codec_surface *surface,
				  u64 surface_id, u64 generation)
{
	unsigned long flags;
	u32 i;
	int ret = -ESTALE;

	if (!decoder || !surface || !generation)
		return -EINVAL;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	for (i = 0; i < decoder->captures; i++) {
		if (decoder->capture[i].surface != surface ||
		    decoder->capture[i].surface_id != surface_id || generation != decoder->generation)
			continue;
		if (decoder->queue_serial[i] == U64_MAX) {
			ret = -EOVERFLOW;
			break;
		}
		decoder->queue_serial[i]++;
		decoder->queued_mask |= BIT(i);
		if (decoder->job)
			complete_all(&decoder->job->wake);
		ret = 0;
		break;
	}
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	return ret;
}

static int vp9_preload(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	struct s7d_codec_firmware_job *preload;
	unsigned long flags;
	bool cancelled = false, queued;
	int ret;

	if (decoder->firmware_loaded)
		return 0;
	if (!decoder->firmware_epoch) {
		ret = s7d_codec_firmware_next_epoch(decoder->firmware, &decoder->firmware_epoch);
		if (ret)
			return ret;
	}
	ret = s7d_codec_firmware_prepare_for_engine(decoder->firmware, job->session,
		decoder->firmware_epoch, S7D_CODEC_ENGINE_HEVC, S7D_CODEC_FIRMWARE_VP9, &preload);
	if (ret || !preload)
		return ret ?: vp9_stop_check(decoder);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->preload = preload;
	if (decoder->stopping || decoder->closing)
		s7d_codec_firmware_job_stop(preload);
	queued = s7d_codec_firmware_job_queue(preload);
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (!queued) {
		decoder->quarantined = true;
		return -EIO;
	}
	mutex_unlock(&decoder->control);
	ret = s7d_codec_firmware_job_join_result(preload, job->session,
						decoder->firmware_epoch, &cancelled);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->preload = NULL;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	s7d_codec_firmware_job_release_custody(preload);
	s7d_codec_firmware_job_put(preload);
	mutex_lock(&decoder->control);
	if (decoder->job != job || decoder->epoch != job->epoch)
		return -ESTALE;
	return ret ?: vp9_stop_check(decoder);
}

static int vp9_start(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	struct reg_sequence regs[] = {
		{ VP9_STATUS, VP9_ACTION_DONE }, { VP9_SHIFT_BYTES, 0 },
		{ VP9_SIZE, job->input.decode_size }, { VP9_COUNT, decoder->decode_count },
	};
	unsigned long flags;
	u32 status;
	int ret;

	ret = vp9_stop_check(decoder);
	if (!ret)
		ret = regmap_multi_reg_write(decoder->map, regs, ARRAY_SIZE(regs));
	if (!ret)
		ret = regmap_read(decoder->map, VP9_STATUS, &status);
	if (!ret && status != VP9_ACTION_DONE)
		ret = -EIO;
	if (!ret)
		ret = regmap_write(decoder->map, VP9_MAILBOX_CLEAR, 1);
	if (ret)
		return ret;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->irq_accept = true;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	ret = s7d_hevc_engine_arm(decoder->engine, decoder->binding, job->token);
	if (!ret)
		ret = s7d_codec_dos_hevc_irq_mask(decoder->dos, true);
	if (!ret)
		ret = vp9_stop_check(decoder);
	if (!ret)
		ret = s7d_codec_dos_hevc_input_enable(decoder->dos);
	if (!ret) {
		dma_wmb();
		ret = s7d_codec_dos_start(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	}
	return ret;
}

static int vp9_wait(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(VP9_TIMEOUT_MS);
	bool finished;
	int ret;

	for (;;) {
		if (READ_ONCE(job->header_waiting))
			wait_for_completion(&job->wake);
		else if (time_after_eq(jiffies, deadline) ||
			 !wait_for_completion_timeout(&job->wake, deadline - jiffies))
			return -ETIMEDOUT;
		reinit_completion(&job->wake);
		mutex_lock(&decoder->protocol);
		ret = vp9_stop_check(decoder);
		if (!ret && job->finished)
			ret = job->status;
		else if (!ret && job->header_waiting) {
			ret = vp9_header(decoder, job);
			if (ret == -EAGAIN)
				ret = 0;
			else if (!ret)
				deadline = jiffies + msecs_to_jiffies(VP9_TIMEOUT_MS);
		}
		finished = job->finished;
		mutex_unlock(&decoder->protocol);
		if (ret || finished)
			return ret;
	}
}

static int vp9_frame_ready(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	struct s7d_codec_frame frame = {
		.surface_id = decoder->capture[job->current_slot].surface_id,
		.generation = decoder->generation, .write_token = job->write_token,
		.timestamp_ns = job->timestamp_ns, .timestamp_meta = job->timestamp_meta,
		.sequence = decoder->output_sequence,
		.flags = job->header.show_existing ? 0 :
			 job->header.keyframe ? S7D_CODEC_FRAME_KEY : S7D_CODEC_FRAME_P,
		.bytesused = { decoder->layout.size[0], decoder->layout.size[1] },
	};

	return s7d_codec_session_frame_ready(job->session, &frame);
}

static int vp9_publish(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	unsigned long flags;
	u32 i;
	int ret;

	ret = s7d_vp9_input_check_current(job->bank, job->prepared);
	if (!ret)
		ret = s7d_codec_surface_written(decoder->capture[job->current_slot].surface,
						job->write_token);
	if (ret)
		return ret;
	job->acquired = false;
	decoder->content_token[job->current_slot] = job->write_token;
	if (job->header.show_frame) {
		ret = vp9_frame_ready(decoder, job);
		if (ret)
			return ret;
		job->published = true;
	} else {
		decoder->hidden_mask |= BIT(job->current_slot);
	}
	for (i = 0; i < decoder->captures; i++) {
		if (!(job->picture.release_mask & BIT(i)))
			continue;
		if (decoder->hidden_mask & BIT(i)) {
			ret = s7d_codec_surface_discard(decoder->capture[i].surface,
							decoder->content_token[i]);
			if (ret)
				return ret;
			decoder->hidden_mask &= ~BIT(i);
			decoder->content_token[i] = 0;
			spin_lock_irqsave(&decoder->irq_lock, flags);
			decoder->queued_mask |= BIT(i);
			spin_unlock_irqrestore(&decoder->irq_lock, flags);
		} else {
			ret = s7d_codec_surface_ref_put(decoder->capture[i].surface);
			if (ret)
				return ret;
		}
		decoder->pin_mask &= ~BIT(i);
		if (i == job->current_slot)
			job->pinned = false;
	}
	ret = s7d_vp9_input_commit_metadata(job->bank, job->prepared);
	if (!ret)
		ret = s7d_vp9_dpb_commit(&decoder->dpb, job->picture.serial, job->write_token);
	if (!ret) {
		job->planned = false;
		if (job->published)
			decoder->output_sequence++;
	}
	return ret;
}

static int vp9_discard(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	unsigned long flags;
	int ret;

	if (job->published)
		return 0;
	if (job->pinned) {
		ret = job->acquired ?
			s7d_codec_surface_written(decoder->capture[job->current_slot].surface,
							job->write_token) : 0;
		if (!ret)
			ret = s7d_codec_surface_discard(decoder->capture[job->current_slot].surface,
							job->write_token);
		if (ret)
			return ret;
		job->pinned = false;
		decoder->hidden_mask &= ~BIT(job->current_slot);
		decoder->content_token[job->current_slot] = 0;
		decoder->pin_mask &= ~BIT(job->current_slot);
		spin_lock_irqsave(&decoder->irq_lock, flags);
		decoder->queued_mask |= BIT(job->current_slot);
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
	}
	return job->planned ? s7d_vp9_dpb_abort(&decoder->dpb, job->picture.serial,
						job->write_token) : 0;
}

static bool vp9_copy_cancelled(void *priv)
{
	return vp9_stop_check(priv) != 0;
}

static int vp9_present_existing(struct s7d_vp9_decoder *decoder, struct vp9_job *job)
{
	const size_t bytes[2] = { decoder->layout.size[0], decoder->layout.size[1] };
	unsigned long flags;
	u32 source;
	u64 source_token;
	int ret;

	ret = s7d_vp9_input_check_current(job->bank, job->prepared);
	if (ret)
		return ret;
	source = decoder->dpb.map[job->header.existing_index];
	if (source >= decoder->captures || !(decoder->pin_mask & BIT(source)) ||
	    !decoder->content_token[source])
		return -ENODATA;
	source_token = decoder->content_token[source];
	for (;;) {
		spin_lock_irqsave(&decoder->irq_lock, flags);
		reinit_completion(&job->wake);
		spin_unlock_irqrestore(&decoder->irq_lock, flags);
		ret = vp9_stop_check(decoder);
		if (!ret)
			ret = vp9_acquire_surface(decoder, job);
		if (ret != -EAGAIN)
			break;
		ret = vp9_stop_check(decoder);
		if (ret)
			return ret;
		wait_for_completion(&job->wake);
	}
	if (ret)
		return ret;
	ret = s7d_codec_surface_copy(decoder->capture[job->current_slot].surface,
		job->write_token, decoder->capture[source].surface, source_token,
		bytes, vp9_copy_cancelled, decoder);
	if (!ret)
		ret = vp9_stop_check(decoder);
	if (!ret)
		ret = s7d_codec_surface_written(decoder->capture[job->current_slot].surface,
						job->write_token);
	if (ret)
		return ret;
	job->acquired = false;
	decoder->content_token[job->current_slot] = job->write_token;
	ret = s7d_vp9_input_commit_metadata(job->bank, job->prepared);
	if (!ret)
		ret = vp9_frame_ready(decoder, job);
	if (ret)
		return ret;
	job->published = true;
	ret = s7d_codec_surface_ref_put(decoder->capture[job->current_slot].surface);
	if (!ret) {
		decoder->pin_mask &= ~BIT(job->current_slot);
		job->pinned = false;
		decoder->output_sequence++;
	}
	return ret;
}

int s7d_vp9_decoder_decode(struct s7d_vp9_decoder *decoder,
			   struct s7d_codec_session *session,
			   struct s7d_vp9_input_bank *bank, struct s7d_vp9_input *input,
			   u64 token, u64 timestamp_ns,
			   const struct s7d_codec_timestamp_metadata *timestamp_meta,
			   struct s7d_vp9_decode_result *result)
{
	const struct s7d_vp9_input_result *view = s7d_vp9_input_view(input);
	struct s7d_vp9_protocol_frame protocol;
	struct vp9_job *job = NULL;
	struct s7d_codec_session *put_session = NULL;
	unsigned long flags;
	const char *stage = "admission";
	bool load_failed = false;
	int ret, halted = 0, idle, cleanup;

	if (!decoder || !session || !bank || !view || !token || !timestamp_meta || !result)
		return -EINVAL;
	*result = (struct s7d_vp9_decode_result) { };
	mutex_lock(&decoder->control);
	if (decoder->closing || decoder->running || decoder->job || decoder->quarantined ||
	    decoder->failure || decoder->session != session || !decoder->context) {
		ret = decoder->failure ?: -EBUSY;
		goto out;
	}
	ret = vp9_stop_check(decoder);
	if (!ret)
		ret = s7d_vp9_input_check_current(bank, input);
	if (!ret && (token <= decoder->last_token || decoder->decode_count == U32_MAX ||
		     decoder->output_sequence == U32_MAX))
		ret = -ESTALE;
	if (ret)
		goto out;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out;
	}
	job->session = session;
	job->bank = bank;
	job->prepared = input;
	job->header = view->prefix;
	job->epoch = decoder->epoch;
	job->token = token;
	job->current_slot = S7D_VP9_NO_PHYSICAL;
	job->timestamp_ns = timestamp_ns;
	job->timestamp_meta = *timestamp_meta;
	job->status = -EINPROGRESS;
	init_completion(&job->wake);
	s7d_codec_session_get(session);
	reinit_completion(&decoder->idle);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->job = job;
	decoder->running = true;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	ret = vp9_stop_check(decoder);
	if (!ret) {
		stage = "initial-halt";
		ret = vp9_halt_locked(decoder);
	}
	if (!ret && !decoder->context_ready) {
		ret = s7d_vp9_dma_context_mark_idle(decoder->context, job->epoch, 0, 0, true, false);
		decoder->context_ready = !ret;
	}
	if (!ret && job->header.show_existing)
		goto stop;
	if (!ret) {
		stage = "stage-input";
		ret = s7d_vp9_dma_context_stage(decoder->context, view, job->epoch, token, &job->input);
		job->staged = !ret;
	}
	if (!ret) {
		ret = s7d_vp9_dma_context_begin(decoder->context, job->epoch, token);
		job->context_begun = !ret;
	}
	if (ret)
		goto stop;
	decoder->quiesced = false;
	stage = "prepare";
	ret = s7d_codec_dos_prepare(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_vp9_dma_context_program_input(decoder->context, decoder->map, job->epoch, token);
	if (!ret)
		ret = s7d_codec_firmware_set_policy(decoder->dev, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_codec_dos_port_enable(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret) {
		stage = "firmware-preload";
		ret = vp9_preload(decoder, job);
	}
	if (!ret && !decoder->firmware_loaded) {
		stage = "firmware-load";
		ret = s7d_codec_firmware_load(decoder->dev, S7D_CODEC_ENGINE_HEVC,
					      S7D_CODEC_FIRMWARE_VP9);
		decoder->firmware_loaded = !ret;
		load_failed = ret;
	}
	if (!ret)
		ret = s7d_codec_dos_hevc_init(decoder->dos);
	protocol = (struct s7d_vp9_protocol_frame) {
		.workspace = s7d_vp9_dma_context_workspace(decoder->context),
		.layout = &decoder->layout, .surface = decoder->surface,
		.physical_count = decoder->captures, .decode_count = decoder->decode_count,
	};
	mutex_lock(&decoder->protocol);
	if (!ret) {
		stage = "protocol";
		ret = s7d_vp9_protocol_init(decoder->map, &protocol);
	}
	if (!ret) {
		stage = "start";
		ret = vp9_start(decoder, job);
	}
	mutex_unlock(&decoder->protocol);
	if (!ret) {
		stage = "picture";
		ret = vp9_wait(decoder, job);
	}
stop:
	halted = vp9_halt_locked(decoder);
	if (job->staged) {
		idle = s7d_vp9_dma_context_mark_idle(decoder->context, job->epoch, token,
			halted, !halted, !ret && job->picture_complete);
		if (!halted && idle)
			halted = idle;
	}
	if (halted) {
		decoder->quarantined = true;
		if (decoder->binding)
			s7d_hevc_engine_quarantine(decoder->engine, decoder->binding);
		ret = ret ?: halted;
		goto retained;
	}
	if (!ret)
		ret = vp9_stop_check(decoder);
	if (!ret && !job->header.show_existing &&
	    (!job->picture_complete || !job->header_armed || !job->planned))
		ret = -EPROTO;
	if (!ret) {
		stage = job->header.show_existing ? "show-existing" : "publish";
		ret = job->header.show_existing ? vp9_present_existing(decoder, job) :
			vp9_publish(decoder, job);
		result->published_frames = job->published;
	}
	if (ret && !READ_ONCE(decoder->stopping)) {
		cleanup = vp9_discard(decoder, job);
		if (cleanup && cleanup != -ESHUTDOWN)
			ret = cleanup;
	}
	if (!ret) {
		decoder->last_token = token;
		if (!job->header.show_existing) {
			decoder->decode_count++;
			decoder->previous_valid = true;
			decoder->previous_keyframe = job->header.keyframe;
			decoder->previous_intra_only = job->header.intra_only;
			decoder->previous_show_frame = job->header.show_frame;
			decoder->mv_bank ^= 1;
		}
	} else {
		result->cancelled = ret == -ECANCELED ||
			(ret == -ESHUTDOWN && READ_ONCE(decoder->stopping));
		if (!result->cancelled)
			decoder->failure = ret;
		decoder->firmware_loaded = false;
		/* The owner is shared by every codec; only a failed TEE load concerns it. */
		if (load_failed)
			s7d_codec_firmware_invalidate(decoder->firmware);
	}
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->job = NULL;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	put_session = job->session;
	kfree(job);
	job = NULL;
	goto completed;
retained:
	job->bank = NULL;
	job->prepared = NULL;
	decoder->failure = ret;
	decoder->firmware_loaded = false;
	if (load_failed)
		s7d_codec_firmware_invalidate(decoder->firmware);
completed:
	if (ret && !result->cancelled)
		dev_err(decoder->dev,
			"VP9 input %llu epoch %llu stage=%s error=%d halt=%d retained=%u\n",
			token, decoder->epoch, stage, ret, halted, decoder->quarantined);
	decoder->running = false;
	complete_all(&decoder->idle);
out:
	mutex_unlock(&decoder->control);
	if (put_session)
		s7d_codec_session_put(put_session);
	return ret;
}

int s7d_vp9_decoder_drain(struct s7d_vp9_decoder *decoder,
			  struct s7d_codec_session *session, u32 *published, bool *cancelled)
{
	int ret;

	if (!decoder || !session || !published || !cancelled)
		return -EINVAL;
	*published = 0;
	*cancelled = false;
	mutex_lock(&decoder->control);
	ret = decoder->failure;
	if (!ret && (decoder->session != session || decoder->running || decoder->job ||
		     decoder->quarantined || !decoder->quiesced || decoder->dpb.active))
		ret = -EBUSY;
	if (!ret)
		ret = vp9_stop_check(decoder);
	*cancelled = ret == -ECANCELED;
	mutex_unlock(&decoder->control);
	return ret;
}

int s7d_vp9_decoder_stream_close(struct s7d_vp9_decoder *decoder,
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
	if (decoder->session != session || decoder->running || decoder->job || decoder->quarantined) {
		ret = -EBUSY;
		goto out;
	}
	ret = vp9_halt_locked(decoder);
	if (!ret)
		ret = vp9_unbind_locked(decoder);
	if (!ret && !decoder->context_ready) {
		ret = s7d_vp9_dma_context_mark_idle(decoder->context, decoder->epoch, 0, 0, true, false);
		decoder->context_ready = !ret;
	}
	if (!ret)
		ret = s7d_vp9_dma_context_destroy_stream(decoder->context, decoder->epoch);
	if (ret)
		goto out;
	decoder->context = NULL;
	decoder->context_ready = false;
	decoder->session = NULL;
	decoder->generation = 0;
	decoder->pin_mask = 0;
	decoder->last_token = 0;
	decoder->decode_count = 0;
	decoder->output_sequence = 0;
	decoder->mv_bank = 0;
	decoder->previous_valid = false;
	decoder->previous_intra_only = false;
	decoder->previous_show_frame = false;
	decoder->hidden_mask = 0;
	memset(decoder->content_token, 0, sizeof(decoder->content_token));
	decoder->firmware_loaded = false;
	memset(&decoder->dpb, 0, sizeof(decoder->dpb));
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->captures = 0;
	decoder->queued_mask = 0;
	memset(decoder->capture, 0, sizeof(decoder->capture));
	memset(decoder->queue_serial, 0, sizeof(decoder->queue_serial));
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
out:
	mutex_unlock(&decoder->control);
	return ret;
}

struct s7d_vp9_decoder *
s7d_vp9_decoder_create(struct device *dev, struct s7d_codec_dos *dos,
			struct s7d_codec_firmware_owner *firmware,
			struct s7d_hevc_engine *engine, struct module *owner)
{
	struct s7d_vp9_decoder *decoder;
	int ret;

	if (!dev || IS_ERR_OR_NULL(dos) || IS_ERR_OR_NULL(firmware) || IS_ERR_OR_NULL(engine))
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
	decoder->dos = dos;
	decoder->firmware = firmware;
	decoder->engine = engine;
	mutex_init(&decoder->control);
	mutex_init(&decoder->protocol);
	spin_lock_init(&decoder->irq_lock);
	init_completion(&decoder->idle);
	complete_all(&decoder->idle);
	decoder->map = s7d_codec_dos_regmap(dos, S7D_CODEC_ENGINE_HEVC);
	if (IS_ERR_OR_NULL(decoder->map)) {
		ret = decoder->map ? PTR_ERR(decoder->map) : -ENODEV;
		goto free;
	}
	decoder->core = s7d_codec_core_create(dev, owner, &vp9_resource_ops, decoder, 32);
	if (!IS_ERR(decoder->core))
		return decoder;
	ret = PTR_ERR(decoder->core);
free:
	put_device(decoder->dev);
	kfree(decoder);
	return ERR_PTR(ret);
}

struct s7d_codec_core *s7d_vp9_decoder_core(struct s7d_vp9_decoder *decoder)
{
	return decoder ? decoder->core : NULL;
}

int s7d_vp9_decoder_destroy(struct s7d_vp9_decoder *decoder)
{
	int ret;

	if (!decoder)
		return -EINVAL;
	WRITE_ONCE(decoder->closing, true);
	s7d_vp9_decoder_request_stop(decoder);
	mutex_lock(&decoder->control);
	if (decoder->running || decoder->job || decoder->session || decoder->context ||
	    decoder->binding || decoder->quarantined || decoder->release_error || decoder->release_pin) {
		ret = -EBUSY;
		goto out;
	}
	ret = s7d_codec_core_destroy(decoder->core);
	if (ret)
		goto out;
	mutex_unlock(&decoder->control);
	put_device(decoder->dev);
	kfree(decoder);
	return 0;
out:
	mutex_unlock(&decoder->control);
	return ret;
}
