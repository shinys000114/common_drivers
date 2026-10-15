/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_TEE_H
#define S7D_CODEC_TEE_H

#include <linux/atomic.h>
#include <linux/types.h>

struct device;
struct s7d_codec_bundle_view;

enum s7d_codec_tee_phase {
	S7D_CODEC_TEE_NONE,
	S7D_CODEC_TEE_OPEN,
	S7D_CODEC_TEE_INVOKE,
};

struct s7d_codec_tee_result {
	size_t submitted_bytes;
	size_t accepted_bytes;
	int linux_status;
	int close_status;
	u32 tee_status;
	u32 tee_origin;
	enum s7d_codec_tee_phase phase;
	bool tee_status_valid;
	bool upload_started;
	bool upload_complete;
	bool close_attempted;
	bool cancelled;
	bool complete;
	bool partial_upload;
};

/*
 * Caller owns one process-context job, cancel_id and immutable view references.
 * Cancellation is checked between calls; no in-flight TEE timeout is provided.
 */
int s7d_codec_tee_upload(struct device *dev,
			 const struct s7d_codec_bundle_view *view, u32 cancel_id,
			 const atomic_t *cancel, struct s7d_codec_tee_result *result);

#endif
