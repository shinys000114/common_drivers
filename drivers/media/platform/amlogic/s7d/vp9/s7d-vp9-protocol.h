/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VP9_PROTOCOL_H
#define S7D_VP9_PROTOCOL_H

#include "s7d-vp9-inter-picture.h"

struct s7d_vp9_protocol_frame {
	const struct s7d_vp9_workspace *workspace;
	const struct s7d_codec_layout *layout;
	const struct s7d_vp9_hw_surface *surface;
	u32 physical_count;
	u32 decode_count;
	/* MMU mode: FBC references at bit_depth; else linear 8-bit (DW 0x10). */
	bool compressed;
	u32 bit_depth;
};

/*
 * Exclusive S7D VP9 lease, FW19/core2/no-swap and IQIT prepared; CPU/input off.
 * FW19 (vp9_mmu) serves both the linear and the compressed (MMU) mode.
 * Persistent probability/count/segment/MV backings are initialized or restored
 * by the caller, never cleared here. Per-job init sets SCRATCH0 to idle (0) and
 * SCRATCH_M to decode_count. No run/resume, stream fetch, IRQ/mailbox, DOS reset,
 * clock or PM operation is performed. First MMIO error requires full halt.
 */
int s7d_vp9_protocol_init(struct regmap *map,
			 const struct s7d_vp9_protocol_frame *frame);

#endif
