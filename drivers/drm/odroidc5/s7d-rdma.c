// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/reset.h>
#include <linux/string.h>

#include "s7d-rdma.h"

#define RDMA_START		0x1102
#define RDMA_END		0x1103
#define RDMA_ACCESS_AUTO	0x1110
#define RDMA_ACCESS_AUTO2	0x1111
#define RDMA_ACCESS_MAN		0x1113
#define RDMA_CTRL		0x1114
#define RDMA_STATUS		0x1115
#define RDMA_SRC1		0x1123
#define RDMA_DONE1		BIT(25)
#define RDMA_WRITE1		BIT(5)
#define RDMA_CONFIG		(3 << 4 | 3 << 2)
#define RDMA_TABLE_BYTES	SZ_4K
#define RDMA_MAX_ENTRIES	(RDMA_TABLE_BYTES / sizeof(struct s7d_rdma_entry))

static u32 rdma_read(struct s7d_rdma *r, u32 reg)
{
	return readl(r->vcbus + reg * 4);
}

static void rdma_write(struct s7d_rdma *r, u32 reg, u32 value)
{
	writel(value, r->vcbus + reg * 4);
}

int s7d_rdma_init(struct s7d_rdma *r, struct device *dev, void __iomem *vcbus,
		  struct reset_control *reset)
{
	if (!r || !dev || !vcbus || IS_ERR_OR_NULL(reset))
		return -EINVAL;
	*r = (struct s7d_rdma) { .dev = dev, .vcbus = vcbus, .reset = reset };
	spin_lock_init(&r->lock);
	r->table = dma_alloc_coherent(dev, RDMA_TABLE_BYTES, &r->dma, GFP_KERNEL);
	if (!r->table)
		return -ENOMEM;
	/* The SC2-compatible S7D path has no descriptor-address MSB registers. */
	if (r->dma > U32_MAX - (RDMA_TABLE_BYTES - 1) || !IS_ALIGNED(r->dma, 8)) {
		dma_free_coherent(dev, RDMA_TABLE_BYTES, r->table, r->dma);
		r->table = NULL;
		return -ERANGE;
	}
	return 0;
}

int s7d_rdma_quiesce(struct s7d_rdma *r)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&r->lock, flags);
	r->ready = false;
	r->fault = true;
	/* Do not access MMIO: a failed prior reset may already hold this block. */
	spin_unlock_irqrestore(&r->lock, flags);

	ret = reset_control_assert(r->reset);
	if (ret)
		return ret;
	ret = reset_control_status(r->reset);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	spin_lock_irqsave(&r->lock, flags);
	r->pending = false;
	r->fault = false;
	spin_unlock_irqrestore(&r->lock, flags);
	return 0;
}

int s7d_rdma_prepare(struct s7d_rdma *r)
{
	unsigned long flags;
	unsigned int i;
	int ret;

	if (!r->table)
		return -EINVAL;
	ret = s7d_rdma_quiesce(r);
	if (ret)
		return ret;
	/* IRQ avoids MMIO while ready is false, including this sleeping call. */
	r->fault = true;
	ret = reset_control_deassert(r->reset);
	if (ret)
		return ret;
	ret = reset_control_status(r->reset);
	if (ret)
		return ret < 0 ? ret : -EIO;

	spin_lock_irqsave(&r->lock, flags);
	/* Reset need not clear register configuration; mask all seven sources. */
	for (i = 0; i < 7; i++)
		rdma_write(r, RDMA_SRC1 + i, 0);
	rdma_write(r, RDMA_ACCESS_AUTO, RDMA_WRITE1);
	rdma_write(r, RDMA_ACCESS_AUTO2, 0);
	rdma_write(r, RDMA_ACCESS_MAN, 0);
	/* Preserve the desired burst settings while acknowledging W1C done bits. */
	rdma_write(r, RDMA_CTRL, RDMA_CONFIG | GENMASK(31, 24));
	if (rdma_read(r, RDMA_STATUS) & GENMASK(31, 24)) {
		r->fault = true;
		ret = -EIO;
	} else {
		r->fault = false;
		r->ready = true;
	}
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

int s7d_rdma_submit(struct s7d_rdma *r, const struct s7d_rdma_entry *entries,
		    unsigned int count)
{
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	if (!entries || !count || count > RDMA_MAX_ENTRIES)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		u32 reg = le32_to_cpu(entries[i].reg);
		u32 value = le32_to_cpu(entries[i].value);

		/* Replays before CPU masking must be idempotent linear OSD writes. */
		if (reg == 0x1a1b && (value & ~0xcU) == 0x8500)
			continue;
		if (reg != 0x1a14 && reg != 0x1a15 &&
		    reg != 0x1a1c && reg != 0x1a1d)
			return -EINVAL;
	}

	spin_lock_irqsave(&r->lock, flags);
	if (!r->ready || r->fault) {
		ret = -EIO;
		goto out;
	}
	if (r->pending) {
		ret = -EBUSY;
		goto out;
	}
	if (rdma_read(r, RDMA_SRC1)) {
		r->fault = true;
		ret = -EIO;
		goto out;
	}

	/* Clear stale completion before this submission can trigger an IRQ. */
	rdma_write(r, RDMA_CTRL, RDMA_CONFIG | RDMA_DONE1);
	if (rdma_read(r, RDMA_STATUS) & RDMA_DONE1) {
		r->fault = true;
		ret = -EIO;
		goto out;
	}
	/*
	 * S7D executes OSD payload but does not self-mask RDMA_SRC1 from a
	 * descriptor. Keep this immutable list until CPU masking and a verified
	 * engine reset in the threaded IRQ. Delayed IRQs may replay these same
	 * idempotent values; they must never fetch a reused table or old buffer.
	 */
	memcpy(r->table, entries, count * sizeof(*entries));
	dma_wmb();
	rdma_write(r, RDMA_START, lower_32_bits(r->dma));
	rdma_write(r, RDMA_END, lower_32_bits(r->dma) +
		   count * sizeof(*entries) - 1);
	r->pending = true;
	/* SC2: a 16-bit source mask in its own register, bit 0 is VIU vsync. */
	rdma_write(r, RDMA_SRC1, BIT(0));
out:
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

enum s7d_rdma_result s7d_rdma_irq(struct s7d_rdma *r)
{
	enum s7d_rdma_result result = S7D_RDMA_NO_IRQ;
	unsigned long flags;
	u32 source, status;

	spin_lock_irqsave(&r->lock, flags);
	/* Do not access a block held in reset or undergoing prepare/quiesce. */
	if (!r->ready)
		goto out;
	if (!(rdma_read(r, RDMA_STATUS) & RDMA_DONE1))
		goto out;
	rdma_write(r, RDMA_CTRL, RDMA_CONFIG | RDMA_DONE1);
	if (!r->pending) {
		result = S7D_RDMA_STALE_IRQ;
		goto out;
	}
	if (r->fault)
		goto fault;
	/* Stop future triggers, but do not assume an in-flight replay is idle. */
	rdma_write(r, RDMA_SRC1, 0);
	source = rdma_read(r, RDMA_SRC1);
	if (source) {
		dev_err(r->dev, "RDMA CPU source mask failed: %#x\n", source);
		goto fault;
	}
	status = rdma_read(r, RDMA_STATUS);
	if (status & RDMA_DONE1) {
		dev_err(r->dev, "RDMA completion acknowledgement failed: %#x\n", status);
		goto fault;
	}
	/* IRQF_ONESHOT masks this IRQ until the sleeping reset has finished. */
	r->ready = false;
	result = S7D_RDMA_NEEDS_DRAIN;
	goto out;
fault:
	/* No completion: retain the table until reset confirms quiescence. */
	r->fault = true;
	result = S7D_RDMA_FAULT;
out:
	spin_unlock_irqrestore(&r->lock, flags);
	return result;
}

int s7d_rdma_finish(struct s7d_rdma *r)
{
	unsigned long flags;
	bool drain;

	spin_lock_irqsave(&r->lock, flags);
	drain = !r->ready && r->pending && !r->fault;
	spin_unlock_irqrestore(&r->lock, flags);
	if (!drain)
		return -EINVAL;
	/*
	 * Threaded IRQ context only. Assert/status proves all descriptor reads
	 * have stopped before prepare clears pending and permits table reuse.
	 * The owner synchronizes this IRQ before shutdown or another modeset.
	 */
	return s7d_rdma_prepare(r);
}

int s7d_rdma_fini(struct s7d_rdma *r)
{
	/* Caller has quiesced the engine and synchronized its IRQ. */
	if (r->ready || r->pending || r->fault)
		return -EBUSY;
	if (r->table) {
		dma_free_coherent(r->dev, RDMA_TABLE_BYTES, r->table, r->dma);
		r->table = NULL;
	}
	return 0;
}
