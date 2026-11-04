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

/*
 * A picture's memory: the NV12 output (address/bytes, Y then UV) and, when
 * the stream decodes with compressed references, its Amlogic FBC header and
 * contiguous body (zero otherwise, or when the body is scattered).
 */
struct s7d_hevc_surface {
	u32 address[2];
	u32 bytes[2];
	u32 header;
	u32 header_bytes;
	u32 body;
	u32 body_bytes;
};

struct s7d_hevc_picture_buffers {
	u32 count;
	const struct s7d_hevc_surface *surface;
	const struct s7d_hevc_dma_region *mv;
	/* MMU firmware: FBC references, NV12 by double write; else linear DW 0x10. */
	bool compressed;
	/* Compressed only: no NV12 double write, surface address/bytes zero (AM08F/AM10F). */
	bool fbc_only;
	/*
	 * Compressed only: each body is scattered system pages the SAO MMU
	 * writes through the frame map (surface body/body_bytes zero).
	 */
	bool scattered;
};

/* Bytes the hardware writes per picture in FBC (vendor compute_losless_comp_*). */
u32 s7d_hevc_fbc_body_bytes(u32 width, u32 height, u32 bit_depth);
u32 s7d_hevc_fbc_header_bytes(u32 width, u32 height);

/* Fresh held DOS lease, CPU stopped, buffers pinned; before owner IQIT init. */
int s7d_hevc_workspace_program(struct regmap *map,
			       const struct s7d_hevc_workspace *workspace,
			       bool compressed);
/* Owner has initialized IQIT; CPU stopped, efficiency1 firmware selected. */
int s7d_hevc_protocol_configure(struct regmap *map, bool compressed);

/* Same paused lease; all registered mappings and plan references remain live. */
int s7d_hevc_picture_configure(struct regmap *map,
			       const struct s7d_hevc_workspace *workspace,
			       const struct s7d_hevc_rpm *rpm,
			       const struct s7d_hevc_sps *host,
			       const struct s7d_codec_layout *layout,
			       const struct s7d_hevc_picture_buffers *buffers,
			       const struct s7d_hevc_slice_info *slice,
			       const struct s7d_hevc_picture_plan *plan);

/* A later slice segment of that picture, at its header pause; @plan from the DPB. */
int s7d_hevc_slice_configure(struct regmap *map,
			     const struct s7d_hevc_rpm *rpm,
			     const struct s7d_hevc_sps *host,
			     const struct s7d_codec_layout *layout,
			     const struct s7d_hevc_picture_buffers *buffers,
			     const struct s7d_hevc_slice_info *slice,
			     const struct s7d_hevc_picture_plan *plan);
/* Any MMIO error may leave partial programming: caller must prove DOS halt. */

#endif
