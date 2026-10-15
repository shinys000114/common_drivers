/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VIDEO_FIRMWARE_H
#define S7D_VIDEO_FIRMWARE_H

#include <linux/types.h>

struct device;
struct s7d_video_fw_handle;
struct s7d_video_fw_ticket;

struct s7d_video_fw_handle *s7d_video_fw_get(struct device *consumer);
void s7d_video_fw_put(struct s7d_video_fw_handle *handle);
int s7d_video_fw_next_epoch(struct s7d_video_fw_handle *handle, u64 *out);
/* A NULL ticket acknowledges this boot's immutable archive, not an engine load. */
int s7d_video_fw_prepare(struct s7d_video_fw_handle *handle, u64 lease_epoch,
			 struct s7d_video_fw_ticket **out);
/* Queue and cancel are non-sleeping; the caller retains the ticket. */
bool s7d_video_fw_ticket_queue(struct s7d_video_fw_ticket *ticket);
void s7d_video_fw_ticket_cancel(struct s7d_video_fw_ticket *ticket);
int s7d_video_fw_ticket_wait(struct s7d_video_fw_ticket *ticket, bool *cancelled);
/* Release in process context after all users have stopped accessing the ticket. */
void s7d_video_fw_ticket_put(struct s7d_video_fw_ticket *ticket);

#endif
