// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "s7d-codec-session.h"

enum s7d_session_state {
	S7D_SESSION_OPEN,
	S7D_SESSION_STOPPING,
	S7D_SESSION_STOPPED,
	S7D_SESSION_QUARANTINED,
};

struct s7d_codec_core {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_resource_ops ops;
	void *priv;
	u64 dma_mask;
	struct mutex lock;
	struct list_head sessions;
	struct s7d_codec_session *active;
	bool failed;
};

struct s7d_codec_session {
	struct kref ref;
	struct s7d_codec_core *core;
	struct list_head node;
	struct list_head surfaces;
	struct list_head allocations;
	enum s7d_codec_engine engine;
	enum s7d_session_state state;
	struct s7d_codec_callbacks callbacks;
	void *callback_priv;
	u64 retired_generation;
};

struct s7d_codec_plane {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t dma;
	size_t bytes;
};

struct s7d_codec_surface {
	struct s7d_codec_session *session;
	struct list_head node;
	struct s7d_codec_surface_request request;
	struct s7d_codec_plane plane[S7D_CODEC_MAX_PLANES];
	u64 write_token;
	u32 dpb_refs;
	bool queued;
	bool writing;
	bool written;
	bool displayed;
	bool user_owned;
};

struct s7d_codec_dma {
	struct list_head node;
	void *cpu;
	dma_addr_t dma;
	size_t bytes;
};

static bool s7d_dma_range(struct s7d_codec_core *core, dma_addr_t dma,
			 size_t bytes)
{
	u64 last;

	return bytes && !check_add_overflow((u64)dma, (u64)bytes - 1, &last) &&
	       last <= core->dma_mask;
}

static void s7d_plane_unpin(struct s7d_codec_plane *plane)
{
	if (plane->sgt)
		dma_buf_unmap_attachment_unlocked(plane->attach, plane->sgt,
						 DMA_BIDIRECTIONAL);
	if (plane->attach)
		dma_buf_detach(plane->dmabuf, plane->attach);
	if (plane->dmabuf)
		dma_buf_put(plane->dmabuf);
}

static void s7d_surface_unpin(struct s7d_codec_surface *surface)
{
	unsigned int i;

	for (i = surface->request.num_planes; i; i--)
		s7d_plane_unpin(&surface->plane[i - 1]);
	kfree(surface);
}

static void s7d_session_release(struct kref *ref)
{
	struct s7d_codec_session *session =
		container_of(ref, struct s7d_codec_session, ref);
	struct s7d_codec_core *core = session->core;
	struct module *owner = core->owner;
	struct s7d_codec_surface *surface, *surface_next;
	struct s7d_codec_dma *mem, *mem_next;

	mutex_lock(&core->lock);
	list_for_each_entry_safe(surface, surface_next, &session->surfaces, node) {
		list_del(&surface->node);
		s7d_surface_unpin(surface);
	}
	list_for_each_entry_safe(mem, mem_next, &session->allocations, node) {
		list_del(&mem->node);
		dma_free_coherent(core->dev, mem->bytes, mem->cpu, mem->dma);
		kfree(mem);
	}
	core->ops.release(core->priv, session->engine);
	if (core->active == session)
		core->active = NULL;
	list_del(&session->node);
	mutex_unlock(&core->lock);
	kfree(session);
	module_put(owner);
}

struct s7d_codec_core *
s7d_codec_core_create(struct device *dev, struct module *owner,
		     const struct s7d_codec_resource_ops *ops, void *priv,
		     unsigned int dma_bits)
{
	struct s7d_codec_core *core;

	if (!dev || !dev->dma_mask || !ops || !ops->hold || !ops->quiesce ||
	    !ops->release || !dma_bits || dma_bits > 64)
		return ERR_PTR(-EINVAL);
#ifdef MODULE
	if (owner != THIS_MODULE)
		return ERR_PTR(-EINVAL);
#endif
	core = kzalloc(sizeof(*core), GFP_KERNEL);
	if (!core)
		return ERR_PTR(-ENOMEM);
	core->dev = get_device(dev);
	core->owner = owner;
	core->ops = *ops;
	core->priv = priv;
	core->dma_mask = DMA_BIT_MASK(dma_bits);
	mutex_init(&core->lock);
	INIT_LIST_HEAD(&core->sessions);
	return core;
}

int s7d_codec_core_destroy(struct s7d_codec_core *core)
{
	mutex_lock(&core->lock);
	if (!list_empty(&core->sessions) || core->failed) {
		mutex_unlock(&core->lock);
		return -EBUSY;
	}
	mutex_unlock(&core->lock);
	put_device(core->dev);
	kfree(core);
	return 0;
}

int s7d_codec_session_open(struct s7d_codec_core *core,
			   enum s7d_codec_engine engine,
			   const struct s7d_codec_callbacks *callbacks, void *priv,
			   struct s7d_codec_session **out)
{
	struct s7d_codec_session *session;
	int ret;

	if (!out || !callbacks || !callbacks->frame_ready ||
	    engine < S7D_CODEC_ENGINE_VDEC || engine > S7D_CODEC_ENGINE_HCODEC)
		return -EINVAL;
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;
	mutex_lock(&core->lock);
	if (core->failed) {
		ret = -EIO;
		goto free;
	}
	if (core->active) {
		ret = -EBUSY;
		goto free;
	}
	if (!try_module_get(core->owner)) {
		ret = -ENODEV;
		goto free;
	}
	ret = core->ops.hold(core->priv, engine);
	if (ret) {
		module_put(core->owner);
		ret = ret < 0 ? ret : -EIO;
		goto free;
	}
	kref_init(&session->ref);
	kref_get(&session->ref);
	session->core = core;
	session->engine = engine;
	session->state = S7D_SESSION_OPEN;
	session->callbacks = *callbacks;
	session->callback_priv = priv;
	INIT_LIST_HEAD(&session->surfaces);
	INIT_LIST_HEAD(&session->allocations);
	list_add_tail(&session->node, &core->sessions);
	core->active = session;
	*out = session;
	mutex_unlock(&core->lock);
	return 0;
free:
	mutex_unlock(&core->lock);
	kfree(session);
	return ret;
}

void s7d_codec_session_get(struct s7d_codec_session *session)
{
	kref_get(&session->ref);
}

void s7d_codec_session_put(struct s7d_codec_session *session)
{
	kref_put(&session->ref, s7d_session_release);
}

int s7d_codec_session_retire_generation(struct s7d_codec_session *session,
				       u64 generation)
{
	struct s7d_codec_surface *surface, *next;
	struct s7d_codec_core *core;
	bool found = false, earlier = false;
	int ret = 0;

	if (!session || !generation)
		return -EINVAL;
	core = session->core;
	mutex_lock(&core->lock);
	if (session->state != S7D_SESSION_OPEN) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (generation <= session->retired_generation) {
		ret = -ESTALE;
		goto unlock;
	}
	list_for_each_entry(surface, &session->surfaces, node) {
		if (surface->request.generation < generation)
			earlier = true;
		if (surface->request.generation != generation)
			continue;
		found = true;
		if (surface->writing || surface->queued || surface->dpb_refs ||
		    !surface->user_owned) {
			ret = -EBUSY;
			goto unlock;
		}
	}
	if (!found) {
		ret = -ENOENT;
		goto unlock;
	}
	if (earlier) {
		ret = -EBUSY;
		goto unlock;
	}
	list_for_each_entry_safe(surface, next, &session->surfaces, node) {
		if (surface->request.generation != generation)
			continue;
		list_del(&surface->node);
		s7d_surface_unpin(surface);
	}
	session->retired_generation = generation;
unlock:
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_session_stop(struct s7d_codec_session *session)
{
	struct s7d_codec_core *core = session->core;
	int ret;

	mutex_lock(&core->lock);
	if (session->state != S7D_SESSION_OPEN) {
		ret = session->state == S7D_SESSION_STOPPED ? 0 :
		      session->state == S7D_SESSION_STOPPING ? -EINPROGRESS : -EIO;
		mutex_unlock(&core->lock);
		return ret;
	}
	session->state = S7D_SESSION_STOPPING;
	session->callbacks.frame_ready = NULL;
	session->callback_priv = NULL;
	mutex_unlock(&core->lock);

	ret = core->ops.quiesce(core->priv, session->engine);
	ret = ret > 0 ? -EIO : ret;
	mutex_lock(&core->lock);
	if (ret) {
		core->failed = true;
		session->state = S7D_SESSION_QUARANTINED;
	} else {
		session->state = S7D_SESSION_STOPPED;
	}
	mutex_unlock(&core->lock);
	if (!ret)
		s7d_codec_session_put(session);
	return ret;
}

struct s7d_codec_dma *
s7d_codec_dma_alloc(struct s7d_codec_session *session, size_t bytes)
{
	struct s7d_codec_core *core = session->core;
	struct s7d_codec_dma *mem;
	int ret = -ENOMEM;

	if (!bytes)
		return ERR_PTR(-EINVAL);
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&core->lock);
	if (session->state != S7D_SESSION_OPEN) {
		ret = -ESHUTDOWN;
		goto free;
	}
	mem->cpu = dma_alloc_coherent(core->dev, bytes, &mem->dma, GFP_KERNEL);
	if (!mem->cpu)
		goto free;
	if (!s7d_dma_range(core, mem->dma, bytes)) {
		dma_free_coherent(core->dev, bytes, mem->cpu, mem->dma);
		ret = -ERANGE;
		goto free;
	}
	mem->bytes = bytes;
	list_add_tail(&mem->node, &session->allocations);
	mutex_unlock(&core->lock);
	return mem;
free:
	mutex_unlock(&core->lock);
	kfree(mem);
	return ERR_PTR(ret);
}

void *s7d_codec_dma_cpu(struct s7d_codec_dma *mem)
{
	return mem->cpu;
}

dma_addr_t s7d_codec_dma_addr(struct s7d_codec_dma *mem)
{
	return mem->dma;
}

size_t s7d_codec_dma_size(struct s7d_codec_dma *mem)
{
	return mem->bytes;
}

static int s7d_plane_pin(struct s7d_codec_core *core, struct dma_buf *dmabuf,
			 struct s7d_codec_plane *plane, size_t minimum,
			 u32 alignment)
{
	struct scatterlist *sg;
	dma_addr_t expected;
	size_t contiguous = 0;
	unsigned int i;
	int ret;

	get_dma_buf(dmabuf);
	plane->dmabuf = dmabuf;
	plane->attach = dma_buf_attach(dmabuf, core->dev);
	if (IS_ERR(plane->attach)) {
		ret = PTR_ERR(plane->attach);
		plane->attach = NULL;
		return ret;
	}
	plane->sgt = dma_buf_map_attachment_unlocked(plane->attach,
						   DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(plane->sgt)) {
		ret = plane->sgt ? PTR_ERR(plane->sgt) : -ENOMEM;
		plane->sgt = NULL;
		return ret;
	}
	if (!plane->sgt->nents)
		return -EINVAL;
	plane->dma = sg_dma_address(plane->sgt->sgl);
	expected = plane->dma;
	for_each_sgtable_dma_sg(plane->sgt, sg, i) {
		if (sg_dma_address(sg) != expected)
			break;
		if (check_add_overflow(contiguous, (size_t)sg_dma_len(sg),
				       &contiguous) ||
		    check_add_overflow(expected, (dma_addr_t)sg_dma_len(sg),
				       &expected))
			return -EOVERFLOW;
	}
	if (contiguous < minimum || !IS_ALIGNED(plane->dma, alignment) ||
	    !s7d_dma_range(core, plane->dma, minimum))
		return -ERANGE;
	plane->bytes = minimum;
	return 0;
}

static bool s7d_planes_alias(const struct s7d_codec_plane *a,
			     const struct s7d_codec_plane *b)
{
	u64 last_a = (u64)a->dma + a->bytes - 1;
	u64 last_b = (u64)b->dma + b->bytes - 1;

	if (a->dmabuf == b->dmabuf ||
	    (a->dmabuf->ops == b->dmabuf->ops && a->dmabuf->priv &&
	     a->dmabuf->priv == b->dmabuf->priv))
		return true;
	return (u64)a->dma <= last_b && (u64)b->dma <= last_a;
}

int s7d_codec_surface_pin(struct s7d_codec_session *session,
			  struct dma_buf *const dmabuf[],
			  const struct s7d_codec_surface_request *request,
			  struct s7d_codec_surface **out)
{
	struct s7d_codec_core *core = session->core;
	struct s7d_codec_surface *surface, *existing;
	unsigned int i, j;
	int ret;

	if (!out || !dmabuf || !request || !request->generation ||
	    !request->num_planes || request->num_planes > S7D_CODEC_MAX_PLANES ||
	    !is_power_of_2(request->dma_alignment))
		return -EINVAL;
	for (i = 0; i < request->num_planes; i++)
		if (IS_ERR_OR_NULL(dmabuf[i]) || !request->min_size[i] ||
		    request->min_size[i] > dmabuf[i]->size)
			return -EINVAL;
	surface = kzalloc(sizeof(*surface), GFP_KERNEL);
	if (!surface)
		return -ENOMEM;
	surface->session = session;
	surface->request = *request;
	surface->user_owned = true;
	mutex_lock(&core->lock);
	if (session->state != S7D_SESSION_OPEN) {
		ret = -ESHUTDOWN;
		goto unpin;
	}
	if (request->generation <= session->retired_generation) {
		ret = -ESTALE;
		goto unpin;
	}
	list_for_each_entry(existing, &session->surfaces, node)
		if (existing->request.surface_id == request->surface_id &&
		    existing->request.generation == request->generation) {
			ret = -EEXIST;
			goto unpin;
		}
	for (i = 0; i < request->num_planes; i++) {
		ret = s7d_plane_pin(core, dmabuf[i], &surface->plane[i],
				    request->min_size[i], request->dma_alignment);
		if (ret)
			goto unpin;
		for (j = 0; j < i; j++)
			if (s7d_planes_alias(&surface->plane[i], &surface->plane[j])) {
				ret = -EINVAL;
				goto unpin;
			}
		list_for_each_entry(existing, &session->surfaces, node)
			for (j = 0; j < existing->request.num_planes; j++)
				if (s7d_planes_alias(&surface->plane[i],
						     &existing->plane[j])) {
					ret = -EINVAL;
					goto unpin;
				}
	}
	list_add_tail(&surface->node, &session->surfaces);
	*out = surface;
	mutex_unlock(&core->lock);
	return 0;
unpin:
	s7d_surface_unpin(surface);
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_dma(struct s7d_codec_surface *surface, unsigned int plane,
			  dma_addr_t *addr, size_t *size)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	if (!addr || !size || plane >= surface->request.num_planes)
		return -EINVAL;
	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN) {
		ret = -ESHUTDOWN;
	} else {
		*addr = surface->plane[plane].dma;
		*size = surface->plane[plane].bytes;
	}
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_queue(struct s7d_codec_surface *surface)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN)
		ret = -ESHUTDOWN;
	else if (!surface->user_owned || surface->writing)
		ret = -EBUSY;
	else {
		surface->user_owned = false;
		surface->queued = true;
	}
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_return_queued(struct s7d_codec_surface *surface)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN)
		ret = -ESHUTDOWN;
	else if (!surface->queued || surface->writing || surface->dpb_refs ||
		 surface->user_owned)
		ret = -EBUSY;
	else {
		surface->queued = false;
		surface->written = false;
		surface->displayed = false;
		surface->user_owned = true;
	}
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_acquire(struct s7d_codec_surface *surface, u64 *token)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	if (!token)
		return -EINVAL;
	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN)
		ret = -ESHUTDOWN;
	else if (!surface->queued || surface->writing || surface->user_owned ||
		 surface->dpb_refs)
		ret = -EAGAIN;
	else if (surface->write_token == U64_MAX)
		ret = -EOVERFLOW;
	else {
		surface->queued = false;
		surface->writing = true;
		surface->written = false;
		surface->displayed = false;
		*token = ++surface->write_token;
	}
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_written(struct s7d_codec_surface *surface, u64 token)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN)
		ret = -ESHUTDOWN;
	else if (!surface->writing || surface->write_token != token)
		ret = -EINVAL;
	else {
		surface->writing = false;
		surface->written = true;
	}
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_discard(struct s7d_codec_surface *surface, u64 token)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN) {
		ret = -ESHUTDOWN;
	} else if (!surface->written || surface->writing || surface->queued ||
		 surface->user_owned || surface->displayed || surface->dpb_refs != 1 ||
		 surface->write_token != token) {
		ret = -EINVAL;
	} else {
		surface->dpb_refs = 0;
		surface->written = false;
		surface->queued = true;
	}
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_ref_get(struct s7d_codec_surface *surface)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN)
		ret = -ESHUTDOWN;
	else if (surface->dpb_refs == U32_MAX)
		ret = -EOVERFLOW;
	else
		surface->dpb_refs++;
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_surface_ref_put(struct s7d_codec_surface *surface)
{
	struct s7d_codec_core *core = surface->session->core;
	int ret = 0;

	mutex_lock(&core->lock);
	if (surface->session->state != S7D_SESSION_OPEN)
		ret = -ESHUTDOWN;
	else if (!surface->dpb_refs)
		ret = -EINVAL;
	else
		surface->dpb_refs--;
	mutex_unlock(&core->lock);
	return ret;
}

int s7d_codec_session_frame_ready(struct s7d_codec_session *session,
				  const struct s7d_codec_frame *frame)
{
	struct s7d_codec_core *core = session->core;
	struct s7d_codec_surface *surface;
	unsigned int i;
	int ret = -ENOENT;

	if (!frame)
		return -EINVAL;
	mutex_lock(&core->lock);
	if (session->state != S7D_SESSION_OPEN) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	list_for_each_entry(surface, &session->surfaces, node) {
		if (surface->request.surface_id != frame->surface_id ||
		    surface->request.generation != frame->generation)
			continue;
		if (!surface->written || surface->writing || surface->queued ||
		    surface->user_owned || surface->displayed ||
		    surface->write_token != frame->write_token) {
			ret = -EINVAL;
			goto unlock;
		}
		for (i = 0; i < surface->request.num_planes; i++)
			if (frame->bytesused[i] > surface->plane[i].bytes) {
				ret = -EINVAL;
				goto unlock;
			}
		surface->displayed = true;
		surface->user_owned = true;
		session->callbacks.frame_ready(session->callback_priv, frame);
		ret = 0;
		break;
	}
unlock:
	mutex_unlock(&core->lock);
	return ret;
}
