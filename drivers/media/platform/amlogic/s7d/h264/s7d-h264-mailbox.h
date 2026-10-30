/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_MAILBOX_H
#define S7D_H264_MAILBOX_H

#include "s7d-h264-protocol.h"

#define S7D_H264_MAILBOX_RPM_BYTES (S7D_H264_RPM_WORDS * sizeof(u16))

struct regmap;
struct s7d_h264_context;

/* config.rpm borrows this packet; keep it immutable until parsing completes. */
struct s7d_h264_mailbox_config {
	struct s7d_h264_config_snapshot config;
	u8 rpm[S7D_H264_MAILBOX_RPM_BYTES];
};

/* Process/threaded context, paused firmware, same transaction lock and live refs. */
int s7d_h264_mailbox_read_config(struct regmap *map,
				 struct s7d_h264_context *context,
				 struct s7d_h264_mailbox_config *packet);
int s7d_h264_mailbox_read_slice(struct regmap *map,
				struct s7d_h264_context *context,
				void *rpm, size_t rpm_bytes);

#endif
