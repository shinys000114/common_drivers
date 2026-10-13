/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_VB2_H
#define S7D_CODEC_VB2_H

struct vb2_buffer;
struct s7d_codec_session;
struct s7d_codec_surface;
struct s7d_codec_surface_request;

int s7d_codec_vb2_pin(struct s7d_codec_session *session, struct vb2_buffer *vb,
		      const struct s7d_codec_surface_request *request,
		      struct s7d_codec_surface **out);

#endif
