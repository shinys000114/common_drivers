/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_RPM_H
#define S7D_AV1_RPM_H

#include "s7d-av1-dpb.h"
#include "s7d-av1-sequence.h"
#include "s7d-av1-workspace.h"

struct s7d_av1_header {
	struct s7d_av1_dpb_frame frame;
	struct s7d_av1_rpm snapshot;
	u16 sequence_flags;
	u16 sequence_flags2;
	u16 segmentation;
	u8 sharpness;
	u8 filter_level[4];
	s8 filter_ref_delta[8];
	s8 filter_mode_delta[2];
	u16 segment_y[8];
	u16 segment_c[8];
	u8 filter_ref_update_mask;
	u8 filter_mode_update_mask;
	bool filter_delta_enabled;
	bool filter_delta_update;
	bool filter_delta_reset;
	bool allow_ref_frame_mvs;
};

int s7d_av1_rpm_check_sequence(const struct s7d_av1_rpm *rpm,
			      const struct s7d_av1_sequence *sequence);
int s7d_av1_rpm_read_frame(const struct s7d_av1_rpm *rpm,
			  const struct s7d_av1_sequence *sequence,
			  struct s7d_av1_header *header);

#endif
