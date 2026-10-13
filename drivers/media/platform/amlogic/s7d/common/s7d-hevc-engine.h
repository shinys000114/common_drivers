/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef S7D_HEVC_ENGINE_H
#define S7D_HEVC_ENGINE_H

#include <linux/interrupt.h>
#include <linux/types.h>

struct device;
struct module;
struct s7d_codec_dos;
struct s7d_codec_session;
struct s7d_hevc_engine;

struct s7d_hevc_irq_cookie {
	struct s7d_codec_session *session;
	u64 binding;
	u64 lease_epoch;
	u64 input_token;
};

struct s7d_hevc_irq_ops {
	irqreturn_t (*irq)(void *priv, const struct s7d_hevc_irq_cookie *cookie);
	irqreturn_t (*thread)(void *priv, const struct s7d_hevc_irq_cookie *cookie);
};

/* Parent joins all consumers before destroy and retains DOS through that call. */
struct s7d_hevc_engine *
s7d_hevc_engine_create(struct device *dev, struct s7d_codec_dos *dos,
		       struct module *owner);
int s7d_hevc_engine_destroy(struct s7d_hevc_engine *engine);
/* Consumer holds the exclusive HEVC DOS lease and a live session reference. */
int s7d_hevc_engine_bind(struct s7d_hevc_engine *engine,
			struct s7d_codec_session *session, u64 lease_epoch,
			const struct s7d_hevc_irq_ops *ops, void *priv, u64 *binding);
/* Clear the mailbox while masked before arming a new monotonically numbered job. */
int s7d_hevc_engine_arm(struct s7d_hevc_engine *engine, u64 binding, u64 token);
/* Process context; do not hold a lock needed by either IRQ callback. */
int s7d_hevc_engine_disarm(struct s7d_hevc_engine *engine, u64 binding);
/* Failed DMA halt retains the binding, session and module until reboot. */
int s7d_hevc_engine_quarantine(struct s7d_hevc_engine *engine, u64 binding);
/* Caller proved full DOS halt and joined codec work; IRQ disarm already joined. */
int s7d_hevc_engine_unbind(struct s7d_hevc_engine *engine, u64 binding);
int s7d_hevc_engine_suspend_check(struct s7d_hevc_engine *engine);

#endif
