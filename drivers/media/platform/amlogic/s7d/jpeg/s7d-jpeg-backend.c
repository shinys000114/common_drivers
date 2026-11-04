// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>

#include "s7d-codec-dos.h"
#include "s7d-codec-firmware.h"
#include "s7d-codec-session.h"
#include "s7d-jpeg-backend.h"
#include "s7d-jpeg-bitstream.h"
#include "s7d-jpeg-protocol.h"

#define JPEG_TIMEOUT_MS 3000U

/* A source dma-buf HCODEC reads in place. */
struct s7d_jpeg_import {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t dma;
};

struct s7d_jpeg_execution {
	struct s7d_codec_session *session;
	struct s7d_jpeg_import import;
	struct s7d_jpeg_job hardware;
	struct s7d_jpeg_quantization quant;
	struct s7d_jpeg_header header;
	struct s7d_jpeg_irq_snapshot snapshot;
	struct completion done;
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
	/* The HCODEC lease, held through each session. */
	struct s7d_codec_lease lease;
	struct s7d_codec_firmware_owner *firmware;
	struct s7d_codec_core *core;
	struct s7d_codec_firmware_job *preload;
	struct s7d_jpeg_execution *job;
	struct regmap *map;
	/*
	 * Input copy and scan buffer, kept from the first encode of a session
	 * until a proved halt at its stop. Retained with the job after an
	 * unproved halt.
	 */
	void *input;
	void *scan;
	dma_addr_t input_dma;
	dma_addr_t scan_dma;
	u32 input_bytes;
	u32 scan_capacity;
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
	/*
	 * The JPEG image stays in HCODEC IMEM for the rest of the lease: the
	 * held lease keeps the domain powered, and the per-job halt resets the
	 * CPU and the local blocks only. Any failed job loads it again.
	 */
	bool firmware_loaded;
};

static int jpeg_check_stop(struct s7d_jpeg_encoder *encoder)
{
	return READ_ONCE(encoder->stop_requested) || READ_ONCE(encoder->closing) ?
		-ECANCELED : 0;
}

/* The input copy and scan buffer of a @width x @height frame. */
static int jpeg_frame_bytes(u32 width, u32 height, u32 *input_bytes, u32 *scan_capacity)
{
	u32 y_bytes, bytes, mcus;

	if (check_mul_overflow(ALIGN(width, 32), height, &y_bytes) ||
	    check_add_overflow(y_bytes, y_bytes / 2, &bytes))
		return -EOVERFLOW;
	*input_bytes = bytes;
	/* Six baseline blocks/MCU, each bounded by 1665 bits plus FF stuffing. */
	mcus = DIV_ROUND_UP(width, 16) * DIV_ROUND_UP(height, 16);
	if (check_mul_overflow(mcus, 6U * 420U, &bytes) ||
	    check_add_overflow(bytes, 2U + PAGE_SIZE - 1, &bytes))
		return -EOVERFLOW;
	*scan_capacity = bytes & PAGE_MASK;
	return 0;
}

static int jpeg_source_check(const struct s7d_jpeg_source *source,
			     u32 *input_bytes, u32 *scan_capacity)
{
	int ret;

	if (!source || (!source->data && !source->dmabuf) || !input_bytes ||
	    !scan_capacity)
		return -EINVAL;
	if (source->format != V4L2_PIX_FMT_NV12 &&
	    source->format != V4L2_PIX_FMT_NV21)
		return -EOPNOTSUPP;
	if (source->width < 16 || source->width > 1920 || source->height < 16 ||
	    source->height > 1080 || (source->width | source->height) & 1)
		return -ERANGE;
	if (source->stride != ALIGN(source->width, 32))
		return -EINVAL;
	ret = jpeg_frame_bytes(source->width, source->height, input_bytes, scan_capacity);
	if (ret)
		return ret;
	return source->bytes < *input_bytes ? -ENOSPC : 0;
}

size_t s7d_jpeg_encoder_pool_bytes(u32 width, u32 height)
{
	u32 input, scan;

	if (jpeg_frame_bytes(width, height, &input, &scan))
		return 0;
	return PAGE_ALIGN(input) + scan;
}

static void jpeg_unimport(struct s7d_jpeg_import *import)
{
	if (import->sgt)
		dma_buf_unmap_attachment_unlocked(import->attach, import->sgt,
						  DMA_TO_DEVICE);
	if (import->attach)
		dma_buf_detach(import->dmabuf, import->attach);
	if (import->dmabuf)
		dma_buf_put(import->dmabuf);
	memset(import, 0, sizeof(*import));
}

/*
 * Map a source dma-buf for HCODEC. It is read in place only as one DMA run of
 * the whole frame, on the 8-byte alignment and within the 32-bit range that
 * the canvas takes; anything else is copied.
 */
static int jpeg_import(struct device *dev, struct dma_buf *dmabuf, u32 bytes,
		       struct s7d_jpeg_import *import)
{
	struct scatterlist *sg;
	dma_addr_t next;
	u64 run = 0;
	unsigned int i;
	int ret;

	get_dma_buf(dmabuf);
	import->dmabuf = dmabuf;
	import->attach = dma_buf_attach(dmabuf, dev);
	if (IS_ERR(import->attach)) {
		ret = PTR_ERR(import->attach);
		import->attach = NULL;
		goto fail;
	}
	import->sgt = dma_buf_map_attachment_unlocked(import->attach, DMA_TO_DEVICE);
	if (IS_ERR_OR_NULL(import->sgt)) {
		ret = import->sgt ? PTR_ERR(import->sgt) : -ENOMEM;
		import->sgt = NULL;
		goto fail;
	}
	ret = -ERANGE;
	if (!import->sgt->nents)
		goto fail;
	import->dma = sg_dma_address(import->sgt->sgl);
	next = import->dma;
	for_each_sgtable_dma_sg(import->sgt, sg, i) {
		if (sg_dma_address(sg) != next || run >= bytes)
			break;
		run += sg_dma_len(sg);
		next += sg_dma_len(sg);
	}
	if (run < bytes || !IS_ALIGNED(import->dma, 8) ||
	    (u64)import->dma + bytes - 1 > U32_MAX)
		goto fail;
	return 0;
fail:
	jpeg_unimport(import);
	return ret;
}

static void jpeg_free_job(struct s7d_jpeg_execution *job)
{
	if (!job)
		return;
	jpeg_unimport(&job->import);
	s7d_codec_session_put(job->session);
	kfree(job);
}

/* HCODEC is proved idle, or never started on these buffers. */
static void jpeg_free_staging(struct s7d_jpeg_encoder *encoder)
{
	lockdep_assert_held(&encoder->control);
	if (encoder->scan)
		dma_free_coherent(encoder->dev, encoder->scan_capacity,
				  encoder->scan, encoder->scan_dma);
	if (encoder->input)
		dma_free_coherent(encoder->dev, encoder->input_bytes,
				  encoder->input, encoder->input_dma);
	encoder->scan = NULL;
	encoder->input = NULL;
	encoder->scan_capacity = 0;
	encoder->input_bytes = 0;
}

/*
 * One format holds for a whole session, so a buffer from its first encode
 * serves every later one. Another size replaces it; no job runs here.
 */
static int jpeg_alloc_staging(struct s7d_jpeg_encoder *encoder, void **cpu,
			      dma_addr_t *dma, u32 *bytes, u32 want)
{
	lockdep_assert_held(&encoder->control);
	if (*cpu && *bytes == want)
		return 0;
	if (*cpu)
		dma_free_coherent(encoder->dev, *bytes, *cpu, *dma);
	*bytes = 0;
	*cpu = dma_alloc_coherent(encoder->dev, want, dma, GFP_KERNEL);
	if (!*cpu)
		return -ENOMEM;
	*bytes = want;
	return 0;
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
	ret = s7d_codec_dos_jpeg_hold(encoder->dos, &encoder->lease);
	if (ret)
		return ret;
	encoder->lease_epoch++;
	encoder->firmware_epoch = 0;
	encoder->firmware_loaded = false;
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
	ret = s7d_codec_dos_halt(&encoder->lease);
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
	encoder->firmware_loaded = false;
	ret = jpeg_halt_locked(encoder);
	if (!ret) {
		job = jpeg_detach_job(encoder);
		jpeg_free_staging(encoder);
	}
	mutex_unlock(&encoder->control);
	jpeg_free_job(job);
	return ret;
}

static void jpeg_release(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_jpeg_encoder *encoder = priv;
	int ret;

	mutex_lock(&encoder->control);
	ret = engine != S7D_CODEC_ENGINE_HCODEC || !encoder->quiesced ?
		-EBUSY : s7d_codec_dos_release(&encoder->lease);
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

/*
 * Copy a frame HCODEC cannot read in place. A dma-buf is read through its
 * kernel mapping only inside CPU access, so that a cached exporter makes the
 * producer's writes visible to the CPU first.
 */
static int jpeg_copy_input(struct s7d_jpeg_encoder *encoder,
			   const struct s7d_jpeg_source *source, u32 bytes)
{
	int ret;

	if (source->dmabuf) {
		ret = dma_buf_begin_cpu_access(source->dmabuf, DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}
	memcpy(encoder->input, source->data, bytes);
	/* The copy is complete; a failed end only concerns the exporter. */
	if (source->dmabuf) {
		ret = dma_buf_end_cpu_access(source->dmabuf, DMA_FROM_DEVICE);
		if (ret)
			dev_warn_ratelimited(encoder->dev,
					     "JPEG source end_cpu_access failed: %d\n", ret);
	}
	return 0;
}

static int jpeg_stage(struct s7d_jpeg_encoder *encoder,
			struct s7d_jpeg_execution *job,
			const struct s7d_jpeg_source *source,
			u32 input_bytes, u32 scan_capacity)
{
	u32 y_bytes = source->stride * source->height;
	dma_addr_t input_dma = job->import.dma;
	int ret;

	ret = jpeg_alloc_staging(encoder, &encoder->scan, &encoder->scan_dma,
				 &encoder->scan_capacity, scan_capacity);
	if (!ret && !job->import.sgt) {
		ret = jpeg_alloc_staging(encoder, &encoder->input, &encoder->input_dma,
					 &encoder->input_bytes, input_bytes);
		input_dma = encoder->input_dma;
	}
	if (ret)
		return ret;
	job->hardware.scan_dma = encoder->scan_dma;
	job->hardware.scan_capacity = encoder->scan_capacity;
	job->hardware.input_format = source->format;
	job->hardware.width = source->width;
	job->hardware.height = source->height;
	job->hardware.y = (struct s7d_jpeg_plane) {
		.dma = input_dma, .bytes = y_bytes, .stride = source->stride,
	};
	job->hardware.uv = (struct s7d_jpeg_plane) {
		.dma = input_dma + y_bytes, .bytes = y_bytes / 2,
		.stride = source->stride,
	};
	ret = s7d_jpeg_protocol_check(&job->hardware);
	if (ret)
		return ret;
	if (!job->import.sgt) {
		ret = jpeg_copy_input(encoder, source, input_bytes);
		if (ret)
			return ret;
	}
	dma_wmb();
	return 0;
}

/*
 * The coherent scan buffer is uncached. Copy the produced bytes once and
 * check their framing in the cached capture buffer, not byte by byte here.
 */
static int jpeg_copy_scan(struct s7d_jpeg_encoder *encoder,
			  const struct s7d_jpeg_execution *job, void *capture,
			  size_t capture_capacity, struct s7d_jpeg_output *output)
{
	u8 *scan = (u8 *)capture + sizeof(job->header.data);
	u32 bytes = job->snapshot.scan_bytes;

	if (bytes > job->hardware.scan_capacity)
		return -EOVERFLOW;
	if (bytes > capture_capacity - sizeof(job->header.data))
		return -ENOSPC;
	dma_rmb();
	memcpy(scan, encoder->scan, bytes);
	return s7d_jpeg_validate_output(scan, bytes, bytes, capture_capacity, output);
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
	struct s7d_jpeg_import import = { };
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
	if (source->dmabuf) {
		ret = jpeg_import(encoder->dev, source->dmabuf, input_bytes, &import);
		if (ret && !source->data)
			return ret;
	}
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
	job->import = import;
	memset(&import, 0, sizeof(import));
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
	ret = s7d_codec_dos_jpeg_prepare(&encoder->lease);
	if (!ret)
		ret = s7d_codec_firmware_set_policy(encoder->dev, S7D_CODEC_ENGINE_HCODEC);
	if (!ret)
		ret = s7d_codec_dos_jpeg_port_enable(&encoder->lease);
	if (!ret && !encoder->firmware_loaded)
		ret = jpeg_preload(encoder, job);
	if (ret)
		goto stop;
	if (!encoder->firmware_loaded) {
		ret = s7d_codec_firmware_load(encoder->dev, S7D_CODEC_ENGINE_HCODEC,
					      S7D_CODEC_FIRMWARE_JPEG_ENC);
		if (ret) {
			s7d_codec_firmware_invalidate(encoder->firmware);
			goto stop;
		}
		encoder->firmware_loaded = true;
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
		ret = s7d_codec_dos_jpeg_start(&encoder->lease);
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
	if (!ret)
		ret = jpeg_copy_scan(encoder, job, capture, capture_capacity, &output);
	if (!ret) {
		memcpy(capture, job->header.data, sizeof(job->header.data));
		*result = (struct s7d_jpeg_encode_result) {
			.bytesused = output.bytesused, .scan_bytes = output.scan_bytes,
			.generation = job->generation,
		};
	}
	retired = jpeg_detach_job(encoder);
idle:
	if (ret)
		encoder->firmware_loaded = false;
	encoder->running = false;
	complete_all(&encoder->idle);
out:
	mutex_unlock(&encoder->control);
	jpeg_free_job(retired);
	jpeg_unimport(&import);
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
	if (!ret) {
		job = jpeg_detach_job(encoder);
		jpeg_free_staging(encoder);
	}
out:
	mutex_unlock(&encoder->control);
	jpeg_free_job(job);
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

struct s7d_codec_dos *s7d_jpeg_encoder_dos(struct s7d_jpeg_encoder *encoder)
{
	return encoder ? encoder->dos : NULL;
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
