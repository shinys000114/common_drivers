/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef S7D_VDEC_ENGINE_H
#define S7D_VDEC_ENGINE_H

#include <linux/interrupt.h>

struct device;
struct module;
struct s7d_codec_dos;
struct s7d_vdec_engine;

struct s7d_vdec_irq_ops {
	irqreturn_t (*irq)(void *priv);
	irqreturn_t (*thread)(void *priv);
};

/*
 * The VDEC mailbox interrupt, shared by the VDEC sessions: it goes to the one
 * whose run is armed. The parent joins every consumer before destroy and
 * retains the DOS through that call.
 */
struct s7d_vdec_engine *
s7d_vdec_engine_create(struct device *dev, struct s7d_codec_dos *dos,
		       struct module *owner);
int s7d_vdec_engine_destroy(struct s7d_vdec_engine *engine);
/*
 * Process context: enable the interrupt for @priv's run. -EBUSY while another
 * run is armed, which the engine's run token rules out.
 */
int s7d_vdec_engine_arm(struct s7d_vdec_engine *engine,
			const struct s7d_vdec_irq_ops *ops, void *priv);
/*
 * Any context, also @priv's own handlers: disable the line while @priv's run
 * stays armed, so its handlers still finish. No-op for another @priv.
 */
void s7d_vdec_engine_mute(struct s7d_vdec_engine *engine, void *priv);
/*
 * Process context, holding no lock either handler takes: on return @priv's
 * run takes the interrupt no more and none of its handlers runs. No-op for a
 * @priv that is not armed.
 */
void s7d_vdec_engine_disarm(struct s7d_vdec_engine *engine, void *priv);
/* Process context: wait for the handlers that run now. */
void s7d_vdec_engine_sync(struct s7d_vdec_engine *engine);

#endif
