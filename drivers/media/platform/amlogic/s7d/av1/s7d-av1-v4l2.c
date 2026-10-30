// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "s7d-av1-input.h"
#include "s7d-av1-runner.h"
#include "s7d-av1-v4l2.h"
#include "../common/s7d-codec-dos.h"
#include "../common/s7d-v4l2-device.h"

#define AV1_INPUT_BYTES SZ_2M
#define AV1_STAGING_BYTES (AV1_INPUT_BYTES * 11U)

struct av1_backend {
	struct s7d_av1_v4l2 *adapter;
	struct list_head node;
	struct mutex lock;
	struct mutex notifications;
	struct mutex stop;
	struct work_struct work;
	struct s7d_codec_session *session;
	struct s7d_v4l2_context *frontend;
	struct s7d_v4l2_input input;
	struct s7d_av1_tu tu;
	struct s7d_v4l2_drain drain;
	struct s7d_v4l2_capture_buffer capture[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_codec_layout layout;
	struct s7d_av1_sequence sequence;
	u8 sequence_payload[S7D_AV1_SEQUENCE_MAX_BYTES];
	u32 sequence_bytes;
	struct s7d_av1_decode_result result;
	u64 lease_epoch;
	u64 generation;
	u64 initial_token;
	u64 last_submitted;
	u64 last_completed;
	u64 picture_token;
	u32 next_picture;
	u32 captures;
	u32 frame_sequence;
	int failure;
	int stop_error;
	bool closing;
	bool detached;
	bool input_pending;
	bool preflight;
	bool initial_complete;
	bool sequence_valid;
	bool sequence_notified;
	bool output_pending;
	bool capture_blocked;
	bool execution_attempted;
	bool drain_pending;
	bool drain_notified;
};

struct s7d_av1_v4l2 {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_dos *dos;
	struct s7d_codec_core *core;
	struct s7d_av1_runner *runner;
	struct s7d_v4l2_backend_factory factory;
	struct mutex clients;
	struct mutex resources;
	struct list_head instances;
	struct av1_backend *active;
	u64 lease_epoch;
	int stop_error;
	int release_error;
	bool lease_active;
	bool runner_closed;
	bool quiesced;
	bool release_pin;
	bool closing;
};

static int av1_hold(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	mutex_lock(&adapter->resources);
	if (READ_ONCE(adapter->closing) || adapter->release_error)
		ret = -ESHUTDOWN;
	else if (adapter->lease_epoch == U64_MAX)
		ret = -EOVERFLOW;
	else
		ret = s7d_codec_dos_hold(adapter->dos, engine);
	if (!ret) {
		adapter->lease_epoch++;
		adapter->lease_active = true;
		adapter->runner_closed = true;
		adapter->quiesced = false;
		adapter->stop_error = 0;
	}
	mutex_unlock(&adapter->resources);
	return ret;
}

static int av1_quiesce(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	mutex_lock(&adapter->resources);
	ret = adapter->stop_error;
	if (!ret && !adapter->runner_closed)
		ret = -EBUSY;
	if (!ret)
		ret = s7d_codec_dos_prepare_access(adapter->dos, engine);
	if (!ret)
		ret = s7d_codec_dos_hevc_irq_mask(adapter->dos, false);
	if (!ret)
		ret = s7d_codec_dos_halt(adapter->dos, engine);
	if (!ret)
		adapter->quiesced = true;
	mutex_unlock(&adapter->resources);
	return ret;
}

static void av1_release(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	mutex_lock(&adapter->resources);
	ret = engine != S7D_CODEC_ENGINE_HEVC || !adapter->quiesced ? -EBUSY :
		s7d_codec_dos_release(adapter->dos, engine);
	if (!ret) {
		adapter->lease_active = false;
	} else {
		adapter->release_error = ret;
		if (!adapter->release_pin && try_module_get(adapter->owner))
			adapter->release_pin = true;
		dev_err(adapter->dev, "AV1 resource release failed: %d; owner retained\n", ret);
	}
	mutex_unlock(&adapter->resources);
}

static const struct s7d_codec_resource_ops av1_resource_ops = {
	.hold = av1_hold,
	.quiesce = av1_quiesce,
	.release = av1_release,
};

static int av1_metadata(struct av1_backend *backend)
{
	const u8 *data = s7d_codec_dma_cpu(backend->input.memory);
	const struct s7d_av1_sequence *active;
	const u8 *payload = NULL;
	struct s7d_av1_sequence sequence;
	struct s7d_av1_obu obu;
	struct s7d_codec_colour colour = { };
	u32 offset = 0, payload_bytes = 0, start = 0, i;
	bool picture = false;
	int ret;

	lockdep_assert_held(&backend->lock);
	if (!data || backend->input.bytes > s7d_codec_dma_size(backend->input.memory))
		return -EINVAL;
	/*
	 * GStreamer queues the caps codec_data, an ISOBMFF av1C record, by
	 * itself. Its first byte has the OBU forbidden bit set; its config OBUs
	 * follow a four-byte header.
	 */
	if (backend->input.bytes > 4 && data[0] == 0x81)
		offset = start = 4;
	while (offset < backend->input.bytes) {
		ret = s7d_av1_obu_next(data, backend->input.bytes, offset, &obu);
		if (ret)
			return ret;
		offset += obu.bytes;
		picture |= obu.type == S7D_AV1_OBU_FRAME || obu.type == S7D_AV1_OBU_FRAME_HEADER;
		if (obu.type != S7D_AV1_OBU_SEQUENCE)
			continue;
		if (payload)
			return -EOPNOTSUPP;
		payload = data + obu.payload_offset;
		payload_bytes = obu.payload_bytes;
		ret = s7d_av1_parse_sequence(payload, payload_bytes, &sequence);
		if (!ret)
			ret = s7d_av1_admit_sequence(&sequence);
		if (ret)
			return ret;
	}
	if (!payload && !backend->sequence_valid)
		return -EPIPE;
	active = payload ? &sequence : &backend->sequence;
	if (!picture) {
		/* Headers only: take the sequence, decode nothing. */
		if (!payload)
			return -EBADMSG;
		memset(&backend->tu, 0, sizeof(backend->tu));
	} else {
		ret = s7d_av1_split_tu(data + start, backend->input.bytes - start, active,
				       &backend->tu);
		if (ret)
			return ret;
		for (i = 0; i < backend->tu.picture_count; i++)
			backend->tu.picture[i].offset += start;
	}
	if (!payload)
		return 0;
	if (sequence.max_frame_width < 64 || sequence.max_frame_height < 64 ||
	    sequence.max_frame_width > 1920 || sequence.max_frame_height > 1080 ||
	    ((sequence.max_frame_width | sequence.max_frame_height) & 1))
		return -EOPNOTSUPP;
	if (backend->sequence_valid)
		return backend->sequence_bytes != payload_bytes ||
			memcmp(payload, backend->sequence_payload, payload_bytes) ? -EOPNOTSUPP : 0;
	ret = s7d_v4l2_default_nv12_layout(sequence.max_frame_width,
					sequence.max_frame_height,
					V4L2_PIX_FMT_NV12M, &backend->layout);
	if (ret)
		return ret;
	colour.flags = S7D_CODEC_COLOUR_SIGNAL_PRESENT;
	if (sequence.color_range)
		colour.flags |= S7D_CODEC_COLOUR_FULL_RANGE;
	if (sequence.color_description_present) {
		colour.flags |= S7D_CODEC_COLOUR_DESCRIPTION_PRESENT;
		colour.primaries = sequence.color_primaries;
		colour.transfer = sequence.transfer_characteristics;
		colour.matrix = sequence.matrix_coefficients;
	}
	ret = s7d_codec_resolve_colour(&colour, sequence.max_frame_width,
				     sequence.max_frame_height, &backend->layout.colourimetry);
	if (ret)
		return ret;
	backend->sequence = sequence;
	memcpy(backend->sequence_payload, payload, payload_bytes);
	backend->sequence_bytes = payload_bytes;
	backend->sequence_valid = true;
	return 0;
}

static int av1_notify_sequence(struct av1_backend *backend, bool *cancelled)
{
	int ret;

	*cancelled = false;
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		ret = s7d_v4l2_sequence_result(backend->frontend, &backend->layout, cancelled);
	else {
		*cancelled = true;
		ret = -ECANCELED;
	}
	mutex_unlock(&backend->notifications);
	return ret;
}

static void av1_notify_error(struct av1_backend *backend, int error)
{
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		s7d_v4l2_error(backend->frontend, error);
	mutex_unlock(&backend->notifications);
}

static int av1_complete_input(struct av1_backend *backend, u64 token, int status)
{
	bool cancelled = false;
	int ret;

	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing)) {
		if (status)
			s7d_v4l2_error(backend->frontend, status);
		ret = s7d_v4l2_input_done_result(backend->frontend, token, status, &cancelled);
	} else {
		cancelled = true;
		ret = -ECANCELED;
	}
	mutex_unlock(&backend->notifications);
	if (ret && !cancelled) {
		mutex_lock(&backend->lock);
		backend->failure = ret;
		mutex_unlock(&backend->lock);
		av1_notify_error(backend, ret);
	}
	return ret;
}

static bool av1_copy_cancelled(void *priv)
{
	struct av1_backend *backend = priv;

	return READ_ONCE(backend->closing);
}

static int av1_publish(struct av1_backend *backend)
{
	const struct s7d_av1_reference_output *output = &backend->result.output;
	struct s7d_codec_dma_copy_plane plane[2];
	struct s7d_codec_surface *surface = NULL;
	struct s7d_codec_frame frame = { };
	u64 token = 0;
	u32 i, capture_index;
	int ret = -EAGAIN, cleanup;

	if (output->output.picture.width != backend->layout.coded_width ||
	    output->output.picture.height != backend->layout.coded_height ||
	    output->pitch != backend->layout.pitch[0])
		return -EOPNOTSUPP;
	for (i = 0; i < backend->captures; i++) {
		surface = backend->capture[i].surface;
		ret = s7d_codec_surface_acquire(surface, &token);
		if (ret != -EAGAIN)
			break;
	}
	if (i == backend->captures)
		return -EAGAIN;
	if (ret)
		return ret;
	capture_index = i;
	ret = s7d_codec_surface_ref_get(surface);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++)
		plane[i] = (struct s7d_codec_dma_copy_plane) {
			.memory = output->memory,
			.offset = output->source_offset[i],
			.source_pitch = output->pitch,
			.destination_pitch = backend->layout.pitch[i],
			.row_bytes = backend->layout.coded_width,
			.rows = backend->layout.coded_height >> i,
		};
	ret = s7d_codec_surface_copy_dma(surface, token, backend->generation, plane,
					av1_copy_cancelled, backend);
	cleanup = s7d_codec_surface_written(surface, token);
	if (!ret)
		ret = cleanup;
	if (!ret && av1_copy_cancelled(backend))
		ret = -ECANCELED;
	if (ret) {
		if (!cleanup)
			s7d_codec_surface_discard(surface, token);
		return ret;
	}
	frame.surface_id = backend->capture[capture_index].surface_id;
	frame.generation = backend->generation;
	frame.write_token = token;
	frame.timestamp_ns = output->output.metadata.timestamp_ns;
	frame.timestamp_meta = backend->input.timestamp_meta;
	frame.sequence = backend->frame_sequence;
	if (output->output.picture.frame_type == S7D_AV1_KEY_FRAME)
		frame.flags = S7D_CODEC_FRAME_KEY;
	frame.bytesused[0] = backend->layout.pitch[0] * backend->layout.coded_height;
	frame.bytesused[1] = frame.bytesused[0] / 2;
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		ret = s7d_codec_session_frame_ready(backend->session, &frame);
	else
		ret = -ECANCELED;
	mutex_unlock(&backend->notifications);
	if (ret) {
		s7d_codec_surface_discard(surface, token);
		return ret;
	}
	ret = s7d_codec_surface_ref_put(surface);
	if (!ret)
		ret = s7d_av1_runner_output_committed(backend->adapter->runner, output);
	if (!ret)
		backend->frame_sequence++;
	return ret;
}

static void av1_work(struct work_struct *work)
{
	struct av1_backend *backend = container_of(work, struct av1_backend, work);
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	u64 token = 0;
	bool cancelled;
	int ret;

	for (;;) {
		mutex_lock(&backend->lock);
		if (backend->closing || backend->failure || backend->capture_blocked)
			goto unlock;
		if (backend->input_pending && !backend->preflight) {
			token = backend->input.token;
			ret = av1_metadata(backend);
			if (ret)
				goto failed;
			backend->preflight = true;
			mutex_unlock(&backend->lock);
			cancelled = false;
			ret = backend->sequence_notified ? 0 :
				av1_notify_sequence(backend, &cancelled);
			mutex_lock(&backend->lock);
			if (ret) {
				if (cancelled && backend->closing)
					goto unlock;
				goto failed;
			}
			backend->sequence_notified = true;
			if (token == backend->initial_token)
				backend->initial_complete = true;
			mutex_unlock(&backend->lock);
			continue;
		}
		if (backend->input_pending && backend->preflight && !backend->tu.picture_count) {
			token = backend->input.token;
			backend->input_pending = false;
			backend->preflight = false;
			backend->last_completed = token;
			mutex_unlock(&backend->lock);
			if (av1_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->input_pending && backend->preflight && backend->captures) {
			token = backend->input.token;
			if (!backend->output_pending) {
				u32 offset = backend->tu.picture[backend->next_picture].offset;
				u32 bytes = backend->tu.picture[backend->next_picture].bytes;
				u64 picture_token;
				const u8 *data;

				if (backend->picture_token == U64_MAX) {
					ret = -EOVERFLOW;
					goto failed;
				}
				picture_token = ++backend->picture_token;
				data = s7d_codec_dma_cpu(backend->input.memory);
				backend->execution_attempted = true;
				mutex_lock(&adapter->resources);
				adapter->runner_closed = false;
				mutex_unlock(&adapter->resources);
				mutex_unlock(&backend->lock);
				memset(&backend->result, 0, sizeof(backend->result));
				ret = s7d_av1_runner_decode(adapter->runner, backend->session,
					backend->lease_epoch, picture_token, data + offset, bytes,
					backend->input.timestamp_ns, &backend->result);
				mutex_lock(&backend->lock);
				backend->output_pending = backend->result.output_ready;
				if (ret) {
					if (ret == -ECANCELED && backend->closing)
						goto unlock;
					goto failed;
				}
				if (backend->closing)
					goto unlock;
				if (!backend->result.input_consumed ||
				    (!backend->result.picture_decoded && !backend->result.show_existing)) {
					ret = -EIO;
					goto failed;
				}
			}
			if (backend->output_pending) {
				mutex_unlock(&backend->lock);
				ret = av1_publish(backend);
				mutex_lock(&backend->lock);
				if (ret == -EAGAIN)
					goto unlock;
				if (ret) {
					if (ret == -ECANCELED && backend->closing)
						goto unlock;
					goto failed;
				}
				backend->output_pending = false;
			}
			backend->next_picture++;
			if (backend->next_picture < backend->tu.picture_count) {
				mutex_unlock(&backend->lock);
				continue;
			}
			backend->input_pending = false;
			backend->preflight = false;
			backend->last_completed = token;
			mutex_unlock(&backend->lock);
			if (av1_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->drain_pending && !backend->drain_notified &&
		    !backend->input_pending && !backend->output_pending) {
			struct s7d_v4l2_drain drain = backend->drain;

			mutex_unlock(&backend->lock);
			mutex_lock(&backend->notifications);
			cancelled = false;
			if (backend->frontend && !READ_ONCE(backend->closing))
				ret = s7d_v4l2_drain_done_result(backend->frontend, drain.epoch,
								 drain.last_input_token, &cancelled);
			else {
				cancelled = true;
				ret = -ECANCELED;
			}
			mutex_unlock(&backend->notifications);
			mutex_lock(&backend->lock);
			if (cancelled && backend->closing)
				goto unlock;
			if (ret) {
				backend->failure = ret;
				mutex_unlock(&backend->lock);
				av1_notify_error(backend, ret);
				return;
			}
			backend->drain_notified = true;
		}
unlock:
		mutex_unlock(&backend->lock);
		return;
failed:
		backend->failure = ret;
		backend->input_pending = false;
		mutex_unlock(&backend->lock);
		av1_complete_input(backend, token, ret);
		return;
	}
}

static int av1_prepare(void *priv, struct s7d_codec_session *session,
		       struct s7d_v4l2_context *frontend)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret = 0;

	if (!session || !frontend)
		return -EINVAL;
	mutex_lock(&adapter->clients);
	mutex_lock(&backend->lock);
	if (adapter->closing || backend->closing || backend->session || adapter->active) {
		ret = -EBUSY;
	} else {
		s7d_codec_session_get(session);
		backend->session = session;
		backend->frontend = frontend;
		mutex_lock(&adapter->resources);
		backend->lease_epoch = adapter->lease_epoch;
		mutex_unlock(&adapter->resources);
		adapter->active = backend;
	}
	mutex_unlock(&backend->lock);
	mutex_unlock(&adapter->clients);
	return ret;
}

static int av1_submit(void *priv, const struct s7d_v4l2_input *input)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!input || !input->memory || !input->bytes || !input->token ||
		 input->bytes > AV1_INPUT_BYTES || input->bytes > s7d_codec_dma_size(input->memory))
		ret = -EINVAL;
	else if (backend->input_pending || backend->output_pending || backend->drain_pending ||
		 input->token <= backend->last_submitted)
		ret = -EBUSY;
	else {
		backend->input = *input;
		memset(&backend->tu, 0, sizeof(backend->tu));
		backend->next_picture = 0;
		backend->last_submitted = input->token;
		if (!backend->initial_token)
			backend->initial_token = input->token;
		backend->input_pending = true;
		backend->preflight = false;
		queue_work(system_unbound_wq, &backend->work);
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_sync_initial(void *priv, u64 token)
{
	struct av1_backend *backend = priv;
	int ret;

	flush_work(&backend->work);
	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!token || token != backend->initial_token || backend->captures)
		ret = -ESTALE;
	else
		ret = backend->initial_complete ? 0 : -EAGAIN;
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_capture_start(void *priv, const struct s7d_codec_layout *layout,
			     u64 generation,
			     const struct s7d_v4l2_capture_buffer *buffers, unsigned int count)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!layout || !buffers || !generation || !backend->sequence_valid ||
		 backend->captures || count > ARRAY_SIZE(backend->capture))
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = s7d_codec_validate_layout(layout);
	if (!ret && (memcmp(&backend->layout, layout, sizeof(*layout)) ||
		     count < layout->min_capture_buffers))
		ret = -EINVAL;
	if (!ret)
		ret = s7d_v4l2_validate_capture_pool(layout, generation, buffers, count);
	if (!ret) {
		memcpy(backend->capture, buffers, sizeof(*buffers) * count);
		backend->captures = count;
		backend->generation = generation;
		backend->capture_blocked = false;
		queue_work(system_unbound_wq, &backend->work);
	}
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_capture_stop_check(struct av1_backend *backend, u64 generation)
{
	if (backend->closing || backend->failure || !backend->session)
		return backend->failure ? backend->failure : -ESHUTDOWN;
	if (!generation || !backend->sequence_valid ||
	    (backend->captures && backend->generation != generation))
		return -ESTALE;
	return backend->execution_attempted || backend->drain_pending ? -EOPNOTSUPP : 0;
}

static int av1_capture_stop(void *priv, u64 generation)
{
	struct av1_backend *backend = priv;
	int ret;

	mutex_lock(&backend->lock);
	ret = av1_capture_stop_check(backend, generation);
	if (!ret)
		backend->capture_blocked = true;
	mutex_unlock(&backend->lock);
	if (ret)
		return ret;
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	mutex_unlock(&backend->notifications);
	mutex_lock(&backend->lock);
	ret = av1_capture_stop_check(backend, generation);
	if (!ret) {
		memset(backend->capture, 0, sizeof(backend->capture));
		backend->captures = 0;
		backend->generation = 0;
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_capture_queue(void *priv, struct s7d_codec_surface *surface,
			    u64 surface_id, u64 generation)
{
	struct av1_backend *backend = priv;
	u32 i;
	int ret = -ESTALE;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || backend->capture_blocked) {
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
		goto out;
	}
	for (i = 0; i < backend->captures; i++)
		if (backend->capture[i].surface == surface &&
		    backend->capture[i].surface_id == surface_id &&
		    backend->capture[i].generation == generation) {
			queue_work(system_unbound_wq, &backend->work);
			ret = 0;
			break;
		}
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_drain(void *priv, const struct s7d_v4l2_drain *request)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!request || !request->epoch || backend->input_pending ||
		 backend->output_pending || backend->drain_pending ||
		 request->last_input_token != backend->last_completed)
		ret = -EBUSY;
	else {
		backend->drain = *request;
		backend->drain_pending = true;
		queue_work(system_unbound_wq, &backend->work);
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_resume(void *priv)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	flush_work(&backend->work);
	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || backend->input_pending ||
	    backend->output_pending || !backend->drain_notified)
		ret = backend->failure ? backend->failure : -EBUSY;
	else {
		memset(&backend->drain, 0, sizeof(backend->drain));
		backend->drain_pending = false;
		backend->drain_notified = false;
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static void av1_detach(void *priv)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret = 0;

	mutex_lock(&backend->stop);
	if (backend->detached)
		goto unlock;
	mutex_lock(&backend->lock);
	backend->closing = true;
	if (backend->session)
		s7d_av1_runner_request_stop(adapter->runner);
	mutex_unlock(&backend->lock);
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	backend->frontend = NULL;
	mutex_unlock(&backend->notifications);
	if (backend->session) {
		if (backend->output_pending) {
			ret = s7d_av1_runner_output_committed(adapter->runner, &backend->result.output);
			if (!ret)
				backend->output_pending = false;
		}
		if (!ret)
			ret = s7d_av1_runner_close(adapter->runner, backend->session, backend->lease_epoch);
		mutex_lock(&adapter->resources);
		adapter->stop_error = ret;
		adapter->runner_closed = !ret;
		mutex_unlock(&adapter->resources);
	}
	backend->stop_error = ret;
	backend->detached = true;
unlock:
	mutex_unlock(&backend->stop);
}

static int av1_release_epoch(struct av1_backend *backend)
{
	struct s7d_codec_session *session;
	int ret;

	lockdep_assert_held(&backend->adapter->clients);
	if (!backend->closing)
		return -EBUSY;
	if (backend->stop_error)
		return backend->stop_error;
	if (backend->session) {
		ret = s7d_codec_session_stop(backend->session);
		if (ret)
			return ret;
	}
	mutex_lock(&backend->lock);
	if (backend->adapter->active == backend)
		backend->adapter->active = NULL;
	session = backend->session;
	backend->session = NULL;
	mutex_unlock(&backend->lock);
	if (session)
		s7d_codec_session_put(session);
	return 0;
}

static int av1_reset(void *priv)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret;

	mutex_lock(&adapter->clients);
	ret = adapter->closing ? -ESHUTDOWN : av1_release_epoch(backend);
	if (!ret) {
		mutex_lock(&backend->lock);
		memset(&backend->input, 0, sizeof(backend->input));
		memset(&backend->tu, 0, sizeof(backend->tu));
		memset(&backend->drain, 0, sizeof(backend->drain));
		memset(&backend->result, 0, sizeof(backend->result));
		memset(backend->capture, 0, sizeof(backend->capture));
		backend->lease_epoch = 0;
		backend->generation = 0;
		backend->initial_token = 0;
		backend->last_submitted = 0;
		backend->last_completed = 0;
		backend->picture_token = 0;
		backend->next_picture = 0;
		backend->captures = 0;
		backend->frame_sequence = 0;
		backend->failure = 0;
		backend->stop_error = 0;
		backend->input_pending = false;
		backend->preflight = false;
		backend->initial_complete = false;
		backend->output_pending = false;
		backend->capture_blocked = false;
		backend->execution_attempted = false;
		backend->drain_pending = false;
		backend->drain_notified = false;
		backend->closing = false;
		backend->detached = false;
		mutex_unlock(&backend->lock);
	}
	mutex_unlock(&adapter->clients);
	return ret;
}

/* An OUTPUT-only seek keeps the sequence header; a new stream forgets it. */
static int av1_clear_stream(void *priv)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret = 0;

	mutex_lock(&adapter->clients);
	mutex_lock(&backend->lock);
	if (adapter->closing || backend->closing || backend->failure) {
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	} else if (backend->session || backend->frontend || adapter->active == backend ||
		   backend->generation || backend->captures || backend->input_pending ||
		   backend->execution_attempted || backend->drain_pending ||
		   work_busy(&backend->work)) {
		ret = -EBUSY;
	} else {
		memset(&backend->layout, 0, sizeof(backend->layout));
		memset(&backend->sequence, 0, sizeof(backend->sequence));
		memset(backend->sequence_payload, 0, sizeof(backend->sequence_payload));
		backend->sequence_bytes = 0;
		backend->sequence_valid = false;
		backend->sequence_notified = false;
	}
	mutex_unlock(&backend->lock);
	mutex_unlock(&adapter->clients);
	return ret;
}

static const struct s7d_v4l2_backend_ops av1_ops = {
	.prepare = av1_prepare,
	.submit = av1_submit,
	.sync_initial = av1_sync_initial,
	.capture_start = av1_capture_start,
	.capture_stop = av1_capture_stop,
	.capture_queue = av1_capture_queue,
	.drain = av1_drain,
	.resume = av1_resume,
	.detach = av1_detach,
	.reset = av1_reset,
	.clear_stream = av1_clear_stream,
};

static int av1_factory_ready(void *priv)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	mutex_lock(&adapter->resources);
	ret = READ_ONCE(adapter->closing) || adapter->release_error ? -ESHUTDOWN : 0;
	mutex_unlock(&adapter->resources);
	return ret;
}

static void *av1_factory_create(void *priv)
{
	struct s7d_av1_v4l2 *adapter = priv;
	struct av1_backend *backend;

	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&adapter->clients);
	if (adapter->closing || !try_module_get(adapter->owner)) {
		mutex_unlock(&adapter->clients);
		kfree(backend);
		return ERR_PTR(-ESHUTDOWN);
	}
	backend->adapter = adapter;
	mutex_init(&backend->lock);
	mutex_init(&backend->notifications);
	mutex_init(&backend->stop);
	INIT_WORK(&backend->work, av1_work);
	list_add_tail(&backend->node, &adapter->instances);
	mutex_unlock(&adapter->clients);
	return backend;
}

static void av1_factory_destroy(void *priv, void *instance)
{
	struct s7d_av1_v4l2 *adapter = priv;
	struct av1_backend *backend = instance;
	int ret;

	mutex_lock(&adapter->clients);
	av1_detach(backend);
	ret = av1_release_epoch(backend);
	if (ret) {
		dev_err(adapter->dev, "AV1 instance stop failed: %d; owner retained\n", ret);
		mutex_unlock(&adapter->clients);
		return;
	}
	list_del(&backend->node);
	mutex_unlock(&adapter->clients);
	module_put(adapter->owner);
	kfree(backend);
}

struct s7d_av1_v4l2 *
s7d_av1_v4l2_create(struct device *dev, struct s7d_codec_dos *dos,
		      struct s7d_codec_firmware_owner *firmware,
		      struct s7d_hevc_engine *engine, struct module *owner)
{
	struct s7d_av1_v4l2 *adapter;
	int ret;

	if (!dev || !dos || !firmware || !engine)
		return ERR_PTR(-EINVAL);
	adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return ERR_PTR(-ENOMEM);
	adapter->dev = get_device(dev);
	adapter->owner = owner;
	adapter->dos = dos;
	mutex_init(&adapter->clients);
	mutex_init(&adapter->resources);
	INIT_LIST_HEAD(&adapter->instances);
	adapter->runner = s7d_av1_runner_create(dev, dos, firmware, engine, AV1_STAGING_BYTES);
	if (IS_ERR(adapter->runner)) {
		ret = PTR_ERR(adapter->runner);
		goto free_adapter;
	}
	adapter->core = s7d_codec_core_create(dev, owner, &av1_resource_ops, adapter, 32);
	if (IS_ERR(adapter->core)) {
		ret = PTR_ERR(adapter->core);
		s7d_av1_runner_destroy(adapter->runner);
		goto free_adapter;
	}
	adapter->factory = (struct s7d_v4l2_backend_factory) {
		.owner = owner,
		.codec = {
			.engine = S7D_CODEC_ENGINE_HEVC,
			.output_format = V4L2_PIX_FMT_AV1,
			.max_width = 1920, .max_height = 1080,
			.max_input_size = AV1_INPUT_BYTES,
			.min_input_size = AV1_INPUT_BYTES,
			.default_layout = s7d_v4l2_default_nv12_layout,
		},
		.ops = &av1_ops, .ready = av1_factory_ready,
		.create = av1_factory_create, .destroy = av1_factory_destroy,
	};
	return adapter;

free_adapter:
	put_device(adapter->dev);
	kfree(adapter);
	return ERR_PTR(ret);
}

struct s7d_codec_core *s7d_av1_v4l2_core(struct s7d_av1_v4l2 *adapter)
{
	return adapter->core;
}

const struct s7d_v4l2_backend_factory *
s7d_av1_v4l2_factory(struct s7d_av1_v4l2 *adapter)
{
	return &adapter->factory;
}

int s7d_av1_v4l2_suspend_check(struct s7d_av1_v4l2 *adapter)
{
	int ret;

	mutex_lock(&adapter->clients);
	mutex_lock(&adapter->resources);
	ret = adapter->active || adapter->lease_active || adapter->release_error ? -EBUSY : 0;
	mutex_unlock(&adapter->resources);
	if (!ret)
		ret = s7d_codec_dos_suspend_check(adapter->dos);
	mutex_unlock(&adapter->clients);
	return ret;
}

int s7d_av1_v4l2_shutdown(struct s7d_av1_v4l2 *adapter)
{
	struct av1_backend *backend;
	int ret = 0;

	mutex_lock(&adapter->clients);
	adapter->closing = true;
	list_for_each_entry(backend, &adapter->instances, node) {
		av1_detach(backend);
		if (!ret)
			ret = backend->stop_error;
	}
	mutex_unlock(&adapter->clients);
	return ret;
}

int s7d_av1_v4l2_destroy(struct s7d_av1_v4l2 *adapter)
{
	int ret;

	if (!adapter)
		return -EINVAL;
	mutex_lock(&adapter->clients);
	adapter->closing = true;
	if (!list_empty(&adapter->instances)) {
		ret = -EBUSY;
		goto unlock;
	}
	mutex_lock(&adapter->resources);
	ret = adapter->lease_active || adapter->release_error ? -EBUSY : 0;
	mutex_unlock(&adapter->resources);
	if (ret)
		goto unlock;
	if (adapter->core) {
		ret = s7d_codec_core_destroy(adapter->core);
		if (ret)
			goto unlock;
		adapter->core = NULL;
	}
	ret = s7d_av1_runner_destroy(adapter->runner);
unlock:
	mutex_unlock(&adapter->clients);
	if (ret)
		return ret;
	put_device(adapter->dev);
	kfree(adapter);
	return 0;
}
