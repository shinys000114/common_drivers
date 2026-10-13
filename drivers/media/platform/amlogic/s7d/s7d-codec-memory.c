// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-memops.h>

#include "s7d-codec-memory.h"

struct s7d_codec_memory {
	struct device *dev;
	void *cpu;
	dma_addr_t dma;
	unsigned long bytes;
	refcount_t refs;
	struct dma_resv resv;
	struct vb2_vmarea_handler handler;
};

static void s7d_memory_put(void *priv)
{
	struct s7d_codec_memory *mem = priv;

	if (!refcount_dec_and_test(&mem->refs))
		return;
	dma_resv_fini(&mem->resv);
	dma_free_coherent(mem->dev, mem->bytes, mem->cpu, mem->dma);
	put_device(mem->dev);
	kfree(mem);
	module_put(THIS_MODULE);
}

static void *s7d_memory_alloc(struct vb2_buffer *vb, struct device *dev,
			      unsigned long bytes)
{
	struct vb2_queue *q = vb->vb2_queue;
	struct s7d_codec_memory *mem;
	int ret = -ENOMEM;

	if (!dev || !bytes || !PAGE_ALIGNED(bytes))
		return ERR_PTR(-EINVAL);
	if (q->is_output || q->memory != VB2_MEMORY_MMAP ||
	    q->non_coherent_mem || q->dma_attrs ||
	    q->dma_dir != DMA_BIDIRECTIONAL)
		return ERR_PTR(-EOPNOTSUPP);
	if (!try_module_get(THIS_MODULE))
		return ERR_PTR(-ENODEV);
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		goto put_module;
	mem->dev = get_device(dev);
	mem->bytes = bytes;
	mem->cpu = dma_alloc_coherent(dev, bytes, &mem->dma,
				      GFP_KERNEL | q->gfp_flags);
	if (!mem->cpu)
		goto put_device;
	if (mem->dma > U32_MAX || bytes - 1 > U32_MAX - mem->dma) {
		ret = -ERANGE;
		goto free_dma;
	}
	dma_resv_init(&mem->resv);
	refcount_set(&mem->refs, 1);
	mem->handler.refcount = &mem->refs;
	mem->handler.put = s7d_memory_put;
	mem->handler.arg = mem;
	return mem;

free_dma:
	dma_free_coherent(dev, bytes, mem->cpu, mem->dma);
put_device:
	put_device(mem->dev);
	kfree(mem);
put_module:
	module_put(THIS_MODULE);
	return ERR_PTR(ret);
}

static void *s7d_memory_vaddr(struct vb2_buffer *vb, void *priv)
{
	struct s7d_codec_memory *mem = priv;

	return mem->cpu;
}

static unsigned int s7d_memory_num_users(void *priv)
{
	struct s7d_codec_memory *mem = priv;

	return refcount_read(&mem->refs);
}

static int s7d_memory_mmap(void *priv, struct vm_area_struct *vma)
{
	struct s7d_codec_memory *mem = priv;
	int ret;

	ret = dma_mmap_coherent(mem->dev, vma, mem->cpu, mem->dma,
				mem->bytes);
	if (ret)
		return ret;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_private_data = &mem->handler;
	vma->vm_ops = &vb2_common_vm_ops;
	vma->vm_ops->open(vma);
	return 0;
}

static struct sg_table *
s7d_memory_map(struct dma_buf_attachment *attach,
	       enum dma_data_direction direction)
{
	struct s7d_codec_memory *mem = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);
	ret = dma_get_sgtable_attrs(mem->dev, sgt, mem->cpu, mem->dma,
				    mem->bytes, 0);
	if (ret)
		goto free_sgt;
	ret = dma_map_sgtable(attach->dev, sgt, direction,
			      DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		goto free_table;
	return sgt;

free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
	return ERR_PTR(ret);
}

static void s7d_memory_unmap(struct dma_buf_attachment *attach,
			     struct sg_table *sgt,
			     enum dma_data_direction direction)
{
	dma_unmap_sgtable(attach->dev, sgt, direction, DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

static int s7d_memory_vmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	struct s7d_codec_memory *mem = dbuf->priv;

	iosys_map_set_vaddr(map, mem->cpu);
	return 0;
}

static int s7d_memory_dmabuf_mmap(struct dma_buf *dbuf,
				 struct vm_area_struct *vma)
{
	return s7d_memory_mmap(dbuf->priv, vma);
}

static void s7d_memory_release(struct dma_buf *dbuf)
{
	s7d_memory_put(dbuf->priv);
}

static const struct dma_buf_ops s7d_memory_dmabuf_ops = {
	.map_dma_buf = s7d_memory_map,
	.unmap_dma_buf = s7d_memory_unmap,
	.vmap = s7d_memory_vmap,
	.mmap = s7d_memory_dmabuf_mmap,
	.release = s7d_memory_release,
};

static struct dma_buf *s7d_memory_get_dmabuf(struct vb2_buffer *vb,
					  void *priv, unsigned long flags)
{
	struct s7d_codec_memory *mem = priv;
	struct dma_buf *dbuf;
	DEFINE_DMA_BUF_EXPORT_INFO(info);

	info.ops = &s7d_memory_dmabuf_ops;
	info.size = mem->bytes;
	info.flags = flags;
	info.priv = mem;
	info.resv = &mem->resv;
	refcount_inc(&mem->refs);
	dbuf = dma_buf_export(&info);
	if (IS_ERR(dbuf))
		s7d_memory_put(mem);
	return dbuf;
}

const struct vb2_mem_ops s7d_codec_memory_memops = {
	.alloc = s7d_memory_alloc,
	.put = s7d_memory_put,
	.get_dmabuf = s7d_memory_get_dmabuf,
	.vaddr = s7d_memory_vaddr,
	.num_users = s7d_memory_num_users,
	.mmap = s7d_memory_mmap,
};
