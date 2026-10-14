/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_PICTURE_H
#define S7D_HEVC_PICTURE_H

#include <linux/types.h>

struct regmap;
struct s7d_codec_layout;
struct s7d_hevc_rpm;
struct s7d_hevc_sps;
struct s7d_hevc_workspace;
struct s7d_hevc_dma_region;
struct s7d_hevc_picture_plan;
struct s7d_hevc_slice_info;

struct s7d_hevc_surface {
	u32 address[2];
	u32 bytes[2];
};

struct s7d_hevc_picture_buffers {
	u32 count;
	const struct s7d_hevc_surface *surface;
	const struct s7d_hevc_dma_region *mv;
};

/* Fresh held DOS lease, CPU stopped, buffers pinned; before owner IQIT init. */
int s7d_hevc_workspace_program(struct regmap *map,
			       const struct s7d_hevc_workspace *workspace);
/* Owner has initialized IQIT; CPU stopped, efficiency1 firmware selected. */
int s7d_hevc_protocol_configure(struct regmap *map);

/* Same paused lease; all registered mappings and plan references remain live. */
int s7d_hevc_picture_configure(struct regmap *map,
			       const struct s7d_hevc_workspace *workspace,
			       const struct s7d_hevc_rpm *rpm,
			       const struct s7d_hevc_sps *host,
			       const struct s7d_codec_layout *layout,
			       const struct s7d_hevc_picture_buffers *buffers,
			       const struct s7d_hevc_slice_info *slice,
			       const struct s7d_hevc_picture_plan *plan);

/* Any MMIO error may leave partial programming: caller must prove DOS halt. */

#endif
