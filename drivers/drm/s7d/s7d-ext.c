// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>

#include <drm/drm_modes.h>

#include "s7d-ext.h"
#include "s7d-hooks.h"

/*
 * s7d_ext_lock serialises the glue calls, registration and the record of the
 * running mode. The hooks take it under the CRTC mutex, so it nests inside
 * that mutex and nothing here takes a DRM lock. Every op is called under the
 * lock for now. The ops pointer is also published with RCU, so that a later
 * op that cannot sleep, such as one from the vblank interrupt, can read it
 * without the lock: unregister already waits with synchronize_rcu(), which
 * also covers readers in hard interrupts.
 */
static DEFINE_MUTEX(s7d_ext_lock);
static const struct s7d_ext_ops __rcu *s7d_ext_glue;
/* The mode announced by s7d_ext_mode_set() and not yet cleared. */
static struct drm_crtc *s7d_ext_crtc;
static struct drm_display_mode s7d_ext_mode;

static const struct s7d_ext_ops *s7d_ext_ops_locked(void)
{
	return rcu_dereference_protected(s7d_ext_glue, lockdep_is_held(&s7d_ext_lock));
}

void s7d_ext_mode_set(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	const struct s7d_ext_ops *ops;

	mutex_lock(&s7d_ext_lock);
	/* The core clears before it sets again; there is one CRTC. */
	if (!WARN_ON_ONCE(s7d_ext_crtc)) {
		s7d_ext_crtc = crtc;
		drm_mode_copy(&s7d_ext_mode, mode);
		ops = s7d_ext_ops_locked();
		if (ops && ops->mode_set)
			ops->mode_set(crtc, mode);
	}
	mutex_unlock(&s7d_ext_lock);
}

void s7d_ext_mode_clear(struct drm_crtc *crtc)
{
	const struct s7d_ext_ops *ops;

	mutex_lock(&s7d_ext_lock);
	if (crtc == s7d_ext_crtc) {
		ops = s7d_ext_ops_locked();
		if (ops && ops->mode_clear)
			ops->mode_clear(crtc);
		s7d_ext_crtc = NULL;
	}
	mutex_unlock(&s7d_ext_lock);
}

int s7d_ext_register(const struct s7d_ext_ops *ops)
{
	int ret = 0;

	mutex_lock(&s7d_ext_lock);
	if (s7d_ext_ops_locked()) {
		ret = -EBUSY;
		goto unlock;
	}
	rcu_assign_pointer(s7d_ext_glue, ops);
	/* A CRTC stop takes the lock for its clear first: the output stays up. */
	if (s7d_ext_crtc && ops->mode_set)
		ops->mode_set(s7d_ext_crtc, &s7d_ext_mode);
unlock:
	mutex_unlock(&s7d_ext_lock);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(s7d_ext_register, S7D_DRM);

void s7d_ext_unregister(const struct s7d_ext_ops *ops)
{
	mutex_lock(&s7d_ext_lock);
	if (WARN_ON(s7d_ext_ops_locked() != ops)) {
		mutex_unlock(&s7d_ext_lock);
		return;
	}
	if (s7d_ext_crtc && ops->mode_clear)
		ops->mode_clear(s7d_ext_crtc);
	RCU_INIT_POINTER(s7d_ext_glue, NULL);
	mutex_unlock(&s7d_ext_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_NS_GPL(s7d_ext_unregister, S7D_DRM);
