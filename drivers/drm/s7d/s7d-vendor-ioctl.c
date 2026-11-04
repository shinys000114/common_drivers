// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/sync_file.h>
#include <uapi/linux/dma-buf.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_prime.h>

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
