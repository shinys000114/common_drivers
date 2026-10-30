/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_WORKSPACE_H
#define S7D_AV1_WORKSPACE_H

#include <linux/types.h>

#define S7D_AV1_RPM_BYTES 0x100U
#define S7D_AV1_RPM_WORDS 101U
#define S7D_AV1_LMEM_BYTES 0xc00U
#define S7D_AV1_MMU_MAP_BYTES 0x4800U
#define S7D_AV1_MMU_MAX_PAGES 0x1200U
#define S7D_AV1_REFERENCE_SLOTS 8U
#define S7D_AV1_FRAME_SLOTS 16U

enum s7d_av1_workspace_part {
	S7D_AV1_IPP,
	S7D_AV1_SAO_ABOVE,
	S7D_AV1_SAO_VERTICAL,
	S7D_AV1_RPS,
	S7D_AV1_VPS,
	S7D_AV1_SEGMENT_MAP,
	S7D_AV1_DAALA_TOP,
	S7D_AV1_SAO_UP,
	S7D_AV1_SWAP,
	S7D_AV1_CDF,
	S7D_AV1_GMC,
	S7D_AV1_SCALELUT,
	S7D_AV1_DBLK_PARAMETERS,
	S7D_AV1_DBLK_DATA,
	S7D_AV1_CDEF_DATA,
	S7D_AV1_UPSCALE_DATA,
	S7D_AV1_FILM_GRAIN,
	S7D_AV1_MMU_VBH,
	S7D_AV1_COMPRESSED_HEADERS,
	S7D_AV1_MPRED_ABOVE,
	S7D_AV1_WORK_RPM,
	S7D_AV1_WORK_LMEM,
	S7D_AV1_WORKSPACE_PARTS,
};

struct s7d_av1_region {
	u32 address;
	u32 bytes;
};

struct s7d_av1_workspace {
	struct s7d_av1_region backing;
	struct s7d_av1_region part[S7D_AV1_WORKSPACE_PARTS];
	struct s7d_av1_region rpm;
	struct s7d_av1_region lmem;
	u32 max_width;
	u32 max_height;
	u32 used;
};

struct s7d_av1_rpm {
	u16 word[S7D_AV1_RPM_WORDS];
};

struct s7d_av1_reference_layout {
	u32 body_bytes;
	u32 body_pages;
	u32 body_allocation;
	u32 header_bytes;
	u32 header_capacity;
	u32 mv_bytes;
	u32 mv_allocation;
};

int s7d_av1_workspace_size(u32 width, u32 height, u32 *bytes);
int s7d_av1_workspace_plan(u64 base, u32 bytes, u32 width, u32 height,
			   const struct s7d_av1_region *rpm,
			   const struct s7d_av1_region *lmem,
			   struct s7d_av1_workspace *workspace);
int s7d_av1_workspace_validate(const struct s7d_av1_workspace *workspace);
/* Firmware must be paused and its coherent writes visible before snapshot. */
int s7d_av1_rpm_snapshot(const u8 *data, size_t bytes, bool efficiency,
			 struct s7d_av1_rpm *rpm);
int s7d_av1_reference_layout(u32 width, u32 height, u32 bit_depth,
			     u32 superblock_size,
			     struct s7d_av1_reference_layout *layout);
/* Caller retains the coherent body/map and excludes all MMU readers. */
int s7d_av1_mmu_map_contiguous(const struct s7d_av1_region *body,
			       u32 pages, u8 *map, size_t bytes);

#endif
