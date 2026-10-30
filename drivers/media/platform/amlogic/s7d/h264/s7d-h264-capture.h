/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_CAPTURE_H
#define S7D_H264_CAPTURE_H

#include "s7d-h264-protocol.h"

struct regmap;

/*
 * Caller holds the DOS lease and DMA/DPB refs with firmware paused.
 * Programming failure requires quiesce before these refs can be released.
 */
int s7d_h264_capture_program(struct regmap *map,
			     const struct s7d_codec_layout *layout, u32 slot,
			     const struct s7d_h264_capture_plane planes[2]);
int s7d_h264_capture_set_format(struct regmap *map,
				enum s7d_codec_capture_format format);

#endif
