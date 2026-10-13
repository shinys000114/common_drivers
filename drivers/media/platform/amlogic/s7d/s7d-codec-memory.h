/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_MEMORY_H
#define S7D_CODEC_MEMORY_H

struct vb2_mem_ops;

/* Coherent CAPTURE MMAP only; link with the native platform owner. */
extern const struct vb2_mem_ops s7d_codec_memory_memops;

#endif
