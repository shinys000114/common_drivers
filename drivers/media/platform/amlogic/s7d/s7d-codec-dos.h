/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_DOS_H
#define S7D_CODEC_DOS_H

#include "s7d-codec-types.h"

struct module;
struct platform_device;
struct regmap;
struct s7d_codec_dos;

/* The caller serializes lifetime and pins its module before using a lease. */
struct s7d_codec_dos *
s7d_codec_dos_create(struct platform_device *pdev, struct module *owner);
/* The caller has freed its IRQ and drained all callbacks before destroy. */
int s7d_codec_dos_destroy(struct s7d_codec_dos *dos);
int s7d_codec_dos_irq(struct s7d_codec_dos *dos, enum s7d_codec_engine engine);
/* Borrowed protocol map; gates, reset, SRAM and DMC remain DOS-owner-only. */
struct regmap *s7d_codec_dos_regmap(struct s7d_codec_dos *dos,
				 enum s7d_codec_engine engine);
int s7d_codec_dos_hold(struct s7d_codec_dos *dos,
		       enum s7d_codec_engine engine);
/* Process context; initializes local access once per held lease. */
int s7d_codec_dos_prepare_access(struct s7d_codec_dos *dos,
			       enum s7d_codec_engine engine);
int s7d_codec_dos_prepare(struct s7d_codec_dos *dos,
			  enum s7d_codec_engine engine);
/* Firmware/input DMA backing and descriptors must already have live refs. */
int s7d_codec_dos_port_enable(struct s7d_codec_dos *dos,
			      enum s7d_codec_engine engine);
/* Process context only; this does not synchronize the caller's IRQ or work. */
int s7d_codec_dos_halt(struct s7d_codec_dos *dos,
		       enum s7d_codec_engine engine);
/* Keeps context registers until the caller saves them; IRQ/work stay caller-owned. */
int s7d_codec_dos_park(struct s7d_codec_dos *dos,
		       enum s7d_codec_engine engine);
/* Caller holds firmware/context/backing refs and an armed mailbox/FIFO. */
int s7d_codec_dos_start(struct s7d_codec_dos *dos,
			enum s7d_codec_engine engine);
/* A failure retains the lease and pins the owner until an explicit retry. */
int s7d_codec_dos_release(struct s7d_codec_dos *dos,
			  enum s7d_codec_engine engine);
int s7d_codec_dos_suspend_check(struct s7d_codec_dos *dos);

#endif
