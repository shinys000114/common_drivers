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
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>

#include "../common/s7d-codec-dos.h"
#include "../common/s7d-codec-firmware.h"
#include "../common/s7d-codec-session.h"
#include "s7d-jpeg-backend.h"
#include "s7d-jpeg-bitstream.h"
#include "s7d-jpeg-protocol.h"

#define JPEG_TIMEOUT_MS 3000U

struct s7d_jpeg_execution {
	struct s7d_codec_session *session;
	struct s7d_jpeg_job hardware;
	struct s7d_jpeg_quantization quant;
	struct s7d_jpeg_header header;
	struct s7d_jpeg_irq_snapshot snapshot;
	struct completion done;
	void *input;
	void *scan;
	dma_addr_t input_dma;
	size_t input_bytes;
	u64 generation;
	u64 lease_epoch;
	int status;
	bool finished;
	/* The HCODEC CPU was told to run; a stop must let it finish first. */
	bool started;
};

struct s7d_jpeg_encoder {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_dos *dos;
	struct s7d_codec_firmware_owner *firmware;
	struct s7d_codec_core *core;
	struct s7d_codec_firmware_job *preload;
	struct s7d_jpeg_execution *job;
	struct regmap *map;
	struct mutex control; /* Serializes execution and session teardown. */
	struct mutex protocol; /* Serializes register programming and threaded IRQ. */
	struct completion idle;
	spinlock_t irq_lock; /* Protects the published job and stop request. */
	u64 lease_epoch;
	u64 firmware_epoch;
	u64 generation;
	int irq;
	int release_error;
	bool irq_accept;
	bool irq_enabled;
	bool stop_requested;
	bool closing;
	bool lease_active;
	bool quiesced;
	bool running;
	bool retained;
	bool release_pin;
};

static int jpeg_check_stop(struct s7d_jpeg_encoder *encoder)
{
	return READ_ONCE(encoder->stop_requested) || READ_ONCE(encoder->closing) ?
		-ECANCELED : 0;
}

static int jpeg_source_check(const struct s7d_jpeg_source *source,
			     u32 *input_bytes, u32 *scan_capacity)
{
	u32 y_bytes, bytes, mcus;

	if (!source || !source->data || !input_bytes || !scan_capacity)
		return -EINVAL;
	if (source->format != V4L2_PIX_FMT_NV12 &&
	    source->format != V4L2_PIX_FMT_NV21)
		return -EOPNOTSUPP;
	if (source->width < 16 || source->width > 1920 || source->height < 16 ||
	    source->height > 1080 || (source->width | source->height) & 1)
		return -ERANGE;
	if (source->stride != ALIGN(source->width, 32))
		return -EINVAL;
	if (check_mul_overflow(source->stride, source->height, &y_bytes) ||
	    check_add_overflow(y_bytes, y_bytes / 2, &bytes))
		return -EOVERFLOW;
	if (source->bytes < bytes)
		return -ENOSPC;
	*input_bytes = bytes;
	/* Six baseline blocks/MCU, each bounded by 1665 bits plus FF stuffing. */
	mcus = DIV_ROUND_UP(source->width, 16) * DIV_ROUND_UP(source->height, 16);
	if (check_mul_overflow(mcus, 6U * 420U, &bytes) ||
	    check_add_overflow(bytes, 2U + PAGE_SIZE - 1, &bytes))
		return -EOVERFLOW;
	*scan_capacity = bytes & PAGE_MASK;
	return 0;
}

static void jpeg_free_job(struct s7d_jpeg_encoder *encoder,
			  struct s7d_jpeg_execution *job)
{
	if (!job)
		return;
	if (job->scan)
		dma_free_coherent(encoder->dev, job->hardware.scan_capacity,
				  job->scan, job->hardware.scan_dma);
	if (job->input)
		dma_free_coherent(encoder->dev, job->input_bytes, job->input,
				  job->input_dma);
	s7d_codec_session_put(job->session);
	kfree(job);
}

static irqreturn_t jpeg_irq_thread(int irq, void *data)
{
	struct s7d_jpeg_encoder *encoder = data;
	struct s7d_jpeg_irq_snapshot snapshot;
	struct s7d_jpeg_execution *job;
	unsigned long flags;
	int ret;

	mutex_lock(&encoder->protocol);
	spin_lock_irqsave(&encoder->irq_lock, flags);
	job = encoder->irq_accept ? encoder->job : NULL;
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
	if (!job)
		goto out;
	ret = s7d_jpeg_protocol_irq(encoder->map, &snapshot);
	if (job->finished)
		goto out;
	/* Only a finished encode releases the job, even when a stop is pending. */
	if (!ret && snapshot.status != S7D_JPEG_ENCODER_DONE)
		goto out;
	if (!ret)
		ret = jpeg_check_stop(encoder);
	if (!ret && job->generation != encoder->generation)
		ret = -ESTALE;
	if (!ret && (!snapshot.scan_bytes ||
		     snapshot.scan_bytes >= job->hardware.scan_capacity))
		ret = -EOVERFLOW;
	if (!ret)
		job->snapshot = snapshot;
	job->status = ret;
	job->finished = true;
	complete_all(&job->done);
out:
	mutex_unlock(&encoder->protocol);
	return IRQ_HANDLED;
}

static int jpeg_hold(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_jpeg_encoder *encoder = priv;
	unsigned long flags;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HCODEC)
		return -EOPNOTSUPP;
	if (READ_ONCE(encoder->closing) || encoder->retained || encoder->release_error)
		return -ESHUTDOWN;
	if (encoder->lease_epoch == U64_MAX)
		return -EOVERFLOW;
	ret = s7d_codec_dos_jpeg_hold(encoder->dos);
	if (ret)
		return ret;
	encoder->lease_epoch++;
	encoder->firmware_epoch = 0;
	encoder->lease_active = true;
	encoder->quiesced = false;
	spin_lock_irqsave(&encoder->irq_lock, flags);
	encoder->stop_requested = false;
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
	return 0;
}

static int jpeg_halt_locked(struct s7d_jpeg_encoder *encoder)
{
	unsigned long flags;
	bool disable;
	int ret;

	lockdep_assert_held(&encoder->control);
	if (!encoder->lease_active || encoder->quiesced)
		return 0;
	spin_lock_irqsave(&encoder->irq_lock, flags);
	encoder->irq_accept = false;
	disable = encoder->irq_enabled;
	encoder->irq_enabled = false;
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
	if (disable)
		disable_irq_nosync(encoder->irq);
	synchronize_irq(encoder->irq);
	mutex_lock(&encoder->protocol);
	ret = s7d_codec_dos_halt(encoder->dos, S7D_CODEC_ENGINE_HCODEC);
	encoder->quiesced = !ret;
	encoder->retained = !!ret;
	mutex_unlock(&encoder->protocol);
	return ret;
}

static struct s7d_jpeg_execution *jpeg_detach_job(struct s7d_jpeg_encoder *encoder)
{
	struct s7d_jpeg_execution *job;
	unsigned long flags;

	lockdep_assert_held(&encoder->control);
	spin_lock_irqsave(&encoder->irq_lock, flags);
	job = encoder->job;
	encoder->job = NULL;
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
	return job;
}

static int jpeg_quiesce(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_jpeg_encoder *encoder = priv;
	struct s7d_jpeg_execution *job = NULL;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HCODEC)
		return -EOPNOTSUPP;
	s7d_jpeg_encoder_request_stop(encoder);
	mutex_lock(&encoder->control);
	while (encoder->running) {
		mutex_unlock(&encoder->control);
		wait_for_completion(&encoder->idle);
		mutex_lock(&encoder->control);
	}
	ret = jpeg_halt_locked(encoder);
	if (!ret)
		job = jpeg_detach_job(encoder);
	mutex_unlock(&encoder->control);
	jpeg_free_job(encoder, job);
	return ret;
}

static void jpeg_release(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_jpeg_encoder *encoder = priv;
	int ret;

	mutex_lock(&encoder->control);
	ret = engine != S7D_CODEC_ENGINE_HCODEC || !encoder->quiesced ?
		-EBUSY : s7d_codec_dos_release(encoder->dos, engine);
	if (!ret) {
		encoder->lease_active = false;
		mutex_unlock(&encoder->control);
		return;
	}
	encoder->release_error = ret;
	if (!encoder->release_pin && try_module_get(encoder->owner))
		encoder->release_pin = true;
	dev_err(encoder->dev, "JPEG release failed: %d; owner retained\n", ret);
	mutex_unlock(&encoder->control);
}

static const struct s7d_codec_resource_ops jpeg_resource_ops = {
	.hold = jpeg_hold,
	.quiesce = jpeg_quiesce,
	.release = jpeg_release,
};

static int jpeg_preload(struct s7d_jpeg_encoder *encoder,
			struct s7d_jpeg_execution *job)
{
	struct s7d_codec_firmware_job *preload;
	unsigned long flags;
	bool queued;
	int ret;

	if (!encoder->firmware_epoch) {
		ret = s7d_codec_firmware_next_epoch(encoder->firmware,
						    &encoder->firmware_epoch);
		if (ret)
			return ret;
	}
	ret = s7d_codec_firmware_prepare_for_engine(encoder->firmware, job->session,
			encoder->firmware_epoch, S7D_CODEC_ENGINE_HCODEC,
			S7D_CODEC_FIRMWARE_JPEG_ENC, &preload);
	if (ret || !preload)
		return ret ? ret : jpeg_check_stop(encoder);
	spin_lock_irqsave(&encoder->irq_lock, flags);
	encoder->preload = preload;
	if (encoder->stop_requested || encoder->closing)
		s7d_codec_firmware_job_stop(preload);
	queued = s7d_codec_firmware_job_queue(preload);
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
	if (queued) {
		mutex_unlock(&encoder->control);
		ret = s7d_codec_firmware_job_join(preload);
		mutex_lock(&encoder->control);
	} else {
		ret = -EIO;
	}
	spin_lock_irqsave(&encoder->irq_lock, flags);
	encoder->preload = NULL;
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
	mutex_unlock(&encoder->control);
	s7d_codec_firmware_job_release_custody(preload);
	s7d_codec_firmware_job_put(preload);
	mutex_lock(&encoder->control);
	if (encoder->job != job || job->lease_epoch != encoder->lease_epoch)
		return -ESTALE;
	return ret ? ret : jpeg_check_stop(encoder);
}

static int jpeg_stage(struct s7d_jpeg_encoder *encoder,
			struct s7d_jpeg_execution *job,
			const struct s7d_jpeg_source *source,
			u32 input_bytes, u32 scan_capacity)
{
	u32 y_bytes = source->stride * source->height;
	int ret;

	job->input_bytes = input_bytes;
	job->input = dma_alloc_coherent(encoder->dev, input_bytes,
					 &job->input_dma, GFP_KERNEL);
	if (!job->input)
		return -ENOMEM;
	job->hardware.scan_capacity = scan_capacity;
	job->scan = dma_alloc_coherent(encoder->dev, scan_capacity,
					&job->hardware.scan_dma, GFP_KERNEL);
	if (!job->scan)
		return -ENOMEM;
	job->hardware.input_format = source->format;
	job->hardware.width = source->width;
	job->hardware.height = source->height;
	job->hardware.y = (struct s7d_jpeg_plane) {
		.dma = job->input_dma, .bytes = y_bytes, .stride = source->stride,
	};
	job->hardware.uv = (struct s7d_jpeg_plane) {
		.dma = job->input_dma + y_bytes, .bytes = y_bytes / 2,
		.stride = source->stride,
	};
	ret = s7d_jpeg_protocol_check(&job->hardware);
	if (ret)
		return ret;
	memcpy(job->input, source->data, input_bytes);
	dma_wmb();
	return 0;
}

void s7d_jpeg_encoder_request_stop(struct s7d_jpeg_encoder *encoder)
{
	unsigned long flags;

	if (!encoder)
		return;
	spin_lock_irqsave(&encoder->irq_lock, flags);
	encoder->stop_requested = true;
	if (encoder->preload)
		s7d_codec_firmware_job_stop(encoder->preload);
	/* Halting HCODEC while its DMA runs hangs the bus; wait for the IRQ. */
	if (encoder->job && !encoder->job->started)
		complete_all(&encoder->job->done);
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
}

int s7d_jpeg_encoder_encode(struct s7d_jpeg_encoder *encoder,
			    struct s7d_codec_session *session,
			    const struct s7d_jpeg_source *source,
			    void *capture, size_t capture_capacity,
			    unsigned int quality,
			    struct s7d_jpeg_encode_result *result)
{
	struct s7d_jpeg_execution *job, *retired = NULL;
	struct s7d_jpeg_output output;
	unsigned long flags;
	u32 input_bytes, scan_capacity;
	int ret, halted;

	if (!encoder || !session || !capture || !result)
		return -EINVAL;
	if (capture_capacity <= S7D_JPEG_HEADER_BYTES || capture_capacity > U32_MAX)
		return -ENOSPC;
	ret = jpeg_source_check(source, &input_bytes, &scan_capacity);
	if (ret)
		return ret;
	mutex_lock(&encoder->control);
	if (encoder->job || encoder->retained || encoder->release_error) {
		ret = -EBUSY;
		goto out;
	}
	if (!encoder->lease_active) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = jpeg_check_stop(encoder);
	if (ret)
		goto out;
	if (encoder->generation == U64_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out;
	}
	job->session = session;
	s7d_codec_session_get(session);
	init_completion(&job->done);
	job->status = -EINPROGRESS;
	job->lease_epoch = encoder->lease_epoch;
	job->generation = ++encoder->generation;
	reinit_completion(&encoder->idle);
	encoder->running = true;
	spin_lock_irqsave(&encoder->irq_lock, flags);
	encoder->job = job;
	spin_unlock_irqrestore(&encoder->irq_lock, flags);
	ret = s7d_jpeg_build_quantization(quality, &job->quant);
	if (!ret)
		ret = s7d_jpeg_build_header(source->format, source->width,
					   source->height, &job->quant, &job->header);
	if (!ret)
		ret = jpeg_stage(encoder, job, source, input_bytes, scan_capacity);
	if (ret)
		goto stop;
	encoder->quiesced = false;
	ret = s7d_codec_dos_jpeg_prepare(encoder->dos);
	if (!ret)
		ret = s7d_codec_firmware_set_policy(encoder->dev, S7D_CODEC_ENGINE_HCODEC);
	if (!ret)
		ret = s7d_codec_dos_jpeg_port_enable(encoder->dos);
	if (!ret)
		ret = jpeg_preload(encoder, job);
	if (ret)
		goto stop;
	ret = s7d_codec_firmware_load(encoder->dev, S7D_CODEC_ENGINE_HCODEC,
				      S7D_CODEC_FIRMWARE_JPEG_ENC);
	if (ret) {
		s7d_codec_firmware_invalidate(encoder->firmware);
		goto stop;
	}
	mutex_lock(&encoder->protocol);
	ret = jpeg_check_stop(encoder);
	if (!ret)
		ret = s7d_jpeg_protocol_program(encoder->map, &job->hardware, &job->quant);
	if (!ret)
		ret = jpeg_check_stop(encoder);
	if (!ret) {
		spin_lock_irqsave(&encoder->irq_lock, flags);
		encoder->irq_accept = true;
		encoder->irq_enabled = true;
		job->started = true;
		spin_unlock_irqrestore(&encoder->irq_lock, flags);
		enable_irq(encoder->irq);
		dma_wmb();
		ret = s7d_codec_dos_jpeg_start(encoder->dos);
	}
	mutex_unlock(&encoder->protocol);
	if (ret)
		goto stop;
	if (!wait_for_completion_timeout(&job->done, msecs_to_jiffies(JPEG_TIMEOUT_MS)))
		ret = -ETIMEDOUT;
	mutex_lock(&encoder->protocol);
	if (!ret)
		ret = jpeg_check_stop(encoder);
	if (!ret)
		ret = job->finished ? job->status : -ECANCELED;
	mutex_unlock(&encoder->protocol);
stop:
	halted = jpeg_halt_locked(encoder);
	if (halted) {
		dev_err(encoder->dev, "JPEG job error %d, stop unproved %d; DMA retained\n",
			ret, halted);
		ret = ret ? ret : halted;
		goto idle;
	}
	if (!ret)
		ret = jpeg_check_stop(encoder);
	if (!ret) {
		dma_rmb();
		ret = s7d_jpeg_validate_output(job->scan, job->hardware.scan_capacity,
				job->snapshot.scan_bytes, capture_capacity, &output);
	}
	if (!ret) {
		memcpy(capture, job->header.data, sizeof(job->header.data));
		memcpy((u8 *)capture + sizeof(job->header.data), job->scan,
		       output.scan_bytes);
		*result = (struct s7d_jpeg_encode_result) {
			.bytesused = output.bytesused, .scan_bytes = output.scan_bytes,
			.generation = job->generation,
		};
	}
	retired = jpeg_detach_job(encoder);
idle:
	encoder->running = false;
	complete_all(&encoder->idle);
out:
	mutex_unlock(&encoder->control);
	jpeg_free_job(encoder, retired);
	return ret;
}

int s7d_jpeg_encoder_stop(struct s7d_jpeg_encoder *encoder,
			  struct s7d_codec_session *session)
{
	struct s7d_jpeg_execution *job = NULL;
	int ret;

	if (!encoder || !session)
		return -EINVAL;
	s7d_jpeg_encoder_request_stop(encoder);
	mutex_lock(&encoder->control);
	while (encoder->running) {
		mutex_unlock(&encoder->control);
		wait_for_completion(&encoder->idle);
		mutex_lock(&encoder->control);
	}
	if (encoder->job && encoder->job->session != session) {
		ret = -EINVAL;
		goto out;
	}
	ret = jpeg_halt_locked(encoder);
	if (!ret)
		job = jpeg_detach_job(encoder);
out:
	mutex_unlock(&encoder->control);
	jpeg_free_job(encoder, job);
	return ret;
}

int s7d_jpeg_encoder_shutdown(struct s7d_jpeg_encoder *encoder)
{
	if (!encoder)
		return -EINVAL;
	WRITE_ONCE(encoder->closing, true);
	return jpeg_quiesce(encoder, S7D_CODEC_ENGINE_HCODEC);
}

struct s7d_jpeg_encoder *
s7d_jpeg_encoder_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			struct module *owner)
{
	struct s7d_jpeg_encoder *encoder;
	int ret;

	if (!dev || IS_ERR_OR_NULL(borrowed_dos) || IS_ERR_OR_NULL(borrowed_firmware))
		return ERR_PTR(-EINVAL);
#ifdef MODULE
	if (owner != THIS_MODULE)
		return ERR_PTR(-EINVAL);
#endif
	encoder = kzalloc(sizeof(*encoder), GFP_KERNEL);
	if (!encoder)
		return ERR_PTR(-ENOMEM);
	encoder->dev = get_device(dev);
	encoder->owner = owner;
	encoder->dos = borrowed_dos;
	encoder->firmware = borrowed_firmware;
	mutex_init(&encoder->control);
	mutex_init(&encoder->protocol);
	init_completion(&encoder->idle);
	complete_all(&encoder->idle);
	spin_lock_init(&encoder->irq_lock);
	encoder->map = s7d_codec_dos_regmap(borrowed_dos, S7D_CODEC_ENGINE_HCODEC);
	if (IS_ERR_OR_NULL(encoder->map)) {
		ret = encoder->map ? PTR_ERR(encoder->map) : -ENODEV;
		goto free;
	}
	encoder->irq = s7d_codec_dos_irq(borrowed_dos, S7D_CODEC_ENGINE_HCODEC);
	if (encoder->irq < 0) {
		ret = encoder->irq;
		goto free;
	}
	ret = request_threaded_irq(encoder->irq, NULL, jpeg_irq_thread,
				   IRQF_NO_AUTOEN | IRQF_ONESHOT, dev_name(dev), encoder);
	if (ret)
		goto free;
	encoder->core = s7d_codec_core_create(dev, owner, &jpeg_resource_ops, encoder, 32);
	if (IS_ERR(encoder->core)) {
		ret = PTR_ERR(encoder->core);
		goto free_irq;
	}
	return encoder;
free_irq:
	free_irq(encoder->irq, encoder);
free:
	put_device(encoder->dev);
	kfree(encoder);
	return ERR_PTR(ret);
}

struct s7d_codec_core *s7d_jpeg_encoder_core(struct s7d_jpeg_encoder *encoder)
{
	return encoder ? encoder->core : NULL;
}

int s7d_jpeg_encoder_destroy(struct s7d_jpeg_encoder *encoder)
{
	int ret;

	if (!encoder)
		return -EINVAL;
	ret = s7d_jpeg_encoder_shutdown(encoder);
	if (ret)
		return ret;
	mutex_lock(&encoder->control);
	if (encoder->job || encoder->retained || encoder->release_error ||
	    encoder->release_pin) {
		ret = -EBUSY;
		mutex_unlock(&encoder->control);
		return ret;
	}
	mutex_unlock(&encoder->control);
	ret = s7d_codec_core_destroy(encoder->core);
	if (ret)
		return ret;
	free_irq(encoder->irq, encoder);
	put_device(encoder->dev);
	kfree(encoder);
	return 0;
}
