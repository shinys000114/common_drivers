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

#include "s7d-vp9-backend.h"
#include "s7d-vp9-input.h"
#include "s7d-vp9-prefix.h"
#include "s7d-vp9-v4l2.h"
#include "../common/s7d-codec-dos.h"
#include "../common/s7d-v4l2-device.h"

struct vp9_backend {
	struct s7d_vp9_v4l2 *adapter;
	struct list_head node;
	struct mutex lock; /* Admission, metadata and capture bindings. */
	struct mutex notifications; /* Frontend lifetime and callbacks. */
	struct work_struct work;
	struct s7d_codec_session *session;
	struct s7d_v4l2_context *frontend;
	struct s7d_v4l2_input input;
	struct s7d_vp9_packet packet;
	struct s7d_v4l2_drain drain;
	struct s7d_v4l2_capture_buffer capture[S7D_VP9_MAX_PHYSICAL];
	struct s7d_vp9_input_bank *bank;
	struct s7d_vp9_input *prepared;
	struct s7d_vp9_input_result view;
	struct s7d_codec_layout layout;
	u64 generation;
	u64 last_submitted;
	u64 last_completed;
	u64 initial_token;
	u64 picture_serial;
	u32 next_frame;
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

struct s7d_vp9_v4l2 {
	struct device *dev;
	struct module *owner;
	struct s7d_vp9_decoder *decoder;
	struct s7d_codec_dos *dos;
	struct s7d_v4l2_backend_factory factory;
	struct mutex clients; /* Instance lifetime and shutdown. */
	struct list_head instances;
	struct vp9_backend *active;
	bool closing;
};

static void vp9_input_release(struct vp9_backend *backend)
{
	s7d_vp9_input_release(backend->prepared);
	backend->prepared = NULL;
	memset(&backend->view, 0, sizeof(backend->view));
}

static int vp9_metadata(struct vp9_backend *backend)
{
	struct s7d_vp9_input *prepared = NULL;
	const struct s7d_vp9_input_result *view;
	const struct s7d_vp9_frame_span *span;
	const u8 *data = s7d_codec_dma_cpu(backend->input.memory);
	int ret;

	lockdep_assert_held(&backend->lock);
	if (!backend->bank || backend->prepared || !data || !backend->input.bytes)
		return -EINVAL;
	if (!backend->packet.count) {
		ret = s7d_vp9_split_packet(data, backend->input.bytes, &backend->packet);
		if (ret)
			return ret;
	}
	if (backend->next_frame >= backend->packet.count)
		return -ESTALE;
	span = &backend->packet.frame[backend->next_frame];
	ret = s7d_vp9_input_prepare_frame(backend->bank, data + span->offset,
					span->size, &prepared);
	if (ret)
		return ret;
	view = s7d_vp9_input_view(prepared);
	backend->layout = view->layout;
	backend->sequence_valid = true;
	backend->prepared = prepared;
	backend->view = *view;
	return 0;
}

static int vp9_notify_sequence(struct vp9_backend *backend, bool *cancelled)
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

static int vp9_notify_input(struct vp9_backend *backend, u64 token, int status,
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

static void vp9_notify_error(struct vp9_backend *backend, int error)
{
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		s7d_v4l2_error(backend->frontend, error);
	mutex_unlock(&backend->notifications);
}

static int vp9_complete_input(struct vp9_backend *backend, u64 token, int status)
{
	bool cancelled;
	int ret = vp9_notify_input(backend, token, status, &cancelled);

	if (ret && !cancelled) {
		mutex_lock(&backend->lock);
		backend->failure = ret;
		mutex_unlock(&backend->lock);
		vp9_notify_error(backend, ret);
	}
	return ret;
}

static void vp9_work(struct work_struct *work)
{
	struct vp9_backend *backend = container_of(work, struct vp9_backend, work);
	struct s7d_vp9_decode_result result;
	u64 token = 0, picture_token;
	bool cancelled;
	int ret;

	for (;;) {
		mutex_lock(&backend->lock);
		if (backend->closing || backend->failure || backend->capture_blocked)
			goto unlock;
		if (backend->input_pending && !backend->preflight) {
			token = backend->input.token;
			ret = vp9_metadata(backend);
			if (ret)
				goto failed;
			backend->preflight = true;
			mutex_unlock(&backend->lock);
			cancelled = false;
			ret = backend->sequence_valid && !backend->sequence_notified ?
				vp9_notify_sequence(backend, &cancelled) : 0;
			mutex_lock(&backend->lock);
			if (ret) {
				if (backend->closing && cancelled)
					goto unlock;
				goto failed;
			}
			backend->sequence_notified = backend->sequence_valid;
			if (token == backend->initial_token)
				backend->initial_complete = true;
			mutex_unlock(&backend->lock);
			continue;
		}

		if (backend->input_pending && backend->prepared && backend->captures) {
			bool bound = backend->decoder_bound;

			token = backend->input.token;
			if (backend->picture_serial == U64_MAX) {
				ret = -EOVERFLOW;
				goto failed;
			}
			picture_token = ++backend->picture_serial;
			backend->execution_attempted = true;
			mutex_unlock(&backend->lock);
			if (!bound) {
				ret = s7d_vp9_decoder_bind(backend->adapter->decoder, backend->session,
					&backend->layout, backend->generation,
					backend->capture, backend->captures);
				mutex_lock(&backend->lock);
				if (!ret)
					backend->decoder_bound = true;
				mutex_unlock(&backend->lock);
			} else {
				ret = 0;
			}
			result = (struct s7d_vp9_decode_result) { };
			if (!ret)
				ret = s7d_vp9_decoder_decode(backend->adapter->decoder,
					backend->session, backend->bank, backend->prepared,
					picture_token, backend->input.timestamp_ns,
					&backend->input.timestamp_meta, &result);
			mutex_lock(&backend->lock);
			if (ret) {
				if (result.cancelled && backend->closing)
					goto unlock;
				goto failed;
			}
			vp9_input_release(backend);
			backend->preflight = false;
			backend->next_frame++;
			if (backend->next_frame < backend->packet.count) {
				mutex_unlock(&backend->lock);
				continue;
			}
			backend->input_pending = false;
			backend->last_completed = token;
			memset(&backend->packet, 0, sizeof(backend->packet));
			backend->next_frame = 0;
			mutex_unlock(&backend->lock);
			if (vp9_complete_input(backend, token, 0))
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
				s7d_vp9_decoder_drain(backend->adapter->decoder,
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
				vp9_notify_error(backend, ret);
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
		vp9_input_release(backend);
		mutex_unlock(&backend->lock);
		vp9_complete_input(backend, token, ret);
		return;
	}
}


static int vp9_prepare(void *priv, struct s7d_codec_session *session,
			struct s7d_v4l2_context *frontend)
{
	struct vp9_backend *backend = priv;
	struct s7d_vp9_v4l2 *adapter = backend->adapter;
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

static int vp9_submit(void *priv, const struct s7d_v4l2_input *input)
{
	struct vp9_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session) {
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	} else if (!input || !input->memory || !input->bytes ||
		   input->bytes > S7D_VP9_INPUT_MAX_BYTES || !input->token) {
		ret = -EINVAL;
	} else if (backend->input_pending || backend->drain_pending ||
		   input->token <= backend->last_submitted) {
		ret = -EBUSY;
	} else {
		backend->input = *input;
		memset(&backend->packet, 0, sizeof(backend->packet));
		backend->next_frame = 0;
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

static int vp9_sync_initial(void *priv, u64 token)
{
	struct vp9_backend *backend = priv;
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

static int vp9_capture_start(void *priv, const struct s7d_codec_layout *layout,
			      u64 generation,
			      const struct s7d_v4l2_capture_buffer *buffers, unsigned int count)
{
	struct vp9_backend *backend = priv;
	struct s7d_codec_layout checked;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!layout || !buffers || !generation || !backend->sequence_valid ||
		 backend->captures || count > S7D_VP9_MAX_PHYSICAL)
		ret = -EINVAL;
	if (ret)
		goto out;
	checked = backend->layout;
	ret = s7d_codec_validate_layout(&checked);
	if (ret)
		goto out;
	if (memcmp(&checked, layout, sizeof(checked)) || count < checked.min_capture_buffers) {
		ret = -EINVAL;
		goto out;
	}
	ret = s7d_v4l2_validate_capture_pool(&checked, generation, buffers, count);
	if (ret)
		goto out;
	memcpy(backend->capture, buffers, sizeof(*buffers) * count);
	backend->captures = count;
	backend->generation = generation;
	backend->capture_blocked = false;
	queue_work(system_unbound_wq, &backend->work);
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int vp9_capture_stop_check(struct vp9_backend *backend, u64 generation)
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

static int vp9_capture_stop(void *priv, u64 generation)
{
	struct vp9_backend *backend = priv;
	int ret;

	mutex_lock(&backend->lock);
	ret = vp9_capture_stop_check(backend, generation);
	if (!ret)
		backend->capture_blocked = true;
	mutex_unlock(&backend->lock);
	if (ret)
		return ret;
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	mutex_unlock(&backend->notifications);
	mutex_lock(&backend->lock);
	ret = vp9_capture_stop_check(backend, generation);
	if (!ret) {
		memset(backend->capture, 0, sizeof(backend->capture));
		backend->captures = 0;
		backend->generation = 0;
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int vp9_capture_queue(void *priv, struct s7d_codec_surface *surface,
			      u64 surface_id, u64 generation)
{
	struct vp9_backend *backend = priv;
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
				s7d_vp9_decoder_capture_queue(backend->adapter->decoder,
							surface, surface_id, generation) : 0;
			if (!ret)
				queue_work(system_unbound_wq, &backend->work);
			break;
		}
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int vp9_drain(void *priv, const struct s7d_v4l2_drain *request)
{
	struct vp9_backend *backend = priv;
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

static int vp9_resume(void *priv)
{
	struct vp9_backend *backend = priv;
	int ret = 0;

	flush_work(&backend->work);
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

static void vp9_detach(void *priv)
{
	struct vp9_backend *backend = priv;

	mutex_lock(&backend->lock);
	backend->closing = true;
	if (backend->session)
		s7d_vp9_decoder_request_stop(backend->adapter->decoder);
	mutex_unlock(&backend->lock);
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	backend->frontend = NULL;
	mutex_unlock(&backend->notifications);
}

static int vp9_release_epoch(struct vp9_backend *backend)
{
	struct s7d_codec_session *session;
	int ret;

	lockdep_assert_held(&backend->adapter->clients);
	if (!backend->closing)
		return -EBUSY;
	if (backend->session) {
		ret = s7d_codec_session_stop(backend->session);
		if (!ret)
			ret = s7d_vp9_decoder_stream_close(backend->adapter->decoder,
							backend->session);
		if (ret)
			return ret;
	}
	mutex_lock(&backend->lock);
	if (backend->adapter->active == backend)
		backend->adapter->active = NULL;
	session = backend->session;
	backend->session = NULL;
	vp9_input_release(backend);
	backend->decoder_bound = false;
	mutex_unlock(&backend->lock);
	if (session)
		s7d_codec_session_put(session);
	return 0;
}

static int vp9_reset(void *priv)
{
	struct vp9_backend *backend = priv;
	struct s7d_vp9_v4l2 *adapter = backend->adapter;
	struct s7d_vp9_input_bank *bank;
	int ret;

	bank = s7d_vp9_input_bank_create();
	if (IS_ERR(bank))
		return PTR_ERR(bank);
	mutex_lock(&adapter->clients);
	ret = adapter->closing ? -ESHUTDOWN : vp9_release_epoch(backend);
	if (!ret) {
		mutex_lock(&backend->lock);
		memset(&backend->input, 0, sizeof(backend->input));
		memset(&backend->packet, 0, sizeof(backend->packet));
		memset(&backend->drain, 0, sizeof(backend->drain));
		memset(&backend->layout, 0, sizeof(backend->layout));
		memset(backend->capture, 0, sizeof(backend->capture));
		s7d_vp9_input_bank_destroy(backend->bank);
		backend->bank = bank;
		bank = NULL;
		backend->generation = 0;
		backend->last_submitted = 0;
		backend->last_completed = 0;
		backend->initial_token = 0;
		backend->picture_serial = 0;
		backend->next_frame = 0;
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
	s7d_vp9_input_bank_destroy(bank);
	return ret;
}

static const struct s7d_v4l2_backend_ops vp9_ops = {
	.prepare = vp9_prepare,
	.submit = vp9_submit,
	.sync_initial = vp9_sync_initial,
	.capture_start = vp9_capture_start,
	.capture_stop = vp9_capture_stop,
	.capture_queue = vp9_capture_queue,
	.drain = vp9_drain,
	.resume = vp9_resume,
	.detach = vp9_detach,
	.reset = vp9_reset,
};

static int vp9_factory_ready(void *priv)
{
	struct s7d_vp9_v4l2 *adapter = priv;
	int ret;

	mutex_lock(&adapter->clients);
	ret = adapter->closing ? -ESHUTDOWN : 0;
	mutex_unlock(&adapter->clients);
	return ret;
}

static void *vp9_factory_create(void *priv)
{
	struct s7d_vp9_v4l2 *adapter = priv;
	struct vp9_backend *backend;

	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return ERR_PTR(-ENOMEM);
	backend->bank = s7d_vp9_input_bank_create();
	if (IS_ERR(backend->bank)) {
		int ret = PTR_ERR(backend->bank);

		kfree(backend);
		return ERR_PTR(ret);
	}
	mutex_lock(&adapter->clients);
	if (adapter->closing || !try_module_get(adapter->owner)) {
		mutex_unlock(&adapter->clients);
		s7d_vp9_input_bank_destroy(backend->bank);
		kfree(backend);
		return ERR_PTR(-ESHUTDOWN);
	}
	backend->adapter = adapter;
	mutex_init(&backend->lock);
	mutex_init(&backend->notifications);
	INIT_WORK(&backend->work, vp9_work);
	list_add_tail(&backend->node, &adapter->instances);
	mutex_unlock(&adapter->clients);
	return backend;
}

static void vp9_factory_destroy(void *priv, void *instance)
{
	struct s7d_vp9_v4l2 *adapter = priv;
	struct vp9_backend *backend = instance;
	int ret;

	mutex_lock(&adapter->clients);
	vp9_detach(backend);
	ret = vp9_release_epoch(backend);
	if (ret) {
		dev_err(adapter->dev, "VP9 instance stop failed: %d; owner retained\n", ret);
		mutex_unlock(&adapter->clients);
		return;
	}
	list_del(&backend->node);
	mutex_unlock(&adapter->clients);
	s7d_vp9_input_bank_destroy(backend->bank);
	module_put(adapter->owner);
	kfree(backend);
}

struct s7d_vp9_v4l2 *
s7d_vp9_v4l2_create(struct device *dev, struct s7d_codec_dos *borrowed_dos,
		      struct s7d_codec_firmware_owner *borrowed_firmware,
		      struct s7d_hevc_engine *borrowed_engine, struct module *owner)
{
	struct s7d_vp9_v4l2 *adapter;
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
	adapter->decoder = s7d_vp9_decoder_create(dev, borrowed_dos, borrowed_firmware,
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
			.output_format = V4L2_PIX_FMT_VP9,
			.max_width = 1920, .max_height = 1080,
			.max_input_size = S7D_VP9_INPUT_MAX_BYTES,
			.staging_headroom = SZ_64K,
			.default_layout = s7d_v4l2_default_nv12_layout,
			.max_capture_buffers = S7D_VP9_MAX_PHYSICAL,
		},
		.ops = &vp9_ops, .ready = vp9_factory_ready,
		.create = vp9_factory_create, .destroy = vp9_factory_destroy,
	};
	return adapter;
}

struct s7d_codec_core *s7d_vp9_v4l2_core(struct s7d_vp9_v4l2 *adapter)
{
	return s7d_vp9_decoder_core(adapter->decoder);
}

const struct s7d_v4l2_backend_factory *
s7d_vp9_v4l2_factory(struct s7d_vp9_v4l2 *adapter)
{
	return &adapter->factory;
}

int s7d_vp9_v4l2_suspend_check(struct s7d_vp9_v4l2 *adapter)
{
	int ret;

	mutex_lock(&adapter->clients);
	ret = adapter->active ? -EBUSY : s7d_codec_dos_suspend_check(adapter->dos);
	mutex_unlock(&adapter->clients);
	return ret;
}

int s7d_vp9_v4l2_shutdown(struct s7d_vp9_v4l2 *adapter)
{
	struct vp9_backend *backend;

	mutex_lock(&adapter->clients);
	adapter->closing = true;
	list_for_each_entry(backend, &adapter->instances, node) {
		mutex_lock(&backend->lock);
		backend->closing = true;
		mutex_unlock(&backend->lock);
	}
	s7d_vp9_decoder_request_stop(adapter->decoder);
	list_for_each_entry(backend, &adapter->instances, node)
		vp9_detach(backend);
	mutex_unlock(&adapter->clients);
	return 0;
}

int s7d_vp9_v4l2_destroy(struct s7d_vp9_v4l2 *adapter)
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
	ret = s7d_vp9_decoder_destroy(adapter->decoder);
	mutex_unlock(&adapter->clients);
	if (ret)
		return ret;
	put_device(adapter->dev);
	kfree(adapter);
	return 0;
}
