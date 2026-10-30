// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-memops.h>

#include "s7d-codec-memory.h"
#include "s7d-codec-session.h"

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

static int s7d_memory_copy_wait(struct dma_buf *const dst[2],
				struct dma_buf *const src[2],
				struct dma_resv *resv[4], unsigned int *count_out,
				bool (*cancelled)(void *priv), void *priv)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(2000);
	unsigned int count = 0;
	unsigned int i, j, locked;
	long waited;
	bool ready;
	int ret;

	for (i = 0; i < 2; i++) {
		resv[count++] = dst[i]->resv;
		if (!src)
			continue;
		resv[count++] = src[i]->resv;
		for (j = 0; j < 2; j++)
			if (src[i]->resv == dst[j]->resv)
				return -EINVAL;
	}
	for (i = 1; i < count; i++)
		for (j = i; j && (unsigned long)resv[j - 1] > (unsigned long)resv[j]; j--)
			swap(resv[j - 1], resv[j]);
	/* A single-buffer NV12 surface carries both planes in one dma-buf. */
	for (i = 1, j = 1; i < count; i++)
		if (resv[i] != resv[j - 1])
			resv[j++] = resv[i];
	count = j;
	*count_out = count;
	for (;;) {
		if (cancelled && cancelled(priv))
			return -ECANCELED;
		ret = 0;
		for (locked = 0; locked < count; locked++)
			if (!dma_resv_trylock(resv[locked]))
				break;
		ready = locked == count;
		if (ready)
			for (i = 0; i < 2; i++) {
				if (src) {
					waited = dma_resv_wait_timeout(src[i]->resv,
								    DMA_RESV_USAGE_WRITE, true, 0);
					if (waited < 0) {
						ret = waited;
						ready = false;
						break;
					}
					ready &= waited > 0;
				}
				waited = dma_resv_wait_timeout(dst[i]->resv,
							    DMA_RESV_USAGE_READ, true, 0);
				if (waited < 0) {
					ret = waited;
					ready = false;
					break;
				}
				ready &= waited > 0;
			}
		if (ready)
			return 0;
		while (locked)
			dma_resv_unlock(resv[--locked]);
		if (ret)
			return ret;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(1000, 2000);
	}
}

int s7d_codec_memory_copy(struct dma_buf *const dst[2], const size_t dst_offset[2],
			  struct dma_buf *const src[2], const size_t src_offset[2],
			  const size_t bytes[2], bool (*cancelled)(void *priv), void *priv)
{
	struct s7d_codec_memory *source[2], *target[2];
	struct dma_resv *resv[4];
	size_t offset, chunk;
	unsigned int i, locked;
	int ret;

	for (i = 0; i < 2; i++) {
		if (!src[i] || !dst[i] || src[i]->ops != &s7d_memory_dmabuf_ops ||
		    dst[i]->ops != &s7d_memory_dmabuf_ops || !bytes[i] ||
		    src_offset[i] > src[i]->size || bytes[i] > src[i]->size - src_offset[i] ||
		    dst_offset[i] > dst[i]->size || bytes[i] > dst[i]->size - dst_offset[i])
			return -EINVAL;
		source[i] = src[i]->priv;
		target[i] = dst[i]->priv;
	}
	ret = s7d_memory_copy_wait(dst, src, resv, &locked, cancelled, priv);
	if (ret)
		return ret;

	/* Only this coherent exporter is accepted; keep fence insertion excluded. */
	dma_rmb();
	ret = 0;
	for (i = 0; i < 2 && !ret; i++) {
		for (offset = 0; offset < bytes[i]; offset += chunk) {
			if (cancelled && cancelled(priv)) {
				ret = -ECANCELED;
				break;
			}
			chunk = min_t(size_t, SZ_64K, bytes[i] - offset);
			memcpy((u8 *)target[i]->cpu + dst_offset[i] + offset,
			       (u8 *)source[i]->cpu + src_offset[i] + offset, chunk);
			cond_resched();
		}
	}
	dma_wmb();
	while (locked)
		dma_resv_unlock(resv[--locked]);
	return ret;
}

int s7d_codec_memory_copy_dma(struct dma_buf *const dst[2], const size_t dst_offset[2],
			      const struct s7d_codec_dma_copy_plane *planes,
			      bool (*cancelled)(void *priv), void *priv)
{
	struct s7d_codec_memory *target[2];
	struct dma_resv *resv[4];
	unsigned int i, locked;
	int ret;

	for (i = 0; i < 2; i++) {
		if (!dst[i] || dst[i]->ops != &s7d_memory_dmabuf_ops)
			return -EINVAL;
		target[i] = dst[i]->priv;
	}
	ret = s7d_memory_copy_wait(dst, NULL, resv, &locked, cancelled, priv);
	if (ret)
		return ret;
	dma_rmb();
	for (i = 0; i < 2 && !ret; i++) {
		const struct s7d_codec_dma_copy_plane *plane = &planes[i];
		const u8 *source = (u8 *)s7d_codec_dma_cpu(plane->memory) + plane->offset;
		u8 *destination = (u8 *)target[i]->cpu + dst_offset[i];
		size_t rows = plane->rows, row_bytes = plane->row_bytes;
		size_t row, offset, chunk;

		if (plane->source_pitch == row_bytes &&
		    plane->destination_pitch == row_bytes) {
			row_bytes *= rows;
			rows = 1;
		}
		for (row = 0; row < rows && !ret; row++)
			for (offset = 0; offset < row_bytes; offset += chunk) {
				if (cancelled && cancelled(priv)) {
					ret = -ECANCELED;
					break;
				}
				chunk = min_t(size_t, SZ_64K, row_bytes - offset);
				memcpy(destination + row * plane->destination_pitch + offset,
				       source + row * plane->source_pitch + offset, chunk);
				cond_resched();
			}
	}
	dma_wmb();
	while (locked)
		dma_resv_unlock(resv[--locked]);
	return ret;
}

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
