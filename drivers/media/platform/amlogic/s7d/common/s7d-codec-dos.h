/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_DOS_H
#define S7D_CODEC_DOS_H

#include <linux/types.h>

#include "s7d-codec-types.h"

struct module;
struct platform_device;
struct regmap;
struct s7d_codec_dos;

/*
 * VDEC, the HEVC core and HCODEC are leased on their own: a lease, its
 * sequences and a fault concern only that engine, so sessions on different
 * engines run at the same time. Each engine has one holder at a time, which
 * runs it. The caller serializes lifetime and pins its module before using
 * a lease.
 */
struct s7d_codec_dos *
s7d_codec_dos_create(struct platform_device *pdev, struct module *owner);
/* The caller has freed its IRQ and drained all callbacks before destroy. */
int s7d_codec_dos_destroy(struct s7d_codec_dos *dos);
int s7d_codec_dos_irq(struct s7d_codec_dos *dos, enum s7d_codec_engine engine);
/* Borrowed protocol map; gates, reset, SRAM and DMC remain DOS-owner-only. */
struct regmap *s7d_codec_dos_regmap(struct s7d_codec_dos *dos,
				 enum s7d_codec_engine engine);

/*
 * One holder's lease of an engine. The holder embeds it zeroed, passes it to
 * every call on the engine, and keeps it until release succeeds; the DOS
 * owns its fields.
 */
struct s7d_codec_lease {
	struct s7d_codec_dos *dos;
	enum s7d_codec_engine engine;
	bool held;
};

int s7d_codec_dos_hold(struct s7d_codec_dos *dos, enum s7d_codec_engine engine,
		       struct s7d_codec_lease *lease);
/* HCODEC has only a JPEG 666.67MHz lease; generic hold(HCODEC) is unsupported. */
int s7d_codec_dos_jpeg_hold(struct s7d_codec_dos *dos, struct s7d_codec_lease *lease);
int s7d_codec_dos_jpeg_prepare(struct s7d_codec_lease *lease);
/* Caller retains input, output and firmware backing before opening AXI9. */
int s7d_codec_dos_jpeg_port_enable(struct s7d_codec_lease *lease);
/* Caller has loaded firmware and armed the complete JPEG context/mailbox2. */
int s7d_codec_dos_jpeg_start(struct s7d_codec_lease *lease);
/* Process context; initializes local access once per powered engine. */
int s7d_codec_dos_prepare_access(struct s7d_codec_lease *lease);
int s7d_codec_dos_prepare(struct s7d_codec_lease *lease);
/* Firmware/input DMA backing and descriptors must already have live refs. */
int s7d_codec_dos_port_enable(struct s7d_codec_lease *lease);
/* Post-TEE workspace setup; issues IQIT reset before protocol configuration. */
int s7d_codec_dos_hevc_init(struct s7d_codec_lease *lease);
/* Process context; the caller arms/joins its IRQ, and owns mailbox clear. */
int s7d_codec_dos_hevc_irq_mask(struct s7d_codec_lease *lease, bool enable);
/* Caller has pinned/staged the frame input and completed DMA visibility. */
int s7d_codec_dos_hevc_input_enable(struct s7d_codec_lease *lease);
/* Process context only; this does not synchronize the caller's IRQ or work. */
int s7d_codec_dos_halt(struct s7d_codec_lease *lease);
/* VDEC context stop only; HEVC park/resume remains unsupported. */
int s7d_codec_dos_park(struct s7d_codec_lease *lease);
/* Caller holds firmware/context/backing refs and an armed mailbox/FIFO. */
int s7d_codec_dos_start(struct s7d_codec_lease *lease);
/* A failure retains the lease and pins the owner until an explicit retry. */
int s7d_codec_dos_release(struct s7d_codec_lease *lease);
/*
 * Zero when the engine can take another session, -EBUSY while its holders
 * use it up, -EIO after a fault or failed release left it unusable.
 */
int s7d_codec_dos_engine_state(struct s7d_codec_dos *dos, enum s7d_codec_engine engine);
/* -EBUSY while any engine is held, prepared or faulted. */
int s7d_codec_dos_suspend_check(struct s7d_codec_dos *dos);

#endif
