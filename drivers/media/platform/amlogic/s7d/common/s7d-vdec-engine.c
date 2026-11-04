// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "s7d-codec-dos.h"
#include "s7d-vdec-engine.h"

struct s7d_vdec_engine {
	struct device *dev;
	/* The armed run, its line state, and their visibility to the handlers. */
	spinlock_t lock;
	const struct s7d_vdec_irq_ops *ops;
	void *armed;
	int irq;
	/* The line is enabled: armed and not muted. */
	bool enabled;
	bool closing;
};

/* The handlers of the armed run; disarm waits for any that read them. */
static void *engine_armed(struct s7d_vdec_engine *engine,
			  const struct s7d_vdec_irq_ops **ops)
{
	unsigned long flags;
	void *priv;

	spin_lock_irqsave(&engine->lock, flags);
	priv = engine->armed;
	*ops = engine->ops;
	spin_unlock_irqrestore(&engine->lock, flags);
	return priv;
}

static irqreturn_t engine_irq(int irq, void *data)
{
	const struct s7d_vdec_irq_ops *ops;
	void *priv = engine_armed(data, &ops);

	return priv ? ops->irq(priv) : IRQ_HANDLED;
}

static irqreturn_t engine_irq_thread(int irq, void *data)
{
	const struct s7d_vdec_irq_ops *ops;
	void *priv = engine_armed(data, &ops);

	return priv ? ops->thread(priv) : IRQ_HANDLED;
}

struct s7d_vdec_engine *
s7d_vdec_engine_create(struct device *dev, struct s7d_codec_dos *dos,
		       struct module *owner)
{
	struct s7d_vdec_engine *engine;
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
	spin_lock_init(&engine->lock);
	engine->irq = s7d_codec_dos_irq(dos, S7D_CODEC_ENGINE_VDEC);
	if (engine->irq < 0) {
		ret = engine->irq;
		goto free;
	}
	ret = request_threaded_irq(engine->irq, engine_irq, engine_irq_thread,
				   IRQF_NO_AUTOEN | IRQF_ONESHOT, dev_name(dev), engine);
	if (!ret)
		return engine;
free:
	put_device(engine->dev);
	kfree(engine);
	return ERR_PTR(ret);
}

int s7d_vdec_engine_arm(struct s7d_vdec_engine *engine,
			const struct s7d_vdec_irq_ops *ops, void *priv)
{
	unsigned long flags;
	int ret = 0;

	if (!engine || !ops || !ops->irq || !ops->thread || !priv)
		return -EINVAL;
	spin_lock_irqsave(&engine->lock, flags);
	if (engine->closing) {
		ret = -ESHUTDOWN;
	} else if (engine->armed) {
		ret = -EBUSY;
	} else {
		engine->ops = ops;
		engine->armed = priv;
		engine->enabled = true;
		enable_irq(engine->irq);
	}
	spin_unlock_irqrestore(&engine->lock, flags);
	return ret;
}

void s7d_vdec_engine_mute(struct s7d_vdec_engine *engine, void *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&engine->lock, flags);
	if (priv && engine->armed == priv && engine->enabled) {
		engine->enabled = false;
		disable_irq_nosync(engine->irq);
	}
	spin_unlock_irqrestore(&engine->lock, flags);
}

void s7d_vdec_engine_disarm(struct s7d_vdec_engine *engine, void *priv)
{
	unsigned long flags;
	bool armed;

	spin_lock_irqsave(&engine->lock, flags);
	armed = priv && engine->armed == priv;
	if (armed) {
		if (engine->enabled)
			disable_irq_nosync(engine->irq);
		engine->enabled = false;
		engine->armed = NULL;
		engine->ops = NULL;
	}
	spin_unlock_irqrestore(&engine->lock, flags);
	/* A handler that saw the run before it was disarmed has finished. */
	if (armed)
		synchronize_irq(engine->irq);
}

void s7d_vdec_engine_sync(struct s7d_vdec_engine *engine)
{
	synchronize_irq(engine->irq);
}

int s7d_vdec_engine_destroy(struct s7d_vdec_engine *engine)
{
	unsigned long flags;

	if (!engine)
		return 0;
	spin_lock_irqsave(&engine->lock, flags);
	if (engine->armed) {
		spin_unlock_irqrestore(&engine->lock, flags);
		return -EBUSY;
	}
	engine->closing = true;
	spin_unlock_irqrestore(&engine->lock, flags);
	free_irq(engine->irq, engine);
	put_device(engine->dev);
	kfree(engine);
	return 0;
}
