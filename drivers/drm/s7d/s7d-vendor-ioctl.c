// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/build_bug.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/sync_file.h>
#include <uapi/linux/dma-buf.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_vblank.h>

#include <uapi/amlogic/drm/meson_drm.h>

#include "s7d-vendor.h"

MODULE_IMPORT_NS(DMA_BUF);

/*
 * Buffers for scanout, for the cursor or without a use come from the
 * display's DMA pool, like dumb buffers. The vendor driver takes the other
 * uses from the codec or system heaps, and the vendor video stack takes
 * video buffers only as UVM dma-bufs; without those paths they are refused
 * rather than taken from the scanout pool.
 */
int s7d_vendor_gem_create_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_meson_gem_create *args = data;
	struct drm_gem_dma_object *obj;
	int ret;

	if (!args->size)
		return -EINVAL;
	/* No secure heap on this SoC. */
	if (args->flags & MESON_USE_PROTECTED)
		return -EINVAL;
	/* As in the vendor driver, scanout and cursor take precedence. */
	if (args->flags && !(args->flags & (MESON_USE_SCANOUT | MESON_USE_CURSOR)))
		return -ENODEV;
	obj = drm_gem_dma_create(drm, args->size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	ret = drm_gem_handle_create(file, &obj->base, &args->handle);
	drm_gem_object_put(&obj->base);
	return ret;
}

/*
 * Drop the framebuffer from the file without disabling the planes that
 * show it, as DRM_IOCTL_MODE_CLOSEFB does: the last frame stays on screen.
 */
int s7d_vendor_rmfb_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_framebuffer *fb, *fbl;
	u32 *fb_id = data;
	bool found = false;

	fb = drm_framebuffer_lookup(drm, file, *fb_id);
	if (!fb)
		return -ENOENT;
	mutex_lock(&file->fbs_lock);
	list_for_each_entry(fbl, &file->fbs, filp_head)
		if (fbl == fb)
			found = true;
	if (found)
		list_del_init(&fb->filp_head);
	mutex_unlock(&file->fbs_lock);
	/* The reference of the file's list, then the lookup one. */
	if (found)
		drm_framebuffer_put(fb);
	drm_framebuffer_put(fb);
	return found ? 0 : -ENOENT;
}

/*
 * A sync file for the writers of a dma-buf, with the vendor errors. A
 * request that includes writing gets a signalled fence, as from the vendor
 * driver.
 */
int s7d_vendor_export_sync_file_ioctl(struct drm_device *drm, void *data,
				      struct drm_file *file)
{
	struct drm_meson_dma_buf_export_sync_file *arg = data;
	struct dma_fence *fence = NULL;
	struct sync_file *sync_file;
	struct dma_buf *dmabuf;
	int fd, ret;

	if ((arg->flags & ~DMA_BUF_SYNC_RW) || !(arg->flags & DMA_BUF_SYNC_RW))
		return -EINVAL;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	ret = -EINVAL;
	if (!arg->dmabuf_fd)
		goto put_fd;
	dmabuf = dma_buf_get(arg->dmabuf_fd);
	if (IS_ERR(dmabuf))
		goto put_fd;
	if (!(arg->flags & DMA_BUF_SYNC_WRITE)) {
		ret = dma_resv_get_singleton(dmabuf->resv, DMA_RESV_USAGE_WRITE, &fence);
		if (ret)
			goto put_buf;
	}
	if (!fence)
		fence = dma_fence_get_stub();
	sync_file = sync_file_create(fence);
	dma_fence_put(fence);
	ret = -ENOMEM;
	if (!sync_file)
		goto put_buf;
	dma_buf_put(dmabuf);
	fd_install(fd, sync_file->file);
	arg->fd = fd;
	return 0;

put_buf:
	dma_buf_put(dmabuf);
put_fd:
	put_unused_fd(fd);
	return ret;
}

int s7d_vendor_prime_handle_to_fd_ioctl(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct drm_prime_handle *args = data;

	if (args->flags & ~(DRM_CLOEXEC | DRM_RDWR | MESON_RDONLY))
		return -EINVAL;
	return drm_gem_prime_handle_to_fd(drm, file, args->handle, args->flags, &args->fd);
}

/*
 * A present fence with its own lock: its sync file can outlive the device,
 * and the last close takes the lock of a polled fence.
 */
struct s7d_vendor_fence {
	struct dma_fence base;
	spinlock_t lock; /* the fence lock, freed with the fence */
};

/* The core frees a fence without a release callback with kfree_rcu(). */
static_assert(offsetof(struct s7d_vendor_fence, base) == 0);

static const char *s7d_vendor_fence_driver_name(struct dma_fence *fence)
{
	return "meson";
}

static const char *s7d_vendor_fence_timeline_name(struct dma_fence *fence)
{
	return "present_fence";
}

static const struct dma_fence_ops s7d_vendor_present_ops = {
	.get_driver_name = s7d_vendor_fence_driver_name,
	.get_timeline_name = s7d_vendor_fence_timeline_name,
};

/*
 * Vblank wait queue callback, under the queue lock and from the vblank
 * interrupt or the CRTC stop: signal the fence, which DRM also does for
 * its own fences under the event lock held here. The vblank reference can
 * only be dropped outside these locks.
 */
static int s7d_vendor_present_wake(struct wait_queue_entry *wait, unsigned int mode,
				   int flags, void *key)
{
	struct s7d_vendor_present *p = container_of(wait, struct s7d_vendor_present, wait);
	struct dma_fence *fence = p->fence;

	list_del_init(&wait->entry);
	p->fence = NULL;
	p->puts++;
	schedule_work(&p->put_work);
	dma_fence_signal(fence);
	dma_fence_put(fence);
	return 1;
}

static void s7d_vendor_present_put(struct work_struct *work)
{
	struct s7d_vendor_present *p = container_of(work, struct s7d_vendor_present, put_work);
	unsigned int puts;

	spin_lock_irq(&p->vblank->lock);
	puts = p->puts;
	p->puts = 0;
	spin_unlock_irq(&p->vblank->lock);
	while (puts--)
		drm_crtc_vblank_put(p->crtc);
}

void s7d_vendor_present_init(struct s7d_vendor *v)
{
	struct s7d_vendor_present *p = &v->present;

	p->crtc = v->crtc;
	p->vblank = drm_crtc_vblank_waitqueue(v->crtc);
	init_waitqueue_func_entry(&p->wait, s7d_vendor_present_wake);
	INIT_LIST_HEAD(&p->wait.entry);
	INIT_WORK(&p->put_work, s7d_vendor_present_put);
	/* Not the CRTC fence context: OUT_FENCE_PTR fences have their own seqnos. */
	p->context = dma_fence_context_alloc(1);
}

/* Signal a waiting fence and refuse new ones; the device is going away. */
void s7d_vendor_present_fini(struct s7d_vendor *v)
{
	struct s7d_vendor_present *p = &v->present;
	struct dma_fence *fence;

	spin_lock_irq(&p->vblank->lock);
	p->closed = true;
	fence = p->fence;
	if (fence) {
		list_del_init(&p->wait.entry);
		p->fence = NULL;
		p->puts++;
	}
	spin_unlock_irq(&p->vblank->lock);
	if (fence) {
		dma_fence_signal(fence);
		dma_fence_put(fence);
	}
	cancel_work_sync(&p->put_work);
	s7d_vendor_present_put(&p->put_work);
}

/*
 * A sync file signalled at the next vblank of the CRTC, whatever is
 * committed; -EEXIST while the previous one waits. A stopped CRTC has no
 * next vblank: the fence is signalled at once instead of at its next start.
 */
int s7d_vendor_present_fence_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_meson_present_fence *arg = data;
	struct s7d_vendor *v = s7d_vendor_find(drm);
	struct s7d_vendor_present *p;
	struct s7d_vendor_fence *f;
	struct sync_file *sync_file;
	struct dma_fence *fence;
	bool armed = false;
	int fd, ret = 0;

	if (!v)
		return -ENODEV;
	p = &v->present;
	if (arg->crtc_idx >= drm->num_crtcs || drm_crtc_from_index(drm, arg->crtc_idx) != p->crtc)
		return -EINVAL;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	spin_lock_init(&f->lock);
	fence = &f->base;
	dma_fence_init(fence, &s7d_vendor_present_ops, &f->lock, p->context,
		       atomic64_inc_return(&p->seqno));
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto put_fence;
	}
	sync_file = sync_file_create(fence);
	if (!sync_file) {
		ret = -ENOMEM;
		goto put_fd;
	}
	if (!drm_crtc_vblank_get(p->crtc)) {
		spin_lock_irq(&p->vblank->lock);
		if (p->closed) {
			ret = -ENODEV;
		} else if (p->fence) {
			ret = -EEXIST;
		} else if (READ_ONCE(drm_crtc_vblank_crtc(p->crtc)->enabled)) {
			/* A stop clears this before it wakes the queue. */
			p->fence = dma_fence_get(fence);
			__add_wait_queue_entry_tail(p->vblank, &p->wait);
			armed = true;
		}
		spin_unlock_irq(&p->vblank->lock);
		if (!armed)
			drm_crtc_vblank_put(p->crtc);
	}
	if (ret)
		goto put_file;
	if (!armed)
		dma_fence_signal(fence);
	dma_fence_put(fence);
	fd_install(fd, sync_file->file);
	arg->fd = fd;
	return 0;

put_file:
	fput(sync_file->file);
put_fd:
	put_unused_fd(fd);
put_fence:
	dma_fence_put(fence);
	return ret;
}
