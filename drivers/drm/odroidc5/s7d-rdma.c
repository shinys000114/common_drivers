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
#include <linux/slab.h>
#include <linux/string.h>

#include "s7d-rdma.h"
#include "s7d-gfcd.h"
#include "s7d-csc.h"
#include "s7d-osd-pipeline.h"

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
#define AFBC_CLEAR		0x3a02
#define AFBC_COMMAND		0x3a05

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
	r->staged = kmalloc(RDMA_TABLE_BYTES, GFP_KERNEL);
	if (!r->staged) {
		dma_free_coherent(dev, RDMA_TABLE_BYTES, r->table, r->dma);
		r->table = NULL;
		return -ENOMEM;
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
	r->loop = false;
	r->loop_cookie = 0;
	r->staged_count = 0;
	r->staged_cookie = 0;
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
		r->vd1_free_clk = rdma_read(r, 0x4800) & BIT(31);
		r->vd2_free_clk = rdma_read(r, 0x4880) & BIT(31);
		r->fault = false;
		r->ready = true;
	}
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

static bool display_write_valid(u32 reg, u32 value)
{
	u32 mask = s7d_csc_reg_mask(reg);

	if (mask)
		return !(value & ~mask);
	switch (reg) {
	case S7D_GFCD_HEAD:
		return IS_ENABLED(CONFIG_AMLOGIC_C5_GFCD) && value && !(value & 3);
	case 0x1a1b:
	case 0x1a3b:
		value &= ~GENMASK(29, 28);
		return (value & ~0xcU) == 0x8500 || value == 0x8408 ||
		       value == 0x8700 || value == 0x8714;
	case 0x1a14:
	case 0x1a65:
		return true;
	case 0x1a15:
	case 0x1a66:
		return value && value <= 0xfff && !(value & 3);
	case 0x1a1c:
	case 0x1a1d:
	case 0x1a3c:
	case 0x1a3d:
	case 0x39b3:
	case 0x39b4:
	case 0x1df5:
	case 0x1df6:
	case 0x1df7:
	case 0x1df8:
	case 0x1d1c:
	case 0x1d1d:
	case 0x1d1e:
	case 0x1d1f:
	case 0x4883:
	case 0x4884:
	case 0x4885:
	case 0x4886:
	case 0x4803:
	case 0x4804:
	case 0x4805:
	case 0x4806:
		return !(value & ~0x1fff1fffU);
	case 0x1a3e:
	case 0x1a64:
		return !(value & ~0x0fff0fffU);
	case 0x1a2f:
	case 0x1abd:
		return !value || value == BIT(28);
	case 0x1a2d:
	case 0x1a4d:
		return value == 0x7fc0 || value == 0x7fc1 || value == BIT(2);
	case 0x1a30:
		return value == 0x00100004 || value == 0x00100005;
	case 0x39ba:
		return !(value & ~GENMASK(28, 11)) &&
		       (value >> 20) <= 256 && ((value >> 11) & 0x1ff) <= 256;
	case 0x39b0:
		value |= BIT(17) | BIT(19);
		return value == (0x807f4413 | BIT(25)) ||
		       value == (0x80ff2413 | BIT(25));
	case 0x1d1a:
	case 0x1d1b:
		return value < 4096;
	case 0x1d20:
		return (value >> 16) && (value >> 16) <= 4096 &&
		       (value & 0xffff) && (value & 0xffff) <= 4096;
	case 0x1d76:
		return value <= 256;
	case 0x1d26:
		return !(value & ~(GENMASK(26, 18) | BIT(7) | BIT(6) | BIT(1))) &&
		       (value & (BIT(7) | BIT(6))) == (BIT(7) | BIT(6)) &&
		       (value >> 18) <= 256;
	case 0x1dfb:
		return (value & BIT(16)) &&
		       !(value & ~(BIT(16) | GENMASK(10, 8) | BIT(4) | BIT(0))) &&
		       ((value >> 8) & 7) <= 4 &&
		       (!(value & 0xff) || (value & 0xff) == 0x11);
	case 0x1dfc:
		return !(value & ~(BIT(20) | GENMASK(10, 8))) &&
		       ((value >> 8) & 7) <= 4;
	case 0x1dfd:
	case 0x1dfe:
		return (value & BIT(20)) &&
		       !(value & ~(BIT(20) | GENMASK(10, 8))) &&
		       ((value >> 8) & 7) <= 4;
	case 0x4880:
	case 0x4800:
		return value == 0x38400052 || value == 0x38400053 ||
		       value == 0x38404050 || value == 0x38404051;
	case 0x4899:
	case 0x4819:
		return !(value & ~GENMASK(5, 0)) && (value & 3) <= 2 &&
		       !!(value & BIT(2)) == !!(value & BIT(4)) &&
		       !!(value & BIT(3)) == !!(value & BIT(5));
	case 0x48a0:
	case 0x48a1:
	case 0x4820:
	case 0x4821:
		return true;
	case 0x48a3:
	case 0x4823:
		return !(value & ~0x1fff1fffU) && (value & 0x1fff);
	case 0x48a4:
	case 0x4824:
		return !(value & ~0x11fffU) && (value & BIT(16));
	case 0x489e:
		return (value >> 16) >= 2 && (value >> 16) <= 2048 &&
		       !(value & BIT(16)) && (value & 0xffff) == (value >> 17);
	case 0x1df0:
		return (value >> 16) >= 2 && (value >> 16) <= 1080 &&
		       (value & 0xffff) >= 2 && (value & 0xffff) <= 2048 &&
		       !(value & 0x10001);
	case 0x481e:
		return (value >> 16) >= 2 && (value >> 16) <= 4096 &&
		       !(value & BIT(16)) && (value & 0xffff) == (value >> 17);
	case 0x1d01:
		return value >= 2 && value <= 4096 && !(value & BIT(0));
	case 0x1d02:
		return value >= 2 && value <= 2160 && !(value & BIT(0));
	default:
		return false;
	}
}

int s7d_rdma_check_idle(struct s7d_rdma *r)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&r->lock, flags);
	ret = !r->ready || r->fault ? -EIO : (r->pending ? -EBUSY : 0);
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

		/* Replays before CPU masking must remain idempotent. */
		if (!display_write_valid(reg, value))
			return -EINVAL;
	}

	spin_lock_irqsave(&r->lock, flags);
	if (!r->ready || r->fault) {
		ret = -EIO;
		goto out;
	}
	if (r->pending || r->loop || r->staged_count) {
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
	 * S7D executes the payload but does not self-mask RDMA_SRC1 from a
	 * descriptor. Keep this immutable list until CPU masking and a verified
	 * engine reset in the threaded IRQ. Delayed IRQs may replay these same
	 * idempotent values; they must never fetch a reused table or old buffer.
	 */
	memcpy(r->table, entries, count * sizeof(*entries));
	for (i = 0; i < count; i++) {
		u32 reg = le32_to_cpu(entries[i].reg);
		u32 value = le32_to_cpu(entries[i].value);
		u32 mask = s7d_csc_reg_mask(reg);

		if (!mask)
			mask = s7d_osd_unpack_mask(reg);
		if (mask)
			value |= rdma_read(r, reg) & ~mask;
		else if (reg == 0x4800)
			value |= r->vd1_free_clk;
		else if (reg == 0x4880)
			value |= r->vd2_free_clk;
		r->table[i].value = cpu_to_le32(value);
	}
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

int s7d_rdma_loop_stage(struct s7d_rdma *r,
			const struct s7d_rdma_entry *entries, unsigned int count,
			unsigned int surface, u64 header,
			enum s7d_rdma_afbc_command command, u64 cookie)
{
	unsigned long flags;
	unsigned int i;
	u32 bank;
	int ret = 0;

	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		return -EOPNOTSUPP;
	if ((!entries && count) || count > RDMA_MAX_ENTRIES - S7D_RDMA_AFBC_LOOP_ENTRIES ||
	    surface > 1 || !cookie ||
	    (command != S7D_RDMA_AFBC_DIRECT && command != S7D_RDMA_AFBC_PENDING) ||
	    !IS_ALIGNED(header, 64) ||
	    header > GENMASK_ULL(35, 0))
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (!display_write_valid(le32_to_cpu(entries[i].reg),
					le32_to_cpu(entries[i].value)))
			return -EINVAL;

	spin_lock_irqsave(&r->lock, flags);
	if (!r->ready || r->fault || !r->staged) {
		ret = -EIO;
		goto out;
	}
	if (r->staged_count || (r->pending && !r->loop)) {
		ret = -EBUSY;
		goto out;
	}
	if (cookie <= r->cookie_floor) {
		ret = -EINVAL;
		goto out;
	}
	if (count)
		memcpy(r->staged, entries, count * sizeof(*entries));
	bank = surface ? 0x3a30 : 0x3a10;
	r->staged[count] = (struct s7d_rdma_entry) {
		.reg = cpu_to_le32(bank),
		.value = cpu_to_le32(lower_32_bits(header)),
	};
	r->staged[count + 1] = (struct s7d_rdma_entry) {
		.reg = cpu_to_le32(bank + 1),
		.value = cpu_to_le32(upper_32_bits(header)),
	};
	r->staged[count + 2] = (struct s7d_rdma_entry) {
		.reg = cpu_to_le32(AFBC_CLEAR),
		.value = cpu_to_le32(BIT(1)),
	};
	r->staged[count + 3] = (struct s7d_rdma_entry) {
		.reg = cpu_to_le32(AFBC_COMMAND),
		.value = cpu_to_le32(command),
	};
	r->staged_cookie = cookie;
	r->cookie_floor = cookie;
	r->staged_count = count + S7D_RDMA_AFBC_LOOP_ENTRIES;
out:
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

int s7d_rdma_loop_cancel(struct s7d_rdma *r)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&r->lock, flags);
	if (!r->ready || r->fault) {
		ret = -EIO;
		goto out;
	}
	r->staged_count = 0;
	r->staged_cookie = 0;
out:
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

int s7d_rdma_loop_check_stage(struct s7d_rdma *r)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&r->lock, flags);
	if (!r->ready || r->fault || !r->staged)
		ret = -EIO;
	else if (!r->loop || !r->loop_cookie)
		ret = -EINVAL;
	else
		ret = r->staged_count ? -EBUSY : 0;
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

bool s7d_rdma_loop_can_replace(struct s7d_rdma *r)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&r->lock, flags);
	ready = r->ready && !r->fault && r->loop && r->loop_cookie &&
		!r->pending && r->staged_count;
	spin_unlock_irqrestore(&r->lock, flags);
	return ready;
}

int s7d_rdma_loop_replace(struct s7d_rdma *r,
			  int (*freshness)(void *data), void *data)
{
	unsigned long flags;
	unsigned int i, count;
	u32 start, end;
	int ret = -EIO;

	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		return -EOPNOTSUPP;
	if (!freshness)
		return -EINVAL;
	spin_lock_irqsave(&r->lock, flags);
	if (!r->ready || r->fault)
		goto out;
	if (!r->staged_count) {
		ret = -ENOENT;
		goto out;
	}
	if (r->pending) {
		ret = -EBUSY;
		goto out;
	}

	r->ready = false;
	r->fault = true;
	rdma_write(r, RDMA_SRC1, 0);
	if (rdma_read(r, RDMA_SRC1))
		goto out;
	ret = reset_control_assert(r->reset);
	if (ret)
		goto out;
	ret = reset_control_status(r->reset);
	if (ret != 1) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	ret = reset_control_deassert(r->reset);
	if (ret)
		goto out;
	ret = reset_control_status(r->reset);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	ret = -EIO;
	for (i = 0; i < 7; i++) {
		rdma_write(r, RDMA_SRC1 + i, 0);
		if (rdma_read(r, RDMA_SRC1 + i))
			goto out;
	}
	rdma_write(r, RDMA_ACCESS_AUTO, RDMA_WRITE1);
	rdma_write(r, RDMA_ACCESS_AUTO2, 0);
	rdma_write(r, RDMA_ACCESS_MAN, 0);
	if (rdma_read(r, RDMA_ACCESS_AUTO) != RDMA_WRITE1 ||
	    rdma_read(r, RDMA_ACCESS_AUTO2) || rdma_read(r, RDMA_ACCESS_MAN))
		goto out;
	rdma_write(r, RDMA_CTRL, RDMA_CONFIG | GENMASK(31, 24));
	if (rdma_read(r, RDMA_STATUS) & GENMASK(31, 24))
		goto out;
	ret = freshness(data);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}

	count = r->staged_count;
	memcpy(r->table, r->staged, count * sizeof(*r->table));
	for (i = 0; i < count - S7D_RDMA_AFBC_LOOP_ENTRIES; i++) {
		u32 reg = le32_to_cpu(r->staged[i].reg);
		u32 value = le32_to_cpu(r->staged[i].value);
		u32 mask = s7d_csc_reg_mask(reg);

		if (!mask)
			mask = s7d_osd_unpack_mask(reg);
		if (mask)
			value |= rdma_read(r, reg) & ~mask;
		else if (reg == 0x4800)
			value |= r->vd1_free_clk;
		else if (reg == 0x4880)
			value |= r->vd2_free_clk;
		r->table[i].value = cpu_to_le32(value);
	}
	dma_wmb();
	start = lower_32_bits(r->dma);
	end = start + count * sizeof(*r->table) - 1;
	rdma_write(r, RDMA_START, start);
	rdma_write(r, RDMA_END, end);
	ret = -EIO;
	if (rdma_read(r, RDMA_START) != start || rdma_read(r, RDMA_END) != end)
		goto out;
	r->loop_cookie = r->staged_cookie;
	r->loop = true;
	r->pending = true;
	rdma_write(r, RDMA_SRC1, BIT(0));
	if (rdma_read(r, RDMA_SRC1) != BIT(0))
		goto out;
	r->staged_count = 0;
	r->staged_cookie = 0;
	r->fault = false;
	r->ready = true;
	ret = 0;
out:
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

static enum s7d_rdma_result rdma_irq(struct s7d_rdma *r, u64 *cookie)
{
	enum s7d_rdma_result result = S7D_RDMA_NO_IRQ;
	unsigned long flags;
	u32 source, status;

	if (cookie)
		*cookie = 0;
	spin_lock_irqsave(&r->lock, flags);
	/* Do not access a block held in reset or undergoing prepare/quiesce. */
	if (!r->ready)
		goto out;
	if (!(rdma_read(r, RDMA_STATUS) & RDMA_DONE1))
		goto out;
	rdma_write(r, RDMA_CTRL, RDMA_CONFIG | RDMA_DONE1);
	if (r->loop) {
		if (r->fault || !cookie || !r->loop_cookie ||
		    rdma_read(r, RDMA_SRC1) != BIT(0))
			goto fault;
		*cookie = r->loop_cookie;
		result = r->pending ? S7D_RDMA_APPLIED : S7D_RDMA_REPLAY;
		r->pending = false;
		goto out;
	}
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

enum s7d_rdma_result s7d_rdma_irq(struct s7d_rdma *r)
{
	return rdma_irq(r, NULL);
}

enum s7d_rdma_result s7d_rdma_loop_irq(struct s7d_rdma *r, u64 *cookie)
{
	if (!cookie)
		return S7D_RDMA_FAULT;
	return rdma_irq(r, cookie);
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
	if (r->ready || r->pending || r->fault || r->loop || r->staged_count)
		return -EBUSY;
	if (r->table) {
		dma_free_coherent(r->dev, RDMA_TABLE_BYTES, r->table, r->dma);
		r->table = NULL;
	}
	kfree(r->staged);
	r->staged = NULL;
	return 0;
}
