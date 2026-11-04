// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>

#include <uapi/amlogic/drm/meson_drm.h>

#include "s7d-gem.h"
#include "s7d-hooks.h"
#include "s7d-vendor.h"
#include "s7d-vendor-props.h"

static bool vendor_uapi = true;
module_param(vendor_uapi, bool, 0444);
MODULE_PARM_DESC(vendor_uapi,
		 "Register the display as the Amlogic vendor DRM device \"meson\" (default: true)");

/* Every DRM device in vendor mode, from s7d_vendor_init() to its release. */
static DEFINE_MUTEX(s7d_vendor_list_lock);
static LIST_HEAD(s7d_vendor_list);

/* Numbers and flags of the vendor aml_drm ioctl table. */
static const struct drm_ioctl_desc s7d_vendor_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MESON_GEM_CREATE, s7d_vendor_gem_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MESON_RMFB, s7d_vendor_rmfb_ioctl, 0),
	DRM_IOCTL_DEF_DRV(MESON_DMABUF_EXPORT_SYNC_FILE, s7d_vendor_export_sync_file_ioctl, 0),
	DRM_IOCTL_DEF_DRV(MESON_PRIME_HANDLE_TO_FD, s7d_vendor_prime_handle_to_fd_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MESON_CREAT_PRESENT_FENCE, s7d_vendor_present_fence_ioctl, 0),
};

/* The vendor ADDFB2 takes the core arguments; its own formats are not offered. */
static unsigned int s7d_vendor_ioctl_cmd(unsigned int cmd)
{
	return cmd == DRM_IOCTL_MESON_ADDFB2 ? DRM_IOCTL_MODE_ADDFB2 : cmd;
}

static long s7d_vendor_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return drm_ioctl(filp, s7d_vendor_ioctl_cmd(cmd), arg);
}

#ifdef CONFIG_COMPAT
static long s7d_vendor_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return drm_compat_ioctl(filp, s7d_vendor_ioctl_cmd(cmd), arg);
}
#else
#define s7d_vendor_compat_ioctl NULL
#endif

static const struct file_operations s7d_vendor_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = s7d_vendor_ioctl,
	.compat_ioctl = s7d_vendor_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_mmap,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	DRM_GEM_DMA_UNMAPPED_AREA_FOPS
};

/* The vendor aml_drm identity, with the buffer handling of the native driver. */
static const struct drm_driver s7d_vendor_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_RENDER,
	.ioctls = s7d_vendor_ioctls,
	.num_ioctls = ARRAY_SIZE(s7d_vendor_ioctls),
	.fops = &s7d_vendor_fops,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(s7d_gem_dumb_create),
	.name = "meson",
	.desc = "Amlogic Meson DRM driver",
	.date = "20220603",
	.major = 2,
	.minor = 0,
};

/* The vendor identity only renames the native driver and adds a render node. */
static bool s7d_vendor_same_ops(const struct drm_driver *native)
{
	const struct drm_driver *vendor = &s7d_vendor_drm_driver;

	return native->driver_features == (vendor->driver_features & ~DRIVER_RENDER) &&
		native->dumb_create == vendor->dumb_create &&
		native->gem_prime_import_sg_table == vendor->gem_prime_import_sg_table &&
		!native->gem_create_object && !native->gem_prime_import &&
		!native->open && !native->postclose && !native->release &&
		!native->debugfs_init && !native->fbdev_probe &&
		!native->dumb_map_offset && !native->num_ioctls;
}

const struct drm_driver *s7d_vendor_driver(const struct drm_driver *native)
{
	if (!vendor_uapi || WARN_ON(!s7d_vendor_same_ops(native)))
		return native;
	return &s7d_vendor_drm_driver;
}

bool s7d_vendor_enabled(const struct drm_device *drm)
{
	return drm->driver == &s7d_vendor_drm_driver;
}

struct s7d_vendor *s7d_vendor_find(const struct drm_device *drm)
{
	struct s7d_vendor *v;

	mutex_lock(&s7d_vendor_list_lock);
	list_for_each_entry(v, &s7d_vendor_list, node)
		if (v->drm == drm)
			goto out;
	v = NULL;
out:
	mutex_unlock(&s7d_vendor_list_lock);
	return v;
}

static void s7d_vendor_release(struct drm_device *drm, void *data)
{
	struct s7d_vendor *v = data;

	mutex_lock(&s7d_vendor_list_lock);
	list_del(&v->node);
	mutex_unlock(&s7d_vendor_list_lock);
}

int s7d_vendor_init(struct drm_device *drm, const struct s7d_vendor_objs *objs)
{
	struct s7d_vendor *v;
	int ret;

	if (!s7d_vendor_enabled(drm))
		return 0;
	v = drmm_kzalloc(drm, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->drm = drm;
	v->crtc = objs->crtc;
	v->hdmi = objs->hdmi;
	v->card_minor = -1;
	v->render_minor = -1;
	s7d_vendor_present_init(v);
	ret = s7d_vendor_props_attach(drm, objs);
	if (ret)
		return ret;
	mutex_lock(&s7d_vendor_list_lock);
	list_add(&v->node, &s7d_vendor_list);
	mutex_unlock(&s7d_vendor_list_lock);
	return drmm_add_action_or_reset(drm, s7d_vendor_release, v);
}

static int s7d_vendor_debugfs_show(struct seq_file *m, void *data)
{
	struct s7d_vendor *v = m->private;

	seq_printf(m, "card_minor: %d\n", v->card_minor);
	seq_printf(m, "render_minor: %d\n", v->render_minor);
	seq_printf(m, "node_order_ok: %d\n", v->node_order_ok);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(s7d_vendor_debugfs);

int s7d_vendor_register(struct drm_device *drm)
{
	struct s7d_vendor *v = s7d_vendor_find(drm);

	if (!v)
		return 0;
	/* Minors go to the first DRM device allocated; report, never renumber. */
	v->card_minor = drm->primary ? drm->primary->index : -1;
	v->render_minor = drm->render ? drm->render->index : -1;
	v->node_order_ok = v->card_minor == 0 && v->render_minor == 128;
	if (!v->node_order_ok)
		dev_err(drm->dev, "display is card%d and renderD%d; vendor userspace opens card0 and renderD128\n",
			v->card_minor, v->render_minor);
	v->debugfs = debugfs_create_dir("s7d", drm->debugfs_root);
	debugfs_create_file("vendor", 0444, v->debugfs, v, &s7d_vendor_debugfs_fops);
	return 0;
}

void s7d_vendor_unregister(struct drm_device *drm)
{
	struct s7d_vendor *v = s7d_vendor_find(drm);

	if (!v)
		return;
	s7d_vendor_present_fini(v);
	debugfs_remove_recursive(v->debugfs);
	v->debugfs = NULL;
}
