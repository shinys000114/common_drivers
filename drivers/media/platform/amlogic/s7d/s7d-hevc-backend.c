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
#include "s7d-hevc-picture.h"
#include "s7d-hevc-rpm.h"
#include "s7d-hevc-sequence.h"

#define HEVC_REG(word) ((word) * 4U)
#define HEVC_MAILBOX_CLEAR HEVC_REG(0x3071)
#define HEVC_STATUS HEVC_REG(0x30c0)
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
	int error;
};

struct s7d_hevc_job {
	struct s7d_codec_session *session;
	struct s7d_codec_surface *surface;
	struct s7d_hevc_context *context;
	struct s7d_hevc_sps sps;
	struct s7d_codec_layout layout;
	struct s7d_hevc_surface capture;
	struct s7d_hevc_input_plan input;
	struct s7d_hevc_idr_plan picture;
	struct s7d_hevc_irq_snapshot snapshot;
	struct s7d_hevc_decode_result result;
	struct completion done;
	u64 generation;
	u64 surface_id;
	u64 lease_epoch;
	u64 firmware_epoch;
	u64 write_token;
	u64 timestamp_ns;
	u32 opaque_swap;
	u32 headers;
	int status;
	bool header_armed;
	bool picture_complete;
	bool dpb_pin;
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
	struct mutex control; /* Serializes execution and session teardown. */
	struct mutex protocol; /* Serializes threaded IRQ and firmware start. */
	struct completion idle;
	spinlock_t irq_lock; /* Protects published job, IRQ snapshot and cancellation. */
	struct s7d_hevc_job *job;
	u64 epoch;
	u64 firmware_epoch;
	int irq;
	int release_error;
	bool irq_accept;
	bool irq_enabled;
	bool stop_requested;
	bool closing;
	bool quiesced;
	bool quarantined;
	bool release_pin;
	bool lease_active;
	bool lease_used;
	bool running;
};

static int hevc_check_stop(struct s7d_hevc_decoder *decoder)
{
	return READ_ONCE(decoder->stop_requested) || READ_ONCE(decoder->closing) ?
		-ECANCELED : 0;
}

static void hevc_finish(struct s7d_hevc_job *job, int error)
{
	if (job->finished)
		return;
	job->status = error;
	job->finished = true;
	complete_all(&job->done);
}

static irqreturn_t hevc_irq(int irq, void *data)
{
	struct s7d_hevc_decoder *decoder = data;
	struct s7d_hevc_irq_snapshot snapshot = { };
	struct s7d_hevc_job *job;
	unsigned long flags;
	u32 status;
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
		ret = regmap_read(decoder->map, HEVC_STATUS, &status);
	if (!ret && status != snapshot.status)
		ret = -ESTALE;
	snapshot.error = ret;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	job->snapshot = snapshot;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	return IRQ_WAKE_THREAD;
}

static int hevc_header(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job)
{
	struct s7d_hevc_rpm rpm;
	u8 raw[S7D_HEVC_RPM_BYTES];
	u32 status, lmem;
	int ret;

	if (job->headers || job->header_armed)
		return -EOPNOTSUPP;
	ret = regmap_read(decoder->map, HEVC_STATUS, &status);
	if (!ret && status != S7D_HEVC_SLICE_HEADER_DONE)
		ret = -ESTALE;
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_LMEM_DMA_CONTROL, &lmem);
	if (!ret && (lmem & BIT(15)))
		ret = -EBUSY;
	if (!ret) {
		dma_rmb();
		ret = s7d_hevc_context_snapshot_rpm(job->context, raw, sizeof(raw));
	}
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_STATUS, &status);
	if (!ret && status != S7D_HEVC_SLICE_HEADER_DONE)
		ret = -ESTALE;
	if (!ret)
		ret = s7d_hevc_rpm_parse(raw, sizeof(raw), true, &rpm);
	if (!ret)
		ret = s7d_hevc_rpm_plan_idr(&rpm, &job->sps, &job->picture);
	if (!ret)
		ret = s7d_hevc_picture_configure_idr(decoder->map,
				s7d_hevc_context_workspace(job->context), &rpm,
				&job->sps, &job->layout, &job->capture);
	if (!ret)
		ret = regmap_update_bits(decoder->map, HEVC_WAIT_FLAG, BIT(1), BIT(1));
	if (!ret)
		ret = hevc_check_stop(decoder);
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
	int ret;

	mutex_lock(&decoder->protocol);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	job = decoder->irq_accept ? decoder->job : NULL;
	if (job)
		snapshot = job->snapshot;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (!job || job->finished)
		goto out;
	ret = hevc_check_stop(decoder);
	if (!ret)
		ret = snapshot.error;
	if (!ret)
		ret = s7d_hevc_classify_status(snapshot.status, &event);
	if (ret)
		goto failed;
	if (event == S7D_HEVC_EVENT_SLICE_HEADER) {
		ret = hevc_header(decoder, job);
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
	ret = s7d_hevc_check_picture_done(&job->picture, &done);
	if (ret)
		goto failed;
	job->result.consumed_bytes = job->input.decode_size;
	job->picture_complete = true;
	hevc_finish(job, 0);
	goto out;
failed:
	hevc_finish(job, ret);
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
		decoder->lease_used = false;
		spin_lock_irqsave(&decoder->irq_lock, flags);
		decoder->stop_requested = false;
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
	if (decoder->job)
		decoder->job->opaque_swap = 0;
	decoder->quiesced = !ret;
	decoder->quarantined = !!ret;
	mutex_unlock(&decoder->protocol);
	return ret;
}

static int hevc_quiesce(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_hevc_decoder *decoder = priv;
	int ret;

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
		return ret ? ret : hevc_check_stop(decoder);
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->preload = preload;
	if (decoder->stop_requested || decoder->closing)
		s7d_codec_firmware_job_stop(preload);
	queued = s7d_codec_firmware_job_queue(preload);
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	if (queued) {
		mutex_unlock(&decoder->control);
		ret = s7d_codec_firmware_job_join(preload);
		mutex_lock(&decoder->control);
		s7d_codec_firmware_retire(decoder->firmware, preload);
	} else {
		ret = -EIO;
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
	return ret ? ret : hevc_check_stop(decoder);
}

static int hevc_protocol(struct s7d_hevc_decoder *decoder, struct s7d_hevc_job *job)
{
	const struct s7d_hevc_workspace *workspace =
		s7d_hevc_context_workspace(job->context);
	const struct reg_sequence regs[] = {
		{ HEVC_PSCALE_CONTROL, 0 },
		{ HEVC_COUNT, 0 },
		{ HEVC_MODE2, 0 },
		{ HEVC_NAL_SEARCH, BIT(21) | BIT(22) },
		{ HEVC_MODE, 0x80000001 },
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
		{ HEVC_COUNT, 0 },
	};
	unsigned long flags;
	int ret;

	ret = hevc_check_stop(decoder);
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
		ret = hevc_check_stop(decoder);
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
	if (decoder->job)
		complete_all(&decoder->job->done);
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
}

int s7d_hevc_decoder_decode_surface(struct s7d_hevc_decoder *decoder,
				    struct s7d_codec_session *session,
				    const void *annexb, size_t bytes,
				    struct s7d_codec_surface *surface,
				    u64 surface_id, u64 generation,
				    u64 timestamp_ns,
				    struct s7d_hevc_decode_result *result)
{
	struct s7d_codec_frame frame;
	struct s7d_hevc_job *job;
	dma_addr_t address;
	size_t size;
	unsigned long flags;
	u32 i;
	int ret, stopped;

	if (!decoder || !annexb || !session || !surface || !generation || !result)
		return -EINVAL;
	mutex_lock(&decoder->control);
	if (READ_ONCE(decoder->closing) || decoder->job || decoder->quarantined ||
	    decoder->release_error) {
		ret = READ_ONCE(decoder->closing) ? -ESHUTDOWN : -EBUSY;
		goto out;
	}
	if (!decoder->lease_active || decoder->lease_used || decoder->quiesced) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = hevc_check_stop(decoder);
	if (ret)
		goto out;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out;
	}
	reinit_completion(&decoder->idle);
	decoder->running = true;
	init_completion(&job->done);
	job->status = -EINPROGRESS;
	job->timestamp_ns = timestamp_ns;
	ret = s7d_hevc_input_validate_annexb(annexb, bytes, &job->sps);
	if (!ret)
		ret = s7d_hevc_build_layout(&job->sps, S7D_CODEC_CAPTURE_NV12M, 0,
					 &job->layout);
	if (ret)
		goto free_job;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->job = job;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
	job->session = session;
	s7d_codec_session_get(session);
	job->generation = generation;
	job->surface_id = surface_id;
	job->lease_epoch = decoder->epoch;
	job->surface = surface;
	decoder->lease_used = true;
	job->context = s7d_hevc_context_create(job->session, &job->sps, bytes);
	if (IS_ERR(job->context)) {
		ret = PTR_ERR(job->context);
		job->context = NULL;
		goto stop;
	}
	ret = s7d_codec_surface_acquire(job->surface, &job->write_token);
	if (!ret)
		ret = s7d_codec_surface_ref_get(job->surface);
	if (ret)
		goto stop;
	job->dpb_pin = true;
	for (i = 0; i < 2; i++) {
		ret = s7d_codec_surface_dma(job->surface, i, &address, &size);
		if (ret)
			goto stop;
		if (address > U32_MAX || size > U32_MAX) {
			ret = -ERANGE;
			goto stop;
		}
		job->capture.address[i] = address;
		job->capture.bytes[i] = size;
	}
	ret = s7d_hevc_context_stage(job->context, annexb, bytes, job->lease_epoch,
				    &job->input);
	if (ret)
		goto stop;
	ret = s7d_codec_dos_prepare(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_hevc_context_program_input(job->context, decoder->map);
	if (!ret)
		ret = s7d_codec_dos_port_enable(decoder->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = hevc_preload(decoder, job);
	if (ret)
		goto stop;
	ret = s7d_codec_firmware_load(decoder->dev, S7D_CODEC_ENGINE_HEVC,
					      S7D_CODEC_FIRMWARE_HEVC);
	if (ret) {
		s7d_codec_firmware_invalidate(decoder->firmware);
		goto stop;
	}
	ret = hevc_check_stop(decoder);
	if (!ret)
		ret = regmap_read(decoder->map, HEVC_SWAP_TOKEN, &job->opaque_swap);
	mutex_lock(&decoder->protocol);
	if (!ret)
		ret = hevc_protocol(decoder, job);
	if (!ret)
		ret = hevc_start(decoder, job);
	mutex_unlock(&decoder->protocol);
	if (ret)
		goto stop;
	if (!wait_for_completion_timeout(&job->done, msecs_to_jiffies(HEVC_TIMEOUT_MS)))
		ret = -ETIMEDOUT;
	mutex_lock(&decoder->protocol);
	if (!ret)
		ret = hevc_check_stop(decoder);
	if (!ret)
		ret = job->finished ? job->status : -ECANCELED;
	mutex_unlock(&decoder->protocol);
	stopped = hevc_quiesce_locked(decoder, S7D_CODEC_ENGINE_HEVC);
	if (stopped) {
		ret = ret ? ret : stopped;
		goto stop;
	}
	if (!ret && !job->picture_complete)
		ret = -EPROTO;
	if (!ret)
		ret = hevc_check_stop(decoder);
	if (ret)
		goto stop;
	ret = s7d_codec_surface_ref_put(job->surface);
	if (ret)
		goto stop;
	job->dpb_pin = false;
	ret = s7d_codec_surface_written(job->surface, job->write_token);
	if (ret)
		goto stop;
	frame = (struct s7d_codec_frame) {
		.surface_id = job->surface_id, .generation = job->generation,
		.write_token = job->write_token, .timestamp_ns = timestamp_ns,
		.sequence = 0, .flags = S7D_CODEC_FRAME_KEY,
		.bytesused = { job->layout.size[0], job->layout.size[1] },
	};
	ret = s7d_codec_session_frame_ready(job->session, &frame);
	if (!ret)
		job->result.frame = frame;
stop:
	stopped = hevc_quiesce_locked(decoder, S7D_CODEC_ENGINE_HEVC);
	if (stopped) {
		decoder->quarantined = true;
		dev_err(decoder->dev, "HEVC job error %d, stop unproved %d; DMA retained\n",
			ret, stopped);
		ret = ret ? ret : stopped;
		decoder->running = false;
		complete_all(&decoder->idle);
		goto out;
	}
	if (job->context)
		s7d_hevc_context_release(job->context);
	s7d_codec_session_put(job->session);
	if (!ret && decoder->release_error)
		ret = decoder->release_error;
	if (!ret)
		*result = job->result;
	spin_lock_irqsave(&decoder->irq_lock, flags);
	decoder->job = NULL;
	spin_unlock_irqrestore(&decoder->irq_lock, flags);
free_job:
	kfree(job);
	decoder->running = false;
	complete_all(&decoder->idle);
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
	if (decoder->job || decoder->quarantined || decoder->release_error ||
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
