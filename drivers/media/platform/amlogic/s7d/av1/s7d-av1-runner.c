// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "../common/s7d-codec-dos.h"
#include "../common/s7d-codec-firmware.h"
#include "../common/s7d-codec-session.h"
#include "s7d-av1-picture.h"
#include "s7d-av1-runner.h"

#define AV1_JOB_TIMEOUT_MS 2000U

struct av1_header_job {
	struct s7d_codec_session *session;
	struct s7d_av1_header_result result;
	struct s7d_av1_transport transport;
	struct s7d_av1_input_plan input;
	struct s7d_av1_execution execution;
	struct s7d_av1_irq_snapshot snapshot;
	struct s7d_av1_reference_picture picture;
	struct completion wake;
	u8 *source;
	u8 sequence_payload[S7D_AV1_SEQUENCE_MAX_BYTES];
	u64 epoch;
	u64 token;
	u64 firmware_epoch;
	u64 timestamp_ns;
	u32 sequence_bytes;
	int status;
	bool decode;
	bool reference_run;
	bool picture_ready;
	bool picture_done;
	bool show_existing;
	bool staged;
	bool finished;
};

struct s7d_av1_runner {
	struct device *dev;
	struct s7d_codec_dos *dos;
	struct s7d_codec_firmware_owner *firmware;
	struct s7d_codec_firmware_job *preload;
	struct s7d_hevc_engine *engine;
	struct regmap *map;
	struct mutex control;
	struct mutex protocol;
	spinlock_t irq_lock;
	struct av1_header_job *job;
	struct s7d_codec_session *session;
	struct s7d_av1_dma_context *context;
	struct s7d_av1_references *references;
	struct s7d_av1_sequence sequence;
	u8 sequence_payload[S7D_AV1_SEQUENCE_MAX_BYTES];
	u32 sequence_bytes;
	u32 decode_count;
	u64 epoch;
	u64 binding;
	u64 firmware_epoch;
	u64 last_token;
	u64 closed_epoch;
	u32 staging_capacity;
	int failure;
	bool context_ready;
	bool irq_accept;
	bool stopping;
	bool quiesced;
	bool quarantined;
	bool sequence_valid;
};

static int runner_restore_references(struct s7d_av1_runner *runner)
{
	u32 words[S7D_AV1_PARSER_REFERENCE_WORDS];
	u32 i;
	int ret;

	if (!runner->references)
		return 0;
	ret = s7d_av1_references_parser_words(runner->references, runner->epoch, words);
	if (!ret)
		ret = regmap_write(runner->map, 0x312d * 4, 0x11a0);
	for (i = 0; !ret && i < ARRAY_SIZE(words); i++)
		ret = regmap_write(runner->map, 0x312e * 4, words[i]);
	return ret;
}

static int runner_configure_picture(struct s7d_av1_runner *runner,
				    struct av1_header_job *job)
{
	const struct s7d_av1_header *header = &job->result.header;
	const struct s7d_av1_sequence *sequence = &job->result.sequence;
	struct s7d_av1_dpb_metadata metadata = {
		.input_token = job->token,
		.timestamp_ns = job->timestamp_ns,
	};
	struct s7d_av1_picture_input input;
	struct s7d_av1_reference_layout layout;
	enum s7d_av1_execution_event event;
	int ret;

	if (!header->frame.show_existing &&
	    (header->frame.width != sequence->max_frame_width ||
	     header->frame.height != sequence->max_frame_height))
		return -EOPNOTSUPP;
	ret = s7d_av1_references_prepare(runner->references, job->epoch, job->token,
					header, &metadata, &job->picture);
	if (!ret)
		ret = s7d_av1_execution_accept_frame(&job->execution, header, &event);
	if (ret)
		return ret;
	job->show_existing = event == S7D_AV1_EVENT_EXISTING;
	job->picture_ready = true;
	if (job->show_existing) {
		job->picture_done = true;
		return 0;
	}
	if (job->picture.count > S7D_AV1_FRAME_SLOTS ||
	    job->picture.plan.current_picture.id.slot >= job->picture.count)
		return -EPROTO;
	input = (struct s7d_av1_picture_input) {
		.workspace = s7d_av1_dma_context_workspace(runner->context),
		.sequence = sequence,
		.header = header,
		.dpb = &job->picture.plan,
		.backing = job->picture.backing,
		.backing_count = job->picture.count,
		.state = job->picture.state,
		.primary = job->picture.primary,
	};
	ret = s7d_av1_reference_layout(header->frame.width, header->frame.height,
		sequence->bit_depth, sequence->use_128x128_superblock ? 128 : 64, &layout);
	if (!ret)
		ret = s7d_av1_dma_context_map_body(runner->context,
			&job->picture.backing[job->picture.plan.current_picture.id.slot].body,
			layout.body_pages, job->epoch, job->token);
	if (!ret)
		ret = s7d_av1_dma_context_mmu_region(runner->context, job->epoch,
						   &input.mmu_map);
	if (!ret)
		ret = s7d_av1_picture_configure(runner->map, &input, job->picture.state);
	if (!ret)
		ret = s7d_av1_references_configured(runner->references, job->epoch, job->token);
	if (!ret) {
		dma_wmb();
		ret = s7d_av1_execution_resume(&job->execution, runner->map, true);
	}
	return ret;
}

static bool runner_cookie(const struct s7d_av1_runner *runner,
			  const struct av1_header_job *job,
			  const struct s7d_hevc_irq_cookie *cookie)
{
	return job && cookie->session == job->session &&
		cookie->binding == runner->binding && cookie->lease_epoch == job->epoch &&
		cookie->input_token == job->token;
}

static irqreturn_t av1_irq(void *priv, const struct s7d_hevc_irq_cookie *cookie)
{
	struct s7d_av1_runner *runner = priv;
	struct av1_header_job *job;
	struct s7d_av1_irq_snapshot snapshot;
	unsigned long flags;

	spin_lock_irqsave(&runner->irq_lock, flags);
	job = runner->irq_accept ? runner->job : NULL;
	if (!runner_cookie(runner, job, cookie))
		job = NULL;
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	if (!job)
		return IRQ_HANDLED;
	s7d_av1_protocol_irq_snapshot(runner->map, &snapshot);
	if (!snapshot.error && (!snapshot.status || snapshot.status == 0xff ||
				 snapshot.status == 5 || snapshot.status == 6 ||
				 snapshot.status == S7D_AV1_FGS_CONTINUE))
		return IRQ_HANDLED;
	spin_lock_irqsave(&runner->irq_lock, flags);
	job->snapshot = snapshot;
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t av1_thread(void *priv, const struct s7d_hevc_irq_cookie *cookie)
{
	struct s7d_av1_runner *runner = priv;
	struct av1_header_job *job;
	struct s7d_av1_irq_snapshot snapshot;
	struct s7d_av1_rpm rpm;
	enum s7d_av1_execution_event event;
	unsigned long flags;
	int ret;

	mutex_lock(&runner->protocol);
	spin_lock_irqsave(&runner->irq_lock, flags);
	job = runner->irq_accept ? runner->job : NULL;
	if (!runner_cookie(runner, job, cookie))
		job = NULL;
	if (job)
		snapshot = job->snapshot;
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	if (!job || job->finished)
		goto out;
	ret = READ_ONCE(runner->stopping) ? -ECANCELED :
		s7d_av1_execution_receive(&job->execution, cookie, &snapshot, &event);
	if (ret)
		goto finished;
	job->result.shift_bytes = snapshot.shift_bytes;
	if (event == S7D_AV1_EVENT_NONE)
		goto out;
	if (event == S7D_AV1_EVENT_FGS_READ) {
		if (job->result.sequence.film_grain_params_present ||
		    (job->execution.phase == S7D_AV1_EXECUTION_DECODE &&
		     (!job->decode || !job->picture_ready || job->show_existing)))
			ret = -EOPNOTSUPP;
		else
			ret = s7d_av1_protocol_fgs_read_ack(runner->map);
		if (!ret)
			goto out;
		goto finished;
	}
	if (event == S7D_AV1_EVENT_PICTURE) {
		if (!job->decode || !job->picture_ready || job->show_existing) {
			ret = -EPROTO;
			goto finished;
		}
		job->picture_done = true;
		goto picture_end;
	}
	if (event == S7D_AV1_EVENT_INPUT || event == S7D_AV1_EVENT_EOS) {
		ret = job->decode && job->picture_done ? 0 : -EPROTO;
		goto finished;
	}
	if (event != S7D_AV1_EVENT_SEQUENCE && event != S7D_AV1_EVENT_FRAME) {
		ret = -EPROTO;
		goto finished;
	}
	ret = s7d_av1_dma_context_snapshot_rpm(runner->context, job->epoch,
					     job->token, true, &rpm);
	if (ret)
		goto finished;
	if (event == S7D_AV1_EVENT_SEQUENCE) {
		ret = s7d_av1_rpm_check_sequence(&rpm, &job->result.sequence);
		if (!ret)
			ret = runner_restore_references(runner);
		if (!ret)
			ret = s7d_av1_execution_resume(&job->execution, runner->map, false);
		if (!ret) {
			job->result.sequence_seen = true;
			goto out;
		}
	} else {
		ret = s7d_av1_rpm_read_frame(&rpm, &job->result.sequence, &job->result.header);
		if (!ret) {
			job->result.status = snapshot.status;
			job->result.shift_bytes = snapshot.shift_bytes;
			job->result.header_ready = true;
		}
		if (!ret && job->decode) {
			ret = runner_configure_picture(runner, job);
			if (ret)
				goto finished;
			if (!job->show_existing)
				goto out;
			goto picture_end;
		}
	}
	goto finished;
picture_end:
	job->result.shift_bytes = snapshot.shift_bytes;
	if (snapshot.shift_bytes < job->input.decode_size) {
		ret = s7d_av1_execution_drain(&job->execution, runner->map);
		if (!ret)
			goto out;
	}
finished:
	job->status = ret;
	job->finished = true;
	complete_all(&job->wake);
out:
	mutex_unlock(&runner->protocol);
	return IRQ_HANDLED;
}

static const struct s7d_hevc_irq_ops av1_irq_ops = {
	.irq = av1_irq,
	.thread = av1_thread,
};

static int runner_halt(struct s7d_av1_runner *runner)
{
	unsigned long flags;
	int ret, joined = 0, halted = 0;

	if (runner->quarantined)
		return -EIO;
	spin_lock_irqsave(&runner->irq_lock, flags);
	runner->irq_accept = false;
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	ret = s7d_codec_dos_prepare_access(runner->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_codec_dos_hevc_irq_mask(runner->dos, false);
	if (runner->binding)
		joined = s7d_hevc_engine_disarm(runner->engine, runner->binding);
	mutex_lock(&runner->protocol);
	if (!ret && !runner->quiesced)
		halted = s7d_codec_dos_halt(runner->dos, S7D_CODEC_ENGINE_HEVC);
	ret = ret ?: joined ?: halted;
	runner->quiesced = !ret;
	mutex_unlock(&runner->protocol);
	if (ret) {
		runner->quarantined = true;
		if (runner->binding)
			s7d_hevc_engine_quarantine(runner->engine, runner->binding);
	}
	return ret;
}

void s7d_av1_runner_request_stop(struct s7d_av1_runner *runner)
{
	unsigned long flags;

	if (!runner)
		return;
	spin_lock_irqsave(&runner->irq_lock, flags);
	runner->stopping = true;
	if (runner->preload)
		s7d_codec_firmware_job_stop(runner->preload);
	if (runner->job)
		complete_all(&runner->job->wake);
	spin_unlock_irqrestore(&runner->irq_lock, flags);
}

static int runner_preload(struct s7d_av1_runner *runner, struct av1_header_job *job)
{
	struct s7d_codec_firmware_job *preload;
	unsigned long flags;
	bool queued, cancelled;
	int ret;

	if (!runner->firmware_epoch) {
		ret = s7d_codec_firmware_next_epoch(runner->firmware, &runner->firmware_epoch);
		if (ret)
			return ret;
	}
	job->firmware_epoch = runner->firmware_epoch;
	ret = s7d_codec_firmware_prepare_for_engine(runner->firmware, job->session,
		job->firmware_epoch, S7D_CODEC_ENGINE_HEVC,
		S7D_CODEC_FIRMWARE_AV1_MMU, &preload);
	if (ret || !preload)
		return ret;
	spin_lock_irqsave(&runner->irq_lock, flags);
	runner->preload = preload;
	if (runner->stopping)
		s7d_codec_firmware_job_stop(preload);
	queued = s7d_codec_firmware_job_queue(preload);
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	if (!queued) {
		runner->quarantined = true;
		s7d_hevc_engine_quarantine(runner->engine, runner->binding);
		return -EIO;
	}
	ret = s7d_codec_firmware_job_join_result(preload, job->session,
						job->firmware_epoch, &cancelled);
	spin_lock_irqsave(&runner->irq_lock, flags);
	runner->preload = NULL;
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	s7d_codec_firmware_job_release_custody(preload);
	s7d_codec_firmware_job_put(preload);
	if (ret)
		return ret;
	return READ_ONCE(runner->stopping) ? -ECANCELED : 0;
}

static int runner_sequence(struct s7d_av1_runner *runner, const u8 *data,
			   size_t bytes, struct av1_header_job *job)
{
	u32 offset = 0, frame_type = 0;
	int ret;

	ret = s7d_av1_transport_size(data, bytes, &job->transport);
	if (ret)
		return ret;
	if (!job->transport.frame_header_count || job->transport.sequence_count > 1 ||
	    (!job->decode && job->transport.sequence_count != 1) ||
	    (job->decode && job->transport.frame_header_count != 1))
		return -EOPNOTSUPP;
	if (job->decode && !runner->references && !job->transport.sequence_count)
		return -EPIPE;
	while (offset < bytes) {
		struct s7d_av1_obu obu;

		ret = s7d_av1_obu_next(data, bytes, offset, &obu);
		if (ret)
			return ret;
		if (obu.type == S7D_AV1_OBU_SEQUENCE) {
			if (frame_type)
				return -EINVAL;
			ret = s7d_av1_parse_sequence(data + obu.payload_offset,
				obu.payload_bytes, &job->result.sequence);
			if (!ret)
				ret = s7d_av1_admit_sequence(&job->result.sequence);
			if (ret)
				return ret;
			job->sequence_bytes = obu.payload_bytes;
			memcpy(job->sequence_payload, data + obu.payload_offset, obu.payload_bytes);
		} else if (obu.type == S7D_AV1_OBU_FRAME ||
			   obu.type == S7D_AV1_OBU_FRAME_HEADER) {
			if (!frame_type)
				frame_type = obu.type;
		} else if (job->decode &&
			   (obu.type == S7D_AV1_OBU_REDUNDANT_HEADER ||
			    (obu.type == S7D_AV1_OBU_TILE_GROUP &&
			     frame_type != S7D_AV1_OBU_FRAME_HEADER))) {
			return -EOPNOTSUPP;
		}
		offset += obu.bytes;
	}
	if (!job->transport.sequence_count) {
		if (!runner->sequence_valid)
			return -EPIPE;
		job->result.sequence = runner->sequence;
	} else if (runner->sequence_valid &&
		   (runner->sequence_bytes != job->sequence_bytes ||
		    memcmp(runner->sequence_payload, job->sequence_payload,
			   job->sequence_bytes))) {
		return -EPIPE;
	}
	return 0;
}

struct s7d_av1_runner *
s7d_av1_runner_create(struct device *dev, struct s7d_codec_dos *dos,
		      struct s7d_codec_firmware_owner *firmware,
		      struct s7d_hevc_engine *engine, u32 staging_capacity)
{
	struct s7d_av1_runner *runner;

	if (!dev || IS_ERR_OR_NULL(dos) || IS_ERR_OR_NULL(firmware) ||
	    IS_ERR_OR_NULL(engine) || !staging_capacity ||
	    staging_capacity > S7D_AV1_INPUT_MAX_BYTES * 11U)
		return ERR_PTR(-EINVAL);
	runner = kzalloc(sizeof(*runner), GFP_KERNEL);
	if (!runner)
		return ERR_PTR(-ENOMEM);
	runner->map = s7d_codec_dos_regmap(dos, S7D_CODEC_ENGINE_HEVC);
	if (IS_ERR_OR_NULL(runner->map)) {
		int ret = runner->map ? PTR_ERR(runner->map) : -ENODEV;

		kfree(runner);
		return ERR_PTR(ret);
	}
	runner->dev = get_device(dev);
	runner->dos = dos;
	runner->firmware = firmware;
	runner->engine = engine;
	runner->staging_capacity = staging_capacity;
	mutex_init(&runner->control);
	mutex_init(&runner->protocol);
	spin_lock_init(&runner->irq_lock);
	return runner;
}

static int runner_run(struct s7d_av1_runner *runner,
		      struct s7d_codec_session *session, u64 lease_epoch,
		      u64 input_token, const u8 *data, size_t bytes, u64 timestamp_ns,
		      struct s7d_av1_header_result *header_result,
		      struct s7d_av1_decode_result *decode_result)
{
	struct av1_header_job *job;
	struct s7d_av1_region mmu;
	struct s7d_hevc_irq_cookie cookie;
	struct s7d_av1_decode_result decoded = { 0 };
	unsigned long flags;
	int ret, halted;

	if (!runner || !session || !lease_epoch || !input_token || !data || !bytes ||
	    (!header_result && !decode_result) || (header_result && decode_result))
		return -EINVAL;
	if (bytes > S7D_AV1_INPUT_MAX_BYTES)
		return -E2BIG;
	mutex_lock(&runner->control);
	if (lease_epoch <= runner->closed_epoch) {
		mutex_unlock(&runner->control);
		return -ESTALE;
	}
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		mutex_unlock(&runner->control);
		return -ENOMEM;
	}
	job->decode = !!decode_result;
	job->timestamp_ns = timestamp_ns;
	job->source = kmemdup(data, bytes, GFP_KERNEL);
	if (!job->source) {
		ret = -ENOMEM;
		goto unlock;
	}
	ret = runner_sequence(runner, job->source, bytes, job);
	if (ret)
		goto unlock;
	if (job->transport.staged_bytes > runner->staging_capacity) {
		ret = -ENOSPC;
		goto unlock;
	}
	if (runner->job || runner->quarantined || runner->failure || runner->stopping) {
		ret = runner->failure ?: (runner->stopping ? -ECANCELED : -EBUSY);
		goto unlock;
	}
	if (!job->decode && runner->references) {
		ret = -EBUSY;
		goto unlock;
	}
	if (job->decode && runner->decode_count == U32_MAX) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	if (job->decode && runner->references) {
		ret = s7d_av1_references_output(runner->references, &decoded.output);
		if (ret != -EAGAIN) {
			ret = ret ?: -EBUSY;
			goto unlock;
		}
	}
	if (runner->session && (runner->session != session || runner->epoch != lease_epoch)) {
		ret = -ESTALE;
		goto unlock;
	}
	if (input_token <= runner->last_token) {
		ret = -ESTALE;
		goto unlock;
	}
	job->session = session;
	job->epoch = lease_epoch;
	job->token = input_token;
	job->status = -EINPROGRESS;
	init_completion(&job->wake);
	if (!runner->session) {
		s7d_codec_session_get(session);
		runner->session = session;
		runner->epoch = lease_epoch;
	}
	runner->last_token = input_token;
	ret = s7d_hevc_engine_bind(runner->engine, session, lease_epoch,
				   &av1_irq_ops, runner, &runner->binding);
	if (ret)
		goto unlock;
	spin_lock_irqsave(&runner->irq_lock, flags);
	runner->job = job;
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	ret = runner_halt(runner);
	if (ret)
		goto retained;
	if (!runner->context) {
		runner->context = s7d_av1_dma_context_create(session,
			job->result.sequence.max_frame_width,
			job->result.sequence.max_frame_height,
			runner->staging_capacity, lease_epoch);
		if (IS_ERR(runner->context)) {
			ret = PTR_ERR(runner->context);
			runner->context = NULL;
			goto stop;
		}
	}
	if (!runner->context_ready) {
		ret = s7d_av1_dma_context_mark_idle(runner->context, lease_epoch, 0, 0, true, false);
		if (ret)
			goto stop;
		runner->context_ready = true;
	}
	if (job->decode && !runner->references) {
		runner->references = s7d_av1_references_create(session,
			s7d_av1_dma_context_workspace(runner->context),
			&job->result.sequence, lease_epoch, 0, true);
		if (IS_ERR(runner->references)) {
			ret = PTR_ERR(runner->references);
			runner->references = NULL;
			goto stop;
		}
	}
	ret = s7d_av1_dma_context_stage(runner->context, job->source, bytes, lease_epoch,
				       input_token, &job->input);
	job->staged = !ret;
	kfree(job->source);
	job->source = NULL;
	if (!ret)
		ret = runner_preload(runner, job);
	if (!ret && job->decode) {
		ret = s7d_av1_references_run_begin(runner->references, lease_epoch, input_token);
		job->reference_run = !ret;
	}
	if (!ret)
		ret = s7d_av1_dma_context_begin(runner->context, lease_epoch, input_token);
	if (ret)
		goto stop;
	runner->quiesced = false;
	ret = s7d_codec_dos_prepare(runner->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_codec_firmware_set_policy(runner->dev, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_codec_dos_port_enable(runner->dos, S7D_CODEC_ENGINE_HEVC);
	if (!ret)
		ret = s7d_codec_firmware_load(runner->dev, S7D_CODEC_ENGINE_HEVC,
					     S7D_CODEC_FIRMWARE_AV1_MMU);
	job->result.firmware_loaded = !ret;
	if (!ret)
		ret = s7d_codec_dos_hevc_init(runner->dos);
	if (!ret)
		ret = s7d_av1_dma_context_program_input(runner->context, runner->map,
						       lease_epoch, input_token);
	if (!ret)
		ret = s7d_av1_dma_context_mmu_region(runner->context, lease_epoch, &mmu);
	if (!ret)
		ret = s7d_av1_protocol_init(runner->map,
			s7d_av1_dma_context_workspace(runner->context), &mmu,
			&job->result.sequence);
	cookie = (struct s7d_hevc_irq_cookie) {
		.session = session, .binding = runner->binding,
		.lease_epoch = lease_epoch, .input_token = input_token,
	};
	if (!ret)
		ret = s7d_av1_execution_begin(&job->execution, &cookie, &job->input,
					     job->transport.frame_header_count);
	if (!ret && !job->transport.sequence_count)
		job->execution.sequence_seen = runner->sequence_valid;
	if (!ret)
		ret = runner_restore_references(runner);
	if (!ret)
		ret = s7d_av1_protocol_start(runner->map,
			s7d_av1_dma_context_workspace(runner->context), &job->input,
			job->decode ? runner->decode_count : 0);
	if (!ret)
		ret = regmap_write(runner->map, 0x3071 * 4, 1);
	if (!ret) {
		spin_lock_irqsave(&runner->irq_lock, flags);
		runner->irq_accept = true;
		spin_unlock_irqrestore(&runner->irq_lock, flags);
		ret = s7d_hevc_engine_arm(runner->engine, runner->binding, input_token);
	}
	if (!ret)
		ret = s7d_codec_dos_hevc_irq_mask(runner->dos, true);
	if (!ret)
		ret = s7d_codec_dos_hevc_input_enable(runner->dos);
	if (!ret && READ_ONCE(runner->stopping))
		ret = -ECANCELED;
	if (!ret) {
		dma_wmb();
		ret = s7d_codec_dos_start(runner->dos, S7D_CODEC_ENGINE_HEVC);
	}
	if (!ret && !wait_for_completion_timeout(&job->wake,
			msecs_to_jiffies(AV1_JOB_TIMEOUT_MS)))
		ret = -ETIMEDOUT;
	mutex_lock(&runner->protocol);
	if (!ret)
		ret = READ_ONCE(runner->stopping) ? -ECANCELED :
			job->finished ? job->status : -EPROTO;
	mutex_unlock(&runner->protocol);
stop:
	halted = runner_halt(runner);
	if (job->reference_run && !halted) {
		int finish;

		if (!ret && job->execution.phase == S7D_AV1_EXECUTION_PICTURE)
			ret = s7d_av1_execution_picture_complete(&job->execution, true);
		if (!ret && (!job->picture_done || !job->execution.input_consumed))
			ret = -EBADMSG;
		finish = s7d_av1_references_finish(runner->references, lease_epoch,
			input_token, 0, true, !ret && job->picture_done);
		if (finish)
			halted = finish;
		if (!ret && !halted) {
			ret = s7d_av1_references_output(runner->references, &decoded.output);
			decoded.output_ready = !ret;
			if (ret == -EAGAIN)
				ret = 0;
			decoded.picture_decoded = !job->show_existing;
			decoded.show_existing = job->show_existing;
			decoded.input_consumed = job->execution.input_consumed;
			decoded.shift_bytes = job->result.shift_bytes;
		}
	}
	if (job->staged && !halted) {
		int idle = s7d_av1_dma_context_mark_idle(runner->context, lease_epoch,
			input_token, 0, true, !ret && job->result.header_ready);

		if (idle)
			halted = idle;
	}
	if (halted) {
		ret = ret ?: halted;
		runner->quarantined = true;
		s7d_hevc_engine_quarantine(runner->engine, runner->binding);
		goto retained;
	}
	halted = s7d_hevc_engine_unbind(runner->engine, runner->binding);
	if (halted) {
		ret = ret ?: halted;
		runner->quarantined = true;
		goto retained;
	}
	runner->binding = 0;
	if (!ret) {
		if (job->sequence_bytes) {
			runner->sequence = job->result.sequence;
			memcpy(runner->sequence_payload, job->sequence_payload, job->sequence_bytes);
			runner->sequence_bytes = job->sequence_bytes;
			runner->sequence_valid = true;
		}
		job->result.staged_bytes = job->input.decode_size;
		/* Header preparation never consumes the TU or publishes a picture. */
		job->result.input_consumed = false;
		if (header_result)
			*header_result = job->result;
		else
			*decode_result = decoded;
		if (job->decode)
			runner->decode_count++;
	}
	spin_lock_irqsave(&runner->irq_lock, flags);
	runner->job = NULL;
	spin_unlock_irqrestore(&runner->irq_lock, flags);
	runner->failure = ret;
	mutex_unlock(&runner->control);
	kfree(job->source);
	kfree(job);
	return ret;
retained:
	runner->failure = ret;
	mutex_unlock(&runner->control);
	kfree(job->source);
	job->source = NULL;
	return ret;
unlock:
	mutex_unlock(&runner->control);
	kfree(job->source);
	kfree(job);
	return ret;
}

int s7d_av1_runner_headers(struct s7d_av1_runner *runner,
			   struct s7d_codec_session *session, u64 lease_epoch,
			   u64 input_token, const u8 *data, size_t bytes,
			   struct s7d_av1_header_result *result)
{
	return runner_run(runner, session, lease_epoch, input_token, data, bytes, 0, result, NULL);
}

int s7d_av1_runner_decode(struct s7d_av1_runner *runner,
			  struct s7d_codec_session *session, u64 lease_epoch,
			  u64 input_token, const u8 *data, size_t bytes,
			  u64 timestamp_ns, struct s7d_av1_decode_result *result)
{
	return runner_run(runner, session, lease_epoch, input_token, data, bytes, timestamp_ns,
			  NULL, result);
}

int s7d_av1_runner_output_committed(struct s7d_av1_runner *runner,
				    const struct s7d_av1_reference_output *output)
{
	int ret;

	if (!runner || !output)
		return -EINVAL;
	mutex_lock(&runner->control);
	if (!runner->references || !runner->quiesced || runner->job || runner->quarantined)
		ret = -EBUSY;
	else
		ret = s7d_av1_references_output_committed(runner->references, output);
	mutex_unlock(&runner->control);
	return ret;
}

int s7d_av1_runner_close(struct s7d_av1_runner *runner,
			 struct s7d_codec_session *session, u64 lease_epoch)
{
	int ret = 0;

	if (!runner || !session || !lease_epoch)
		return -EINVAL;
	mutex_lock(&runner->control);
	if (!runner->session) {
		if (runner->job || runner->context || runner->references || runner->binding ||
		    runner->preload || runner->quarantined)
			ret = -EBUSY;
		else if (lease_epoch < runner->closed_epoch)
			ret = -ESTALE;
		else {
			runner->closed_epoch = lease_epoch;
			runner->stopping = false;
		}
		goto out;
	}
	if (runner->session != session || runner->epoch != lease_epoch)
		ret = -ESTALE;
	else if (runner->job || runner->binding || runner->preload || runner->quarantined)
		ret = -EBUSY;
	else if (runner->references)
		ret = s7d_av1_references_destroy(runner->references, lease_epoch, 0, runner->quiesced);
	if (!ret) {
		runner->references = NULL;
		if (runner->context)
			ret = s7d_av1_dma_context_destroy(runner->context, lease_epoch);
	}
	if (!ret) {
		runner->context = NULL;
		runner->context_ready = false;
		s7d_codec_session_put(runner->session);
		runner->session = NULL;
		runner->epoch = 0;
		runner->firmware_epoch = 0;
		runner->closed_epoch = lease_epoch;
		runner->last_token = 0;
		runner->failure = 0;
		runner->stopping = false;
		runner->quiesced = false;
		runner->sequence_valid = false;
		runner->sequence_bytes = 0;
		runner->decode_count = 0;
	}
out:
	mutex_unlock(&runner->control);
	return ret;
}

int s7d_av1_runner_destroy(struct s7d_av1_runner *runner)
{
	if (!runner)
		return -EINVAL;
	mutex_lock(&runner->control);
	if (runner->job || runner->session || runner->context || runner->references || runner->binding ||
	    runner->preload || runner->quarantined) {
		mutex_unlock(&runner->control);
		return -EBUSY;
	}
	mutex_unlock(&runner->control);
	put_device(runner->dev);
	kfree(runner);
	return 0;
}
