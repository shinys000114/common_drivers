// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <media/videobuf2-core.h>

#include "s7d-codec-memory.h"
#include "s7d-codec-session.h"
#include "s7d-codec-vb2.h"

int s7d_codec_vb2_pin(struct s7d_codec_session *session, struct vb2_buffer *vb,
		      const struct s7d_codec_surface_request *request,
		      struct s7d_codec_surface **out)
{
	struct dma_buf *dmabuf[S7D_CODEC_MAX_PLANES] = {};
	struct vb2_queue *q;
	unsigned int i, buffers;
	int ret = -ENOMEM;

	if (!vb || !request || !out || request->num_planes != 2)
		return -EINVAL;
	buffers = request->contiguous ? 1 : request->num_planes;
	if (vb->num_planes != buffers)
		return -EINVAL;
	q = vb->vb2_queue;
	if (q->is_output || q->memory != VB2_MEMORY_MMAP ||
	    q->mem_ops != &s7d_codec_memory_memops || q->non_coherent_mem ||
	    (q->dma_attrs & DMA_ATTR_NO_KERNEL_MAPPING) ||
	    q->dma_dir != DMA_BIDIRECTIONAL)
		return -EOPNOTSUPP;
	for (i = 0; i < buffers; i++)
		if (!vb->planes[i].mem_priv)
			return -EINVAL;
	for (i = 0; i < buffers; i++) {
		dmabuf[i] = q->mem_ops->get_dmabuf(vb, vb->planes[i].mem_priv,
						 O_RDWR | O_CLOEXEC);
		if (IS_ERR_OR_NULL(dmabuf[i])) {
			ret = dmabuf[i] ? PTR_ERR(dmabuf[i]) : -ENOMEM;
			dmabuf[i] = NULL;
			goto put;
		}
	}
	ret = s7d_codec_surface_pin(session, dmabuf, request, out);
put:
	for (i = buffers; i; i--)
		if (dmabuf[i - 1])
			dma_buf_put(dmabuf[i - 1]);
	return ret;
}
