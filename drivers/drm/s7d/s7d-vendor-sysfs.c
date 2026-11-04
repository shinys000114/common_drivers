// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/device.h>
#include <linux/gcd.h>
#include <linux/kobject.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_lock.h>

#include "s7d-caps.h"
#include "s7d-crtc.h"
#include "s7d-hdmi-output.h"
#include "s7d-vendor.h"

/*
 * The vendor HDMI transmitter and video output create these classes in
 * their probes, which need device tree nodes that are disabled while this
 * display drives the hardware; aml_media only registers their platform
 * drivers at module init. Create them when the display registers, and
 * leave out one that already exists.
 */
static const struct class s7d_vendor_amhdmitx_class = {
	.name = "amhdmitx",
};

/* Class attributes have no device; one display at most owns the class. */
static struct s7d_vendor *s7d_vendor_display_owner;

/* The mode and HDMI output of the running CRTC; false without HDMI output. */
static bool s7d_vendor_output(struct s7d_vendor *v, struct drm_display_mode *mode,
			      struct s7d_hdmi_output_state *output)
{
	struct drm_crtc *crtc = v->crtc;
	struct s7d_crtc_state *state;
	bool active;

	drm_modeset_lock(&crtc->mutex, NULL);
	state = to_s7d_crtc_state(crtc->state);
	active = state->base.active && !state->writeback_only;
	if (active) {
		*mode = state->base.mode;
		*output = state->output;
	}
	drm_modeset_unlock(&crtc->mutex);
	return active;
}

/* The vendor names of the probed modes, '*' marking the preferred one. */
static ssize_t disp_cap_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s7d_vendor *v = dev_get_drvdata(dev);
	struct drm_display_mode *mode, *seen;
	int len = 0;

	mutex_lock(&v->drm->mode_config.mutex);
	list_for_each_entry(mode, &v->hdmi->modes, head) {
		list_for_each_entry(seen, &v->hdmi->modes, head)
			if (seen == mode || !strcmp(seen->name, mode->name))
				break;
		if (seen != mode)
			continue;
		len += sysfs_emit_at(buf, len, "%s%s\n", mode->name,
				     mode->type & DRM_MODE_TYPE_PREFERRED ? "*" : "");
	}
	mutex_unlock(&v->drm->mode_config.mutex);
	return len;
}
static DEVICE_ATTR_RO(disp_cap);

/* The colour format and depth sent, as in the vendor attribute strings. */
static ssize_t attr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s7d_vendor *v = dev_get_drvdata(dev);
	struct s7d_hdmi_output_state output;
	struct drm_display_mode mode;
	const char *space;

	if (!s7d_vendor_output(v, &mode, &output))
		return sysfs_emit(buf, "\n");
	/* 4:2:2 always uses the 12-bit container. */
	if (s7d_hdmi_output_is_y422(&output))
		return sysfs_emit(buf, "422,12bit\n");
	if (s7d_hdmi_output_is_y420(&output))
		space = "420";
	else if (output.encoding == S7D_HDMI_OUTPUT_RGB8 ||
		 output.encoding == S7D_HDMI_OUTPUT_RGB10 ||
		 output.encoding == S7D_HDMI_OUTPUT_RGB12)
		space = "rgb";
	else
		space = "444";
	return sysfs_emit(buf, "%s,%ubit\n", space, output.bpc);
}
static DEVICE_ATTR_RO(attr);

/* No HDCP on this transmitter. */
static ssize_t hdcp_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "off\n");
}
static DEVICE_ATTR_RO(hdcp_mode);

static struct attribute *s7d_vendor_amhdmitx_attrs[] = {
	&dev_attr_disp_cap.attr,
	&dev_attr_attr.attr,
	&dev_attr_hdcp_mode.attr,
	NULL
};
ATTRIBUTE_GROUPS(s7d_vendor_amhdmitx);

/* The vendor mode name of the output, "null" without one. */
static ssize_t mode_show(const struct class *class, const struct class_attribute *attr,
			 char *buf)
{
	struct s7d_vendor *v = READ_ONCE(s7d_vendor_display_owner);
	struct s7d_hdmi_output_state output;
	struct drm_display_mode mode;
	char name[DRM_DISPLAY_MODE_LEN];

	if (!s7d_vendor_output(v, &mode, &output))
		return sysfs_emit(buf, "null\n");
	s7d_vendor_mode_name(&mode, name);
	return sysfs_emit(buf, "%s\n", name);
}

/* The mode comes from DRM clients only. */
static ssize_t mode_store(const struct class *class, const struct class_attribute *attr,
			  const char *buf, size_t count)
{
	return -EPERM;
}
static CLASS_ATTR_RW(mode);

/* The vendor video output information that a DRM mode gives. */
static ssize_t vinfo_show(const struct class *class, const struct class_attribute *attr,
			  char *buf)
{
	struct s7d_vendor *v = READ_ONCE(s7d_vendor_display_owner);
	struct s7d_hdmi_output_state output;
	char name[DRM_DISPLAY_MODE_LEN];
	struct drm_display_mode mode;
	bool interlace;
	u64 num, den, div;

	if (!s7d_vendor_output(v, &mode, &output))
		return sysfs_emit(buf, "current vinfo is null\n");
	s7d_vendor_mode_name(&mode, name);
	interlace = mode.flags & DRM_MODE_FLAG_INTERLACE;
	num = (u64)mode.clock * 1000 << interlace;
	den = (u64)mode.htotal * mode.vtotal;
	div = gcd(num, den) ?: 1;
	return sysfs_emit(buf, "current vinfo:\n"
			  "    name:                  %s\n"
			  "    width:                 %d\n"
			  "    height:                %d\n"
			  "    field_height:          %d\n"
			  "    sync_duration_num:     %llu\n"
			  "    sync_duration_den:     %llu\n"
			  "    htotal:                %d\n"
			  "    vtotal:                %d\n"
			  "    video_clk:             %d\n",
			  name, mode.hdisplay, mode.vdisplay, mode.vdisplay >> interlace,
			  num / div, den / div, mode.htotal, mode.vtotal,
			  s7d_mode_clock(&mode) * 1000);
}
static CLASS_ATTR_RO(vinfo);

static struct attribute *s7d_vendor_display_attrs[] = {
	&class_attr_mode.attr,
	&class_attr_vinfo.attr,
	NULL
};
ATTRIBUTE_GROUPS(s7d_vendor_display);

static const struct class s7d_vendor_display_class = {
	.name = "display",
	.class_groups = s7d_vendor_display_groups,
};

void s7d_vendor_sysfs_register(struct s7d_vendor *v)
{
	struct device *dev = v->drm->dev, *amhdmitx;
	int ret;

	ret = class_register(&s7d_vendor_amhdmitx_class);
	if (!ret) {
		amhdmitx = device_create_with_groups(&s7d_vendor_amhdmitx_class, NULL, 0, v,
						     s7d_vendor_amhdmitx_groups, "amhdmitx0");
		if (IS_ERR(amhdmitx)) {
			ret = PTR_ERR(amhdmitx);
			class_unregister(&s7d_vendor_amhdmitx_class);
		} else {
			mutex_lock(&v->lock);
			v->amhdmitx = amhdmitx;
			mutex_unlock(&v->lock);
		}
	}
	if (ret)
		dev_warn(dev, "no vendor amhdmitx0: %d\n", ret);
	WRITE_ONCE(s7d_vendor_display_owner, v);
	ret = class_register(&s7d_vendor_display_class);
	if (ret) {
		WRITE_ONCE(s7d_vendor_display_owner, NULL);
		dev_warn(dev, "no vendor display class: %d\n", ret);
	}
	v->display_class = !ret;
}

void s7d_vendor_sysfs_unregister(struct s7d_vendor *v)
{
	struct device *amhdmitx;

	if (v->display_class) {
		class_unregister(&s7d_vendor_display_class);
		WRITE_ONCE(s7d_vendor_display_owner, NULL);
		v->display_class = false;
	}
	mutex_lock(&v->lock);
	amhdmitx = v->amhdmitx;
	v->amhdmitx = NULL;
	mutex_unlock(&v->lock);
	if (amhdmitx) {
		device_unregister(amhdmitx);
		class_unregister(&s7d_vendor_amhdmitx_class);
	}
}

static void s7d_vendor_amhdmitx_event(struct s7d_vendor *v, const char *key, int value)
{
	char env[32];
	char *envp[2] = { env, NULL };

	lockdep_assert_held(&v->lock);
	if (!v->amhdmitx)
		return;
	snprintf(env, sizeof(env), "%s=%d", key, value);
	kobject_uevent_env(&v->amhdmitx->kobj, KOBJ_CHANGE, envp);
}

/* Like the vendor transmitter, hdmitx_hpd= on a change, or always if @force. */
void s7d_vendor_sysfs_hpd(struct s7d_vendor *v, bool connected, bool force)
{
	lockdep_assert_held(&v->lock);
	if (!force && v->hpd == connected)
		return;
	v->hpd = connected;
	s7d_vendor_amhdmitx_event(v, "hdmitx_hpd", connected);
}

/* hdmitx_hdcppwr=0 when the output sleeps and 1 when it is back, on a change. */
void s7d_vendor_sysfs_power(struct s7d_vendor *v, bool on)
{
	lockdep_assert_held(&v->lock);
	if (v->hdcppwr == on)
		return;
	v->hdcppwr = on;
	s7d_vendor_amhdmitx_event(v, "hdmitx_hdcppwr", on);
}
