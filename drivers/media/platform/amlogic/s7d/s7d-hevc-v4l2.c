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
#include "s7d-hevc-input.h"
#include "s7d-hevc-sequence.h"
#include "s7d-hevc-v4l2.h"
#include "s7d-codec-dos.h"
#include "s7d-v4l2-device.h"

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
	struct s7d_hevc_input_bank *bank;
	struct s7d_hevc_input *prepared;
	struct s7d_hevc_input_result view;
	struct s7d_hevc_sps sps;
	struct s7d_codec_layout layout;
	u8 *au;
	size_t au_bytes;
	u64 generation;
	u64 parameter_epoch;
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
	bool decoder_bound;
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
	       a->max_latency_increase_plus1 == b->max_latency_increase_plus1 &&
	       a->separate_colour_plane == b->separate_colour_plane &&
	       a->interlaced_source == b->interlaced_source &&
	       a->field_seq == b->field_seq &&
	       a->colour.primaries == b->colour.primaries &&
	       a->colour.transfer == b->colour.transfer &&
	       a->colour.matrix == b->colour.matrix &&
	       a->colour.flags == b->colour.flags;
}

static void hevc_input_release(struct hevc_backend *backend)
{
	s7d_hevc_input_release(backend->prepared);
	backend->prepared = NULL;
	kvfree(backend->au);
	backend->au = NULL;
	backend->au_bytes = 0;
	memset(&backend->view, 0, sizeof(backend->view));
}

static int hevc_metadata(struct hevc_backend *backend)
{
	struct s7d_hevc_input *prepared = NULL;
	const struct s7d_hevc_input_result *view;
	struct s7d_codec_layout layout = backend->layout;
	const u8 *data = s7d_codec_dma_cpu(backend->input.memory);
	size_t prefix, total;
	u8 *au = NULL;
	int ret;

	lockdep_assert_held(&backend->lock);
	if (!backend->bank || backend->prepared || !data || !backend->input.bytes)
		return -EINVAL;
	ret = s7d_hevc_input_prepare(backend->bank, data, backend->input.bytes, &prepared);
	if (ret)
		return ret;
	view = s7d_hevc_input_view(prepared);
	if (!view) {
		ret = -EPROTO;
		goto failed;
	}
	if (view->sequence) {
		ret = s7d_hevc_build_layout(&view->sequence->sps,
					    S7D_CODEC_CAPTURE_NV12M, 0, &layout);
		if (!ret && backend->sequence_valid &&
		    !hevc_same_sps(&backend->sps, &view->sequence->sps))
			ret = -EOPNOTSUPP;
		if (ret)
			goto failed;
	}
	if (view->kind == S7D_HEVC_INPUT_PICTURE) {
		if (!view->sequence || !view->slice.parameter_epoch) {
			ret = -EBADMSG;
			goto failed;
		}
		if (backend->parameter_epoch &&
		    view->slice.parameter_epoch != backend->parameter_epoch) {
			ret = -EOPNOTSUPP;
			goto failed;
		}
		prefix = s7d_hevc_input_prefix_bytes(prepared);
		if (!prefix || check_add_overflow(prefix, view->bytes, &total) ||
		    total > S7D_HEVC_INPUT_MAX_BYTES) {
			ret = -E2BIG;
			goto failed;
		}
		au = kvmalloc(total, GFP_KERNEL);
		if (!au) {
			ret = -ENOMEM;
			goto failed;
		}
		ret = s7d_hevc_input_copy_prefix(prepared, au, total);
		if (ret)
			goto failed;
		memcpy(au + prefix, view->data, view->bytes);
	} else {
		total = 0;
	}
	ret = s7d_hevc_input_commit(backend->bank, prepared);
	if (ret)
		goto failed;
	if (view->sequence) {
		backend->sps = view->sequence->sps;
		backend->layout = layout;
		backend->sequence_valid = true;
	}
	backend->prepared = prepared;
	backend->view = *view;
	backend->au = au;
	backend->au_bytes = total;
	if (au) {
		backend->view.data = au;
		backend->view.bytes = total;
		backend->parameter_epoch = view->slice.parameter_epoch;
	}
	return 0;
failed:
	kvfree(au);
	s7d_hevc_input_release(prepared);
	return ret;
}

static int hevc_notify_sequence(struct hevc_backend *backend, bool *cancelled)
{
	int ret;

	*cancelled = false;
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing)) {
		ret = s7d_v4l2_sequence_result(backend->frontend, &backend->layout, cancelled);
	} else {
		*cancelled = true;
		ret = -ECANCELED;
	}
	mutex_unlock(&backend->notifications);
	return ret;
}

static int hevc_notify_input(struct hevc_backend *backend, u64 token, int status,
			     bool *cancelled)
{
	int ret;

	*cancelled = false;
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing)) {
		if (status)
			s7d_v4l2_error(backend->frontend, status);
		ret = s7d_v4l2_input_done_result(backend->frontend, token, status, cancelled);
	} else {
		*cancelled = true;
		ret = -ECANCELED;
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
	bool cancelled;
	int ret = hevc_notify_input(backend, token, status, &cancelled);

	if (ret && !cancelled) {
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
	u64 token = 0;
	bool cancelled;
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
			cancelled = false;
			ret = backend->sequence_valid && !backend->sequence_notified ?
				hevc_notify_sequence(backend, &cancelled) : 0;
			mutex_lock(&backend->lock);
			if (ret) {
				if (backend->closing && cancelled)
					goto unlock;
				goto failed;
			}
			backend->sequence_notified = backend->sequence_valid;
			if (token == backend->initial_token)
				backend->initial_complete = true;
			if (backend->au) {
				mutex_unlock(&backend->lock);
				continue;
			}
			backend->input_pending = false;
			backend->last_completed = token;
			hevc_input_release(backend);
			mutex_unlock(&backend->lock);
			if (hevc_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->input_pending && backend->au && backend->captures) {
			bool bound = backend->decoder_bound;

			token = backend->input.token;

			backend->execution_attempted = true;
			mutex_unlock(&backend->lock);
			if (!bound) {
				ret = s7d_hevc_decoder_bind(backend->adapter->decoder, backend->session,
					&backend->sps, &backend->layout, backend->generation,
					backend->parameter_epoch, backend->capture, backend->captures);
				mutex_lock(&backend->lock);
				if (!ret)
					backend->decoder_bound = true;
				mutex_unlock(&backend->lock);
			} else {
				ret = 0;
			}
			result = (struct s7d_hevc_decode_result) { };
			if (!ret)
				ret = s7d_hevc_decoder_decode(backend->adapter->decoder,
					backend->session, &backend->view, token,
					backend->input.timestamp_ns, &backend->input.timestamp_meta, &result);
			mutex_lock(&backend->lock);
			if (ret) {
				if (result.cancelled && backend->closing)
					goto unlock;
				goto failed;
			}
			backend->input_pending = false;
			backend->last_completed = token;
			hevc_input_release(backend);
			mutex_unlock(&backend->lock);
			if (hevc_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->drain_pending && !backend->drain_notified) {
			struct s7d_v4l2_drain drain = backend->drain;
			u32 published = 0;

			if (backend->input_pending || drain.last_input_token != backend->last_completed)
				goto unlock;
			mutex_unlock(&backend->lock);
			cancelled = false;
			ret = backend->decoder_bound ?
				s7d_hevc_decoder_drain(backend->adapter->decoder,
						backend->session, &published, &cancelled) : 0;
			if (!ret) {
				mutex_lock(&backend->notifications);
				cancelled = false;
				if (backend->frontend && !READ_ONCE(backend->closing))
					ret = s7d_v4l2_drain_done_result(backend->frontend,
						drain.epoch, drain.last_input_token, &cancelled);
				else
					cancelled = true;
				mutex_unlock(&backend->notifications);
			}
			mutex_lock(&backend->lock);
			if (cancelled && backend->closing)
				goto unlock;
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
		hevc_input_release(backend);
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
			ret = backend->decoder_bound ?
				s7d_hevc_decoder_capture_queue(backend->adapter->decoder,
							surface, surface_id, generation) : 0;
			if (!ret)
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
		   backend->drain_pending ||
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
	struct hevc_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || backend->input_pending ||
	    !backend->drain_notified) {
		ret = backend->failure ? backend->failure : -EBUSY;
	} else {
		memset(&backend->drain, 0, sizeof(backend->drain));
		backend->drain_pending = false;
		backend->drain_notified = false;
	}
	mutex_unlock(&backend->lock);
	return ret;
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
	int ret;

	lockdep_assert_held(&backend->adapter->clients);
	if (!backend->closing)
		return -EBUSY;
	if (backend->session) {
		ret = s7d_codec_session_stop(backend->session);
		if (!ret)
			ret = s7d_hevc_decoder_stream_close(backend->adapter->decoder,
							backend->session);
		if (ret)
			return ret;
	}
	mutex_lock(&backend->lock);
	if (backend->adapter->active == backend)
		backend->adapter->active = NULL;
	session = backend->session;
	backend->session = NULL;
	hevc_input_release(backend);
	backend->decoder_bound = false;
	mutex_unlock(&backend->lock);
	if (session)
		s7d_codec_session_put(session);
	return 0;
}

static int hevc_reset(void *priv)
{
	struct hevc_backend *backend = priv;
	struct s7d_hevc_v4l2 *adapter = backend->adapter;
	struct s7d_hevc_input_bank *bank;
	int ret;

	bank = s7d_hevc_input_bank_create();
	if (IS_ERR(bank))
		return PTR_ERR(bank);
	mutex_lock(&adapter->clients);
	ret = adapter->closing ? -ESHUTDOWN : hevc_release_epoch(backend);
	if (!ret) {
		mutex_lock(&backend->lock);
		memset(&backend->input, 0, sizeof(backend->input));
		memset(&backend->drain, 0, sizeof(backend->drain));
		memset(&backend->sps, 0, sizeof(backend->sps));
		memset(&backend->layout, 0, sizeof(backend->layout));
		memset(backend->capture, 0, sizeof(backend->capture));
		s7d_hevc_input_bank_destroy(backend->bank);
		backend->bank = bank;
		bank = NULL;
		backend->generation = 0;
		backend->parameter_epoch = 0;
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
		backend->decoder_bound = false;
		backend->capture_blocked = false;
		backend->execution_attempted = false;
		backend->drain_pending = false;
		backend->drain_notified = false;
		backend->closing = false;
		mutex_unlock(&backend->lock);
	}
	mutex_unlock(&adapter->clients);
	s7d_hevc_input_bank_destroy(bank);
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
	backend->bank = s7d_hevc_input_bank_create();
	if (IS_ERR(backend->bank)) {
		int ret = PTR_ERR(backend->bank);

		kfree(backend);
		return ERR_PTR(ret);
	}
	mutex_lock(&adapter->clients);
	if (adapter->closing || !try_module_get(adapter->owner)) {
		mutex_unlock(&adapter->clients);
		s7d_hevc_input_bank_destroy(backend->bank);
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
	s7d_hevc_input_bank_destroy(backend->bank);
	module_put(adapter->owner);
	kfree(backend);
}

struct s7d_hevc_v4l2 *
s7d_hevc_v4l2_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
		      struct s7d_codec_firmware_owner *borrowed_firmware,
		      struct s7d_hevc_engine *borrowed_engine, struct module *owner)
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
	adapter->decoder = s7d_hevc_decoder_create(dev, borrowed_dos, borrowed_firmware,
						 borrowed_engine, owner);
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
