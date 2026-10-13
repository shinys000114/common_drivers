// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "s7d-hevc-backend.h"
#include "s7d-hevc-context.h"
#include "s7d-hevc-sequence.h"
#include "s7d-hevc-v4l2.h"
#include "s7d-codec-dos.h"
#include "s7d-v4l2-device.h"

#define HEVC_PARAMETER_BYTES SZ_64K
#define HEVC_PREFIX_BYTES 16U

struct hevc_parameter {
	u8 *data;
	size_t bytes;
};

struct hevc_backend {
	struct s7d_hevc_v4l2 *adapter;
	struct list_head node;
	struct mutex lock; /* Admission, metadata and capture bindings. */
	struct mutex notifications; /* Frontend lifetime and callbacks. */
	struct work_struct work;
	struct s7d_codec_session *session;
	struct s7d_v4l2_context *frontend;
	struct s7d_v4l2_input input;
	struct s7d_v4l2_drain drain;
	struct s7d_v4l2_capture_buffer capture[S7D_HEVC_MAX_CAPTURE_BUFFERS];
	struct hevc_parameter parameter[3];
	struct s7d_hevc_sps sps;
	struct s7d_codec_layout layout;
	u8 *au;
	size_t au_bytes;
	u64 generation;
	u64 last_submitted;
	u64 last_completed;
	u64 initial_token;
	u32 captures;
	int failure;
	bool closing;
	bool input_pending;
	bool preflight;
	bool initial_complete;
	bool sequence_valid;
	bool sequence_notified;
	bool picture_done;
	bool capture_blocked;
	bool execution_attempted;
	bool drain_pending;
	bool drain_notified;
};

struct s7d_hevc_v4l2 {
	struct device *dev;
	struct module *owner;
	struct s7d_hevc_decoder *decoder;
	struct s7d_codec_dos *dos;
	struct s7d_v4l2_backend_factory factory;
	struct mutex clients; /* Instance lifetime and shutdown. */
	struct list_head instances;
	struct hevc_backend *active;
	bool closing;
};

static size_t hevc_start_code(const u8 *data, size_t bytes, size_t pos)
{
	if (bytes - pos < 3 || data[pos] || data[pos + 1])
		return 0;
	if (data[pos + 2] == 1)
		return 3;
	return bytes - pos >= 4 && !data[pos + 2] && data[pos + 3] == 1 ? 4 : 0;
}

static int hevc_rbsp_prefix(const u8 *data, size_t bytes, u8 prefix[], u32 *count)
{
	u32 zeros = 0, kept = 0;
	size_t i;

	for (i = 0; i < bytes; i++) {
		if (zeros >= 2 && data[i] <= 3) {
			if (data[i] != 3 || i + 1 == bytes || data[i + 1] > 3)
				return -EBADMSG;
			zeros = 0;
			continue;
		}
		if (kept < HEVC_PREFIX_BYTES)
			prefix[kept++] = data[i];
		zeros = data[i] ? 0 : zeros + 1;
	}
	*count = kept;
	return kept ? 0 : -EBADMSG;
}

static int hevc_ue(const u8 *data, u32 bytes, u32 *bit, u32 *value)
{
	u32 zeros = 0, result = 1;
	bool one = false;

	while (*bit < bytes * 8) {
		one = data[*bit / 8] & (0x80 >> (*bit & 7));
		(*bit)++;
		if (one)
			break;
		if (++zeros > 31)
			return -EBADMSG;
	}
	if (!one || zeros > bytes * 8 - *bit)
		return -EBADMSG;
	while (zeros--) {
		result = (result << 1) | !!(data[*bit / 8] & (0x80 >> (*bit & 7)));
		(*bit)++;
	}
	*value = result - 1;
	return 0;
}

static bool hevc_same_sps(const struct s7d_hevc_sps *a,
			  const struct s7d_hevc_sps *b)
{
	return a->vps_id == b->vps_id &&
	       a->sps_id == b->sps_id &&
	       a->profile_idc == b->profile_idc &&
	       a->level_idc == b->level_idc &&
	       a->max_sub_layers == b->max_sub_layers &&
	       a->coded_width == b->coded_width &&
	       a->coded_height == b->coded_height &&
	       a->crop_left == b->crop_left &&
	       a->crop_right == b->crop_right &&
	       a->crop_top == b->crop_top &&
	       a->crop_bottom == b->crop_bottom &&
	       a->chroma_format_idc == b->chroma_format_idc &&
	       a->bit_depth_luma == b->bit_depth_luma &&
	       a->bit_depth_chroma == b->bit_depth_chroma &&
	       a->log2_min_cb_size == b->log2_min_cb_size &&
	       a->log2_ctb_size == b->log2_ctb_size &&
	       a->log2_max_poc_lsb == b->log2_max_poc_lsb &&
	       a->max_dec_pic_buffering == b->max_dec_pic_buffering &&
	       a->num_reorder_pics == b->num_reorder_pics &&
	       a->separate_colour_plane == b->separate_colour_plane &&
	       a->interlaced_source == b->interlaced_source &&
	       a->field_seq == b->field_seq &&
	       a->colour.primaries == b->colour.primaries &&
	       a->colour.transfer == b->colour.transfer &&
	       a->colour.matrix == b->colour.matrix &&
	       a->colour.flags == b->colour.flags;
}

static int hevc_check_parameters(const struct hevc_parameter parameter[],
				 const struct s7d_hevc_sps *sps, bool valid)
{
	u8 prefix[HEVC_PREFIX_BYTES];
	u32 count, bit = 0, pps, id;
	int ret;

	if (valid && parameter[0].data && (parameter[0].data[6] >> 4) != sps->vps_id)
		return -EBADMSG;
	if (!parameter[2].data)
		return 0;
	ret = hevc_rbsp_prefix(parameter[2].data + 6, parameter[2].bytes - 6,
			       prefix, &count);
	if (!ret)
		ret = hevc_ue(prefix, count, &bit, &pps);
	if (!ret)
		ret = hevc_ue(prefix, count, &bit, &id);
	if (!ret && (pps > 63 || id >= 8 || (valid && id != sps->sps_id)))
		ret = -EBADMSG;
	return ret;
}

static int hevc_metadata(struct hevc_backend *backend)
{
	struct hevc_parameter next[3];
	struct s7d_hevc_sps sps = backend->sps, checked;
	struct s7d_codec_layout layout = backend->layout;
	const u8 *data = s7d_codec_dma_cpu(backend->input.memory);
	const u8 *idr = NULL;
	size_t bytes = backend->input.bytes, idr_bytes = 0, pos = 0, total = 0;
	u32 seen = 0, nals = 0, i;
	u8 *au = NULL;
	bool valid = backend->sequence_valid;
	int ret = 0;

	lockdep_assert_held(&backend->lock);
	if (!data || !bytes || bytes > S7D_HEVC_INPUT_MAX_BYTES || backend->au)
		return -EINVAL;
	memcpy(next, backend->parameter, sizeof(next));
	while (pos < bytes && !hevc_start_code(data, bytes, pos) && !data[pos])
		pos++;
	while (pos < bytes) {
		u8 prefix[HEVC_PREFIX_BYTES];
		u32 type, prefix_bytes;
		size_t code = hevc_start_code(data, bytes, pos);
		size_t start = pos + code, end;

		if (!code || ++nals > 64) {
			ret = -EBADMSG;
			goto out;
		}
		for (pos = start; pos < bytes; pos++)
			if (hevc_start_code(data, bytes, pos))
				break;
		end = pos;
		while (end > start && !data[end - 1])
			end--;
		if (end - start < 3 || (data[start] & 0x81) || data[start + 1] != 1) {
			ret = -EBADMSG;
			goto out;
		}
		type = (data[start] >> 1) & 0x3f;
		ret = hevc_rbsp_prefix(data + start + 2, end - start - 2,
				       prefix, &prefix_bytes);
		if (ret)
			goto out;
		if (type >= 32 && type <= 34) {
			u32 index = type - 32;

			if (idr || (seen & BIT(index)) || end - start > HEVC_PARAMETER_BYTES) {
				ret = -EOPNOTSUPP;
				goto out;
			}
			next[index].data = kmalloc(end - start + 4, GFP_KERNEL);
			if (!next[index].data) {
				ret = -ENOMEM;
				goto out;
			}
			next[index].bytes = end - start + 4;
			memcpy(next[index].data, "\0\0\0\1", 4);
			memcpy(next[index].data + 4, data + start, end - start);
			seen |= BIT(index);
			if (index == 1) {
				ret = s7d_hevc_parse_sps(data + start, end - start, &sps);
				if (!ret && (sps.sps_id >= 8 || sps.max_sub_layers != 1))
					ret = -EOPNOTSUPP;
				if (!ret && valid && !hevc_same_sps(&backend->sps, &sps))
					ret = -EOPNOTSUPP;
				if (!ret)
					ret = s7d_hevc_build_layout(&sps, S7D_CODEC_CAPTURE_NV12M,
							    0, &layout);
				if (ret)
					goto out;
				valid = true;
			}
		} else if (type == 19 || type == 20) {
			if (idr) {
				ret = -EOPNOTSUPP;
				goto out;
			}
			idr = data + start;
			idr_bytes = end - start;
		} else if (type != 35 && type != 39 && type != 40) {
			ret = -EOPNOTSUPP;
			goto out;
		}
	}
	if (!nals) {
		ret = -EBADMSG;
		goto out;
	}
	ret = hevc_check_parameters(next, &sps, valid);
	if (ret)
		goto out;
	if (idr) {
		if (!valid || !next[0].data || !next[1].data || !next[2].data) {
			ret = -EBADMSG;
			goto out;
		}
		for (i = 0; i < 3; i++)
			if (check_add_overflow(total, next[i].bytes, &total)) {
				ret = -EOVERFLOW;
				goto out;
			}
		if (check_add_overflow(total, idr_bytes + 4, &total) ||
		    total > S7D_HEVC_INPUT_MAX_BYTES) {
			ret = -E2BIG;
			goto out;
		}
		au = kvmalloc(total, GFP_KERNEL);
		if (!au) {
			ret = -ENOMEM;
			goto out;
		}
		pos = 0;
		for (i = 0; i < 3; i++) {
			memcpy(au + pos, next[i].data, next[i].bytes);
			pos += next[i].bytes;
		}
		memcpy(au + pos, "\0\0\0\1", 4);
		memcpy(au + pos + 4, idr, idr_bytes);
		ret = s7d_hevc_input_validate_annexb(au, total, &checked);
		if (!ret && !hevc_same_sps(&checked, &sps))
			ret = -EBADMSG;
		if (ret)
			goto out;
	}
	for (i = 0; i < 3; i++)
		if (seen & BIT(i)) {
			kfree(backend->parameter[i].data);
			backend->parameter[i] = next[i];
		}
	backend->sps = sps;
	backend->layout = layout;
	backend->sequence_valid = valid;
	backend->au = au;
	backend->au_bytes = total;
	return 0;
out:
	for (i = 0; i < 3; i++)
		if (seen & BIT(i))
			kfree(next[i].data);
	kvfree(au);
	return ret;
}

static int hevc_notify_sequence(struct hevc_backend *backend)
{
	int ret = -ESHUTDOWN;

	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		ret = s7d_v4l2_sequence(backend->frontend, &backend->layout);
	mutex_unlock(&backend->notifications);
	return ret;
}

static int hevc_notify_input(struct hevc_backend *backend, u64 token, int status)
{
	int ret = -ESHUTDOWN;

	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing)) {
		if (status)
			s7d_v4l2_error(backend->frontend, status);
		ret = s7d_v4l2_input_done(backend->frontend, token, status);
	}
	mutex_unlock(&backend->notifications);
	return ret;
}

static void hevc_notify_error(struct hevc_backend *backend, int error)
{
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		s7d_v4l2_error(backend->frontend, error);
	mutex_unlock(&backend->notifications);
}

static int hevc_complete_input(struct hevc_backend *backend, u64 token, int status)
{
	int ret = hevc_notify_input(backend, token, status);

	if (ret && ret != -ESHUTDOWN) {
		mutex_lock(&backend->lock);
		backend->failure = ret;
		mutex_unlock(&backend->lock);
		hevc_notify_error(backend, ret);
	}
	return ret;
}

static void hevc_work(struct work_struct *work)
{
	struct hevc_backend *backend = container_of(work, struct hevc_backend, work);
	struct s7d_hevc_decode_result result;
	u64 token;
	int ret;

	for (;;) {
		mutex_lock(&backend->lock);
		if (backend->closing || backend->failure || backend->capture_blocked)
			goto unlock;
		if (backend->input_pending && !backend->preflight) {
			token = backend->input.token;
			ret = hevc_metadata(backend);
			if (ret)
				goto failed;
			backend->preflight = true;
			mutex_unlock(&backend->lock);
			ret = backend->sequence_valid && !backend->sequence_notified ?
				hevc_notify_sequence(backend) : 0;
			mutex_lock(&backend->lock);
			if (ret)
				goto failed;
			backend->sequence_notified = backend->sequence_valid;
			if (token == backend->initial_token)
				backend->initial_complete = true;
			if (backend->au) {
				mutex_unlock(&backend->lock);
				continue;
			}
			backend->input_pending = false;
			backend->last_completed = token;
			mutex_unlock(&backend->lock);
			if (hevc_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->input_pending && backend->au && backend->captures) {
			struct s7d_v4l2_capture_buffer capture = backend->capture[0];
			u8 *au = backend->au;
			size_t bytes = backend->au_bytes;
			u64 timestamp = backend->input.timestamp_ns;

			token = backend->input.token;
			backend->execution_attempted = true;
			mutex_unlock(&backend->lock);
			ret = s7d_hevc_decoder_decode_surface(backend->adapter->decoder,
				backend->session, au, bytes, capture.surface,
				capture.surface_id, capture.generation, timestamp, &result);
			mutex_lock(&backend->lock);
			if (ret)
				goto failed;
			backend->picture_done = true;
			backend->input_pending = false;
			backend->last_completed = token;
			backend->au = NULL;
			backend->au_bytes = 0;
			mutex_unlock(&backend->lock);
			kvfree(au);
			if (hevc_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->drain_pending && !backend->drain_notified) {
			struct s7d_v4l2_drain drain = backend->drain;

			mutex_unlock(&backend->lock);
			mutex_lock(&backend->notifications);
			ret = backend->frontend && !READ_ONCE(backend->closing) ?
				s7d_v4l2_drain_done(backend->frontend, drain.epoch,
						    drain.last_input_token) : -ESHUTDOWN;
			mutex_unlock(&backend->notifications);
			mutex_lock(&backend->lock);
			if (ret) {
				backend->failure = ret;
				mutex_unlock(&backend->lock);
				hevc_notify_error(backend, ret);
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
		hevc_complete_input(backend, token, ret);
		return;
	}
}

static int hevc_prepare(void *priv, struct s7d_codec_session *session,
			struct s7d_v4l2_context *frontend)
{
	struct hevc_backend *backend = priv;
	struct s7d_hevc_v4l2 *adapter = backend->adapter;
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
		adapter->active = backend;
	}
	mutex_unlock(&backend->lock);
	mutex_unlock(&adapter->clients);
	return ret;
}

static int hevc_submit(void *priv, const struct s7d_v4l2_input *input)
{
	struct hevc_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session) {
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	} else if (!input || !input->memory || !input->bytes ||
		   input->bytes > S7D_HEVC_INPUT_MAX_BYTES || !input->token) {
		ret = -EINVAL;
	} else if (backend->picture_done) {
		ret = -EOPNOTSUPP;
	} else if (backend->input_pending || backend->drain_pending ||
		   input->token <= backend->last_submitted) {
		ret = -EBUSY;
	} else {
		backend->input = *input;
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

static int hevc_sync_initial(void *priv, u64 token)
{
	struct hevc_backend *backend = priv;
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

static int hevc_capture_start(void *priv, const struct s7d_codec_layout *layout,
			      u64 generation,
			      const struct s7d_v4l2_capture_buffer *buffers, unsigned int count)
{
	struct hevc_backend *backend = priv;
	struct s7d_codec_layout checked;
	dma_addr_t address;
	size_t bytes;
	u32 i, j;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!layout || !buffers || !generation || !backend->sequence_valid ||
		 backend->captures || count > S7D_HEVC_MAX_CAPTURE_BUFFERS)
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = s7d_hevc_build_layout(&backend->sps, S7D_CODEC_CAPTURE_NV12M, 0, &checked);
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
			ret = s7d_codec_surface_dma(buffers[i].surface, j, &address, &bytes);
			if (ret)
				goto out;
			if (!IS_ALIGNED(address, S7D_CODEC_STORAGE_ALIGNMENT) ||
			    address > U32_MAX || bytes < checked.size[j]) {
				ret = -ERANGE;
				goto out;
			}
		}
	}
	memcpy(backend->capture, buffers, sizeof(*buffers) * count);
	backend->captures = count;
	backend->generation = generation;
	backend->capture_blocked = false;
	queue_work(system_unbound_wq, &backend->work);
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int hevc_capture_stop_check(struct hevc_backend *backend, u64 generation)
{
	lockdep_assert_held(&backend->lock);
	if (backend->closing || backend->failure || !backend->session)
		return backend->failure ? backend->failure : -ESHUTDOWN;
	if (!generation || !backend->sequence_valid)
		return -ESTALE;
	if (backend->execution_attempted || backend->drain_pending)
		return -EOPNOTSUPP;
	if (backend->captures && backend->generation != generation)
		return -ESTALE;
	return 0;
}

static int hevc_capture_stop(void *priv, u64 generation)
{
	struct hevc_backend *backend = priv;
	int ret;

	mutex_lock(&backend->lock);
	ret = hevc_capture_stop_check(backend, generation);
	if (!ret)
		backend->capture_blocked = true;
	mutex_unlock(&backend->lock);
	if (ret)
		return ret;
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	mutex_unlock(&backend->notifications);
	mutex_lock(&backend->lock);
	ret = hevc_capture_stop_check(backend, generation);
	if (!ret) {
		memset(backend->capture, 0, sizeof(backend->capture));
		backend->captures = 0;
		backend->generation = 0;
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int hevc_capture_queue(void *priv, struct s7d_codec_surface *surface,
			      u64 surface_id, u64 generation)
{
	struct hevc_backend *backend = priv;
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
			ret = 0;
			queue_work(system_unbound_wq, &backend->work);
			break;
		}
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int hevc_drain(void *priv, const struct s7d_v4l2_drain *request)
{
	struct hevc_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure) {
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	} else if (!request || !request->epoch || backend->input_pending ||
		   backend->drain_pending || !backend->picture_done ||
		   request->last_input_token != backend->last_completed) {
		ret = -EBUSY;
	} else {
		backend->drain = *request;
		backend->drain_pending = true;
		queue_work(system_unbound_wq, &backend->work);
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int hevc_resume(void *priv)
{
	return -EOPNOTSUPP;
}

static void hevc_detach(void *priv)
{
	struct hevc_backend *backend = priv;

	mutex_lock(&backend->lock);
	backend->closing = true;
	if (backend->session)
		s7d_hevc_decoder_request_stop(backend->adapter->decoder);
	mutex_unlock(&backend->lock);
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	backend->frontend = NULL;
	mutex_unlock(&backend->notifications);
}

static int hevc_release_epoch(struct hevc_backend *backend)
{
	struct s7d_codec_session *session;
	u32 i;
	int ret;

	lockdep_assert_held(&backend->adapter->clients);
	if (!backend->closing)
		return -EBUSY;
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
	for (i = 0; i < 3; i++) {
		kfree(backend->parameter[i].data);
		backend->parameter[i] = (struct hevc_parameter) { };
	}
	kvfree(backend->au);
	backend->au = NULL;
	backend->au_bytes = 0;
	mutex_unlock(&backend->lock);
	if (session)
		s7d_codec_session_put(session);
	return 0;
}

static int hevc_reset(void *priv)
{
	struct hevc_backend *backend = priv;
	struct s7d_hevc_v4l2 *adapter = backend->adapter;
	int ret;

	mutex_lock(&adapter->clients);
	ret = adapter->closing ? -ESHUTDOWN : hevc_release_epoch(backend);
	if (!ret) {
		mutex_lock(&backend->lock);
		memset(&backend->input, 0, sizeof(backend->input));
		memset(&backend->drain, 0, sizeof(backend->drain));
		memset(&backend->sps, 0, sizeof(backend->sps));
		memset(&backend->layout, 0, sizeof(backend->layout));
		memset(backend->capture, 0, sizeof(backend->capture));
		backend->generation = 0;
		backend->last_submitted = 0;
		backend->last_completed = 0;
		backend->initial_token = 0;
		backend->captures = 0;
		backend->failure = 0;
		backend->input_pending = false;
		backend->preflight = false;
		backend->initial_complete = false;
		backend->sequence_valid = false;
		backend->sequence_notified = false;
		backend->picture_done = false;
		backend->capture_blocked = false;
		backend->execution_attempted = false;
		backend->drain_pending = false;
		backend->drain_notified = false;
		backend->closing = false;
		mutex_unlock(&backend->lock);
	}
	mutex_unlock(&adapter->clients);
	return ret;
}

static const struct s7d_v4l2_backend_ops hevc_ops = {
	.prepare = hevc_prepare,
	.submit = hevc_submit,
	.sync_initial = hevc_sync_initial,
	.capture_start = hevc_capture_start,
	.capture_stop = hevc_capture_stop,
	.capture_queue = hevc_capture_queue,
	.drain = hevc_drain,
	.resume = hevc_resume,
	.detach = hevc_detach,
	.reset = hevc_reset,
};

static int hevc_factory_ready(void *priv)
{
	struct s7d_hevc_v4l2 *adapter = priv;
	int ret;

	mutex_lock(&adapter->clients);
	ret = adapter->closing ? -ESHUTDOWN : 0;
	mutex_unlock(&adapter->clients);
	return ret;
}

static void *hevc_factory_create(void *priv)
{
	struct s7d_hevc_v4l2 *adapter = priv;
	struct hevc_backend *backend;

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
	INIT_WORK(&backend->work, hevc_work);
	list_add_tail(&backend->node, &adapter->instances);
	mutex_unlock(&adapter->clients);
	return backend;
}

static void hevc_factory_destroy(void *priv, void *instance)
{
	struct s7d_hevc_v4l2 *adapter = priv;
	struct hevc_backend *backend = instance;
	int ret;

	mutex_lock(&adapter->clients);
	hevc_detach(backend);
	ret = hevc_release_epoch(backend);
	if (ret) {
		dev_err(adapter->dev, "HEVC instance stop failed: %d; owner retained\n", ret);
		mutex_unlock(&adapter->clients);
		return;
	}
	list_del(&backend->node);
	mutex_unlock(&adapter->clients);
	module_put(adapter->owner);
	kfree(backend);
}

struct s7d_hevc_v4l2 *
s7d_hevc_v4l2_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
		      struct s7d_codec_firmware_owner *borrowed_firmware,
		      struct module *owner)
{
	struct s7d_hevc_v4l2 *adapter;
	int ret;

	if (!dev)
		return ERR_PTR(-EINVAL);
	adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return ERR_PTR(-ENOMEM);
	adapter->dev = get_device(dev);
	adapter->owner = owner;
	adapter->dos = borrowed_dos;
	mutex_init(&adapter->clients);
	INIT_LIST_HEAD(&adapter->instances);
	adapter->decoder = s7d_hevc_decoder_create(dev, borrowed_dos, borrowed_firmware, owner);
	if (IS_ERR(adapter->decoder)) {
		ret = PTR_ERR(adapter->decoder);
		put_device(adapter->dev);
		kfree(adapter);
		return ERR_PTR(ret);
	}
	adapter->factory = (struct s7d_v4l2_backend_factory) {
		.owner = owner,
		.codec = {
			.engine = S7D_CODEC_ENGINE_HEVC,
			.output_format = V4L2_PIX_FMT_HEVC,
			.max_width = 1920, .max_height = 1080,
			.max_input_size = S7D_HEVC_INPUT_MAX_BYTES,
		},
		.ops = &hevc_ops, .ready = hevc_factory_ready,
		.create = hevc_factory_create, .destroy = hevc_factory_destroy,
	};
	return adapter;
}

struct s7d_codec_core *s7d_hevc_v4l2_core(struct s7d_hevc_v4l2 *adapter)
{
	return s7d_hevc_decoder_core(adapter->decoder);
}

const struct s7d_v4l2_backend_factory *
s7d_hevc_v4l2_factory(struct s7d_hevc_v4l2 *adapter)
{
	return &adapter->factory;
}

int s7d_hevc_v4l2_suspend_check(struct s7d_hevc_v4l2 *adapter)
{
	int ret;

	mutex_lock(&adapter->clients);
	ret = adapter->active ? -EBUSY : s7d_codec_dos_suspend_check(adapter->dos);
	mutex_unlock(&adapter->clients);
	return ret;
}

int s7d_hevc_v4l2_shutdown(struct s7d_hevc_v4l2 *adapter)
{
	struct hevc_backend *backend;

	mutex_lock(&adapter->clients);
	adapter->closing = true;
	list_for_each_entry(backend, &adapter->instances, node) {
		mutex_lock(&backend->lock);
		backend->closing = true;
		mutex_unlock(&backend->lock);
	}
	s7d_hevc_decoder_request_stop(adapter->decoder);
	list_for_each_entry(backend, &adapter->instances, node)
		hevc_detach(backend);
	mutex_unlock(&adapter->clients);
	return 0;
}

int s7d_hevc_v4l2_destroy(struct s7d_hevc_v4l2 *adapter)
{
	int ret;

	if (!adapter)
		return -EINVAL;
	mutex_lock(&adapter->clients);
	adapter->closing = true;
	if (!list_empty(&adapter->instances)) {
		mutex_unlock(&adapter->clients);
		return -EBUSY;
	}
	ret = s7d_hevc_decoder_destroy(adapter->decoder);
	mutex_unlock(&adapter->clients);
	if (ret)
		return ret;
	put_device(adapter->dev);
	kfree(adapter);
	return 0;
}
