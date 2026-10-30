/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VP9_CONTEXT_H
#define S7D_VP9_CONTEXT_H

#include <linux/types.h>

#include "../common/s7d-codec-layout.h"
#include "s7d-vp9-prefix.h"

#define S7D_VP9_RPM_BYTES	0x800U
#define S7D_VP9_LMEM_BYTES	0xc00U
#define S7D_VP9_RPM_WORDS	48U

enum s7d_vp9_workspace_part {
	S7D_VP9_IPP,
	S7D_VP9_SAO_ABOVE,
	S7D_VP9_SAO_VERTICAL,
	S7D_VP9_RPS,
	S7D_VP9_VPS,
	S7D_VP9_SPS,
	S7D_VP9_PPS,
	S7D_VP9_SAO_UP,
	S7D_VP9_SWAP1,
	S7D_VP9_SWAP2,
	S7D_VP9_SCALELUT,
	S7D_VP9_DBLK_PARAMETERS,
	S7D_VP9_DBLK_DATA,
	S7D_VP9_SEGMENT_MAP,
	S7D_VP9_MPRED_ABOVE,
	S7D_VP9_WORK_RPM,
	S7D_VP9_WORK_LMEM,
	S7D_VP9_PROBABILITY,
	S7D_VP9_COUNTS,
	S7D_VP9_WORKSPACE_PARTS,
};

struct s7d_vp9_region {
	u32 address;
	u32 bytes;
};

struct s7d_vp9_workspace {
	struct s7d_vp9_region backing;
	struct s7d_vp9_region part[S7D_VP9_WORKSPACE_PARTS];
	struct s7d_vp9_region rpm;
	struct s7d_vp9_region lmem;
	u32 used;
};

struct s7d_vp9_rpm {
	u16 word[S7D_VP9_RPM_WORDS];
};

struct s7d_vp9_picture {
	u32 size;
	u32 body_bytes;
	u32 header_bytes;
	u32 sao_ctrl1;
	u32 ipp_axiif;
	u32 dblk_cfgb;
	u32 threshold[32];
	u32 level[16];
};

int s7d_vp9_workspace_plan(u64 base, u32 bytes,
			    const struct s7d_vp9_region *rpm,
			    const struct s7d_vp9_region *lmem,
			    struct s7d_vp9_workspace *workspace);
int s7d_vp9_workspace_validate(const struct s7d_vp9_workspace *workspace);
int s7d_vp9_rpm_snapshot(const u8 *data, size_t bytes, bool efficiency,
			 struct s7d_vp9_rpm *rpm);
int s7d_vp9_stage_keyframe(const u8 *data, size_t bytes, u8 *output,
			   size_t capacity, u32 *staged_bytes,
			   struct s7d_vp9_keyframe_prefix *prefix);
int s7d_vp9_picture_plan(const struct s7d_vp9_workspace *workspace,
			 const struct s7d_vp9_rpm *rpm,
			 const struct s7d_vp9_keyframe_prefix *prefix,
			 const struct s7d_codec_layout *layout,
			 const struct s7d_vp9_region surface[2],
			 struct s7d_vp9_picture *picture);

#endif
