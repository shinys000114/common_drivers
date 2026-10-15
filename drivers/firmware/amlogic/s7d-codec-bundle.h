/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_BUNDLE_H
#define S7D_CODEC_BUNDLE_H

#include <linux/types.h>

struct s7d_codec_bundle_view {
	const u8 *data;
	size_t size;
};

/* Caller keeps data immutable/alive and view disjoint; errors leave view intact. */
int s7d_codec_bundle_parse(const u8 *data, size_t size,
			   struct s7d_codec_bundle_view *view);

#endif
