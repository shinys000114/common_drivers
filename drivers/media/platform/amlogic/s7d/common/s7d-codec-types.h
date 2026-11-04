/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_TYPES_H
#define S7D_CODEC_TYPES_H

enum s7d_codec_engine {
	S7D_CODEC_ENGINE_VDEC,
	S7D_CODEC_ENGINE_HEVC,
	S7D_CODEC_ENGINE_HCODEC,
};

#define S7D_CODEC_ENGINES	(S7D_CODEC_ENGINE_HCODEC + 1)

/*
 * Sessions the HEVC core takes, picture by picture: the datasheet's four
 * 1080p60 streams. VDEC takes four H.264 sessions the same way, the pixel
 * rate of its 4K30 limit as four 1080p30 streams. HCODEC takes one.
 */
#define S7D_CODEC_HEVC_SESSIONS	4
#define S7D_CODEC_VDEC_SESSIONS	4

enum s7d_codec_frame_flags {
	S7D_CODEC_FRAME_KEY = 1U << 0,
	S7D_CODEC_FRAME_P = 1U << 1,
	S7D_CODEC_FRAME_B = 1U << 2,
	S7D_CODEC_FRAME_ERROR = 1U << 3,
};

#endif
