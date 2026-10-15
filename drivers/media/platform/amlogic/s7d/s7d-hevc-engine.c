// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "s7d-codec-dos.h"
#include "s7d-codec-session.h"
#include "s7d-hevc-engine.h"

struct s7d_hevc_engine {
	struct device *dev;
	struct module *owner;
	struct mutex control; /* Binding and Linux IRQ lifetime. */
	spinlock_t lock; /* Cookies and callback visibility to IRQ handlers. */
	struct s7d_hevc_irq_ops ops;
	void *priv;
	struct s7d_hevc_irq_cookie active;
	struct s7d_hevc_irq_cookie pending;
	u64 serial;
	u64 last_token;
	int irq;
	bool armed;
	bool enabled;
	bool pending_valid;
	bool joined;
	bool faulted;
	bool closing;
};

static bool same_job(const struct s7d_hevc_irq_cookie *a,
		     const struct s7d_hevc_irq_cookie *b)
{
	return a->session == b->session && a->binding == b->binding &&
		a->lease_epoch == b->lease_epoch && a->input_token == b->input_token;
}

static irqreturn_t engine_irq(int irq, void *data)
{
	struct s7d_hevc_engine *engine = data;
	struct s7d_hevc_irq_cookie cookie;
	struct s7d_hevc_irq_ops ops;
	unsigned long flags;
	void *priv;
	irqreturn_t ret;

	spin_lock_irqsave(&engine->lock, flags);
	if (!engine->armed || engine->faulted || engine->closing) {
		spin_unlock_irqrestore(&engine->lock, flags);
		return IRQ_HANDLED;
	}
	cookie = engine->active;
	ops = engine->ops;
	priv = engine->priv;
	spin_unlock_irqrestore(&engine->lock, flags);
	ret = ops.irq(priv, &cookie);
	if (ret != IRQ_WAKE_THREAD)
		return ret;
	spin_lock_irqsave(&engine->lock, flags);
	if (engine->armed && !engine->faulted && same_job(&cookie, &engine->active)) {
		engine->pending = cookie;
		engine->pending_valid = true;
	} else {
		ret = IRQ_HANDLED;
	}
	spin_unlock_irqrestore(&engine->lock, flags);
	return ret;
}

static irqreturn_t engine_irq_thread(int irq, void *data)
{
	struct s7d_hevc_engine *engine = data;
	struct s7d_hevc_irq_cookie cookie;
	struct s7d_hevc_irq_ops ops;
	unsigned long flags;
	void *priv;

	spin_lock_irqsave(&engine->lock, flags);
	if (!engine->armed || engine->faulted || !engine->pending_valid ||
	    !same_job(&engine->pending, &engine->active)) {
		spin_unlock_irqrestore(&engine->lock, flags);
		return IRQ_HANDLED;
	}
	cookie = engine->pending;
	ops = engine->ops;
	priv = engine->priv;
	engine->pending_valid = false;
	spin_unlock_irqrestore(&engine->lock, flags);
	return ops.thread(priv, &cookie);
}

struct s7d_hevc_engine *
s7d_hevc_engine_create(struct device *dev, struct s7d_codec_dos *dos,
		       struct module *owner)
{
	struct s7d_hevc_engine *engine;
	int ret;

	if (!dev || IS_ERR_OR_NULL(dos))
		return ERR_PTR(-EINVAL);
#ifdef MODULE
	if (owner != THIS_MODULE)
		return ERR_PTR(-EINVAL);
#endif
	engine = kzalloc(sizeof(*engine), GFP_KERNEL);
	if (!engine)
		return ERR_PTR(-ENOMEM);
	engine->dev = get_device(dev);
	engine->owner = owner;
	mutex_init(&engine->control);
	spin_lock_init(&engine->lock);
	engine->joined = true;
	engine->irq = s7d_codec_dos_irq(dos, S7D_CODEC_ENGINE_HEVC);
	if (engine->irq < 0) {
		ret = engine->irq;
		goto free;
	}
	ret = request_threaded_irq(engine->irq, engine_irq, engine_irq_thread,
				   IRQF_NO_AUTOEN | IRQF_ONESHOT,
				   dev_name(dev), engine);
	if (!ret)
		return engine;
free:
	put_device(engine->dev);
	kfree(engine);
	return ERR_PTR(ret);
}

int s7d_hevc_engine_bind(struct s7d_hevc_engine *engine,
			struct s7d_codec_session *session, u64 lease_epoch,
			const struct s7d_hevc_irq_ops *ops, void *priv, u64 *binding)
{
	unsigned long flags;
	int ret = 0;

	if (!binding)
		return -EINVAL;
	*binding = 0;
	if (!engine || !session || !lease_epoch || !ops || !ops->irq ||
	    !ops->thread || !priv)
		return -EINVAL;
	mutex_lock(&engine->control);
	if (engine->closing || engine->faulted) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (engine->active.binding) {
		ret = -EBUSY;
		goto out;
	}
	if (engine->serial == U64_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (!try_module_get(engine->owner)) {
		ret = -ENODEV;
		goto out;
	}
	s7d_codec_session_get(session);
	spin_lock_irqsave(&engine->lock, flags);
	engine->ops = *ops;
	engine->priv = priv;
	engine->active = (struct s7d_hevc_irq_cookie) {
		.session = session, .binding = ++engine->serial,
		.lease_epoch = lease_epoch,
	};
	engine->last_token = 0;
	*binding = engine->active.binding;
	spin_unlock_irqrestore(&engine->lock, flags);
out:
	mutex_unlock(&engine->control);
	return ret;
}

int s7d_hevc_engine_arm(struct s7d_hevc_engine *engine, u64 binding, u64 token)
{
	unsigned long flags;
	int ret = 0;

	if (!engine || !binding || !token)
		return -EINVAL;
	mutex_lock(&engine->control);
	if (engine->closing || engine->faulted)
		ret = -ESHUTDOWN;
	else if (engine->active.binding != binding || token <= engine->last_token)
		ret = -ESTALE;
	else if (engine->armed || engine->enabled || !engine->joined)
		ret = -EBUSY;
	if (ret)
		goto out;
	spin_lock_irqsave(&engine->lock, flags);
	engine->active.input_token = token;
	engine->last_token = token;
	engine->pending_valid = false;
	engine->armed = true;
	engine->enabled = true;
	engine->joined = false;
	spin_unlock_irqrestore(&engine->lock, flags);
	enable_irq(engine->irq);
out:
	mutex_unlock(&engine->control);
	return ret;
}

static void engine_disarm_locked(struct s7d_hevc_engine *engine)
{
	unsigned long flags;
	bool disable;

	spin_lock_irqsave(&engine->lock, flags);
	engine->armed = false;
	engine->pending_valid = false;
	disable = engine->enabled;
	engine->enabled = false;
	spin_unlock_irqrestore(&engine->lock, flags);
	if (disable)
		disable_irq_nosync(engine->irq);
	synchronize_irq(engine->irq);
	spin_lock_irqsave(&engine->lock, flags);
	engine->joined = true;
	spin_unlock_irqrestore(&engine->lock, flags);
}

int s7d_hevc_engine_disarm(struct s7d_hevc_engine *engine, u64 binding)
{
	int ret = 0;

	if (!engine || !binding)
		return -EINVAL;
	mutex_lock(&engine->control);
	if (engine->active.binding != binding)
		ret = -ESTALE;
	else
		engine_disarm_locked(engine);
	mutex_unlock(&engine->control);
	return ret;
}

int s7d_hevc_engine_quarantine(struct s7d_hevc_engine *engine, u64 binding)
{
	unsigned long flags;
	int ret = 0;

	if (!engine || !binding)
		return -EINVAL;
	mutex_lock(&engine->control);
	if (engine->active.binding != binding) {
		ret = -ESTALE;
		goto out;
	}
	engine_disarm_locked(engine);
	spin_lock_irqsave(&engine->lock, flags);
	engine->faulted = true;
	spin_unlock_irqrestore(&engine->lock, flags);
out:
	mutex_unlock(&engine->control);
	return ret;
}

int s7d_hevc_engine_unbind(struct s7d_hevc_engine *engine, u64 binding)
{
	struct s7d_codec_session *session = NULL;
	struct module *owner = NULL;
	unsigned long flags;
	int ret = 0;

	if (!engine || !binding)
		return -EINVAL;
	mutex_lock(&engine->control);
	if (engine->active.binding != binding)
		ret = -ESTALE;
	else if (engine->faulted || engine->armed || engine->enabled || !engine->joined)
		ret = -EBUSY;
	if (ret)
		goto out;
	spin_lock_irqsave(&engine->lock, flags);
	session = engine->active.session;
	owner = engine->owner;
	engine->active = (struct s7d_hevc_irq_cookie) { 0 };
	engine->ops = (struct s7d_hevc_irq_ops) { 0 };
	engine->priv = NULL;
	spin_unlock_irqrestore(&engine->lock, flags);
out:
	mutex_unlock(&engine->control);
	if (session) {
		s7d_codec_session_put(session);
		module_put(owner);
	}
	return ret;
}

int s7d_hevc_engine_suspend_check(struct s7d_hevc_engine *engine)
{
	int ret;

	mutex_lock(&engine->control);
	ret = engine->active.binding || engine->faulted || engine->closing ? -EBUSY : 0;
	mutex_unlock(&engine->control);
	return ret;
}

int s7d_hevc_engine_destroy(struct s7d_hevc_engine *engine)
{
	unsigned long flags;

	if (!engine)
		return 0;
	mutex_lock(&engine->control);
	if (engine->active.binding || engine->faulted || engine->enabled || !engine->joined) {
		mutex_unlock(&engine->control);
		return -EBUSY;
	}
	spin_lock_irqsave(&engine->lock, flags);
	engine->closing = true;
	spin_unlock_irqrestore(&engine->lock, flags);
	mutex_unlock(&engine->control);
	free_irq(engine->irq, engine);
	put_device(engine->dev);
	kfree(engine);
	return 0;
}
