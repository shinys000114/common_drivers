/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_PICTURE_HW_H
#define S7D_H264_PICTURE_HW_H

struct regmap;
struct s7d_codec_layout;
struct s7d_h264_picture_plan;
struct s7d_h264_mv_buffer;
struct s7d_h264_capture_descriptor;

/* Process context; paused firmware, DOS lease and CAPTURE/DPB/MV pins are held. */
/* Any programming error requires full quiesce before releasing those pins. */
int s7d_h264_picture_program(struct regmap *map,
			     const struct s7d_codec_layout *layout,
			     const struct s7d_h264_picture_plan *picture,
			     const struct s7d_h264_mv_buffer *mv,
			     const struct s7d_h264_capture_descriptor *capture);

#endif
