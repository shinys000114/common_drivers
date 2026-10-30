/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_MEMORY_H
#define S7D_CODEC_MEMORY_H

#include <linux/types.h>

struct dma_buf;
struct s7d_codec_dma_copy_plane;
struct vb2_mem_ops;

/* Coherent CAPTURE MMAP only; link with the native platform owner. */
extern const struct vb2_mem_ops s7d_codec_memory_memops;

int s7d_codec_memory_copy(struct dma_buf *const dst[2], const size_t dst_offset[2],
			  struct dma_buf *const src[2], const size_t src_offset[2],
			  const size_t bytes[2], bool (*cancelled)(void *priv), void *priv);

/* Internal: the session owner has validated and pinned every source plane. */
int s7d_codec_memory_copy_dma(struct dma_buf *const dst[2], const size_t dst_offset[2],
			      const struct s7d_codec_dma_copy_plane *planes,
			      bool (*cancelled)(void *priv), void *priv);

#endif
