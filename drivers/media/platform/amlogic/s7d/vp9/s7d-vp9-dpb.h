/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef S7D_VP9_DPB_H
#define S7D_VP9_DPB_H

#include "s7d-vp9-header-prefix.h"

#define S7D_VP9_MAX_PHYSICAL 24U
#define S7D_VP9_NO_PHYSICAL (~0U)

struct s7d_vp9_dpb_plan {
	u64 serial;
	u64 generation;
	u64 write_token;
	u32 current_slot;
	u32 logical_to_physical[S7D_VP9_REFERENCE_SLOTS];
	u32 next_map[S7D_VP9_REFERENCE_SLOTS];
	u32 reference[3];
	u32 old_mask;
	u32 next_mask;
	u32 release_mask;
};

struct s7d_vp9_dpb {
	u64 generation;
	u64 serial;
	u64 revision;
	u32 physical_count;
	u32 map[S7D_VP9_REFERENCE_SLOTS];
	struct s7d_vp9_dpb_plan pending;
	bool active;
};

int s7d_vp9_dpb_init(struct s7d_vp9_dpb *dpb, u64 generation, u32 physical_count);
/* Caller has acquired and pinned current once; old unique backings remain pinned. */
int s7d_vp9_dpb_begin(struct s7d_vp9_dpb *dpb,
		       const struct s7d_vp9_header_prefix *header,
		       u32 current_slot, u64 write_token, struct s7d_vp9_dpb_plan *plan);
/* Successful picture, DMA idle and physical pin transaction precede commit. */
int s7d_vp9_dpb_commit(struct s7d_vp9_dpb *dpb, u64 serial, u64 write_token);
/* DMA idle precedes abort; caller discards the current pin without changing old refs. */
int s7d_vp9_dpb_abort(struct s7d_vp9_dpb *dpb, u64 serial, u64 write_token);
int s7d_vp9_dpb_owned_mask(const struct s7d_vp9_dpb *dpb, u32 *mask);

#endif
