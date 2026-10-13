/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_RDMA_H
#define __S7D_RDMA_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct device;
struct reset_control;

struct s7d_rdma_entry {
	__le32 reg;
	__le32 value;
};

struct s7d_rdma {
	struct device *dev;
	void __iomem *vcbus;
	struct reset_control *reset;
	struct s7d_rdma_entry *table;
	dma_addr_t dma;
	/* Protects ready/pending/fault, the descriptor table and MMIO vs IRQ. */
	spinlock_t lock;
	bool ready;
	bool pending;
	bool fault;
};

enum s7d_rdma_result {
	S7D_RDMA_NO_IRQ,
	S7D_RDMA_STALE_IRQ,
	S7D_RDMA_COMPLETE,
	S7D_RDMA_NEEDS_DRAIN,
	S7D_RDMA_FAULT,
};

/*
 * VPU owns the entire RDMA engine, reset and MMIO window. Its clocks/PM must
 * remain held for hardware accesses. Legacy RDMA consumers must be absent.
 * The reset must be acquired exclusively. Init only allocates memory:
 * prepare resets the engine at the first Linux modeset, after the caller
 * has stopped its trigger sources (including firmware VENC). The parent
 * must set a 32-bit coherent DMA mask; scanout DMA can have a separate mask.
 */
int s7d_rdma_init(struct s7d_rdma *rdma, struct device *dev, void __iomem *vcbus,
		  struct reset_control *reset);
int s7d_rdma_prepare(struct s7d_rdma *rdma);
int s7d_rdma_submit(struct s7d_rdma *rdma, const struct s7d_rdma_entry *entries,
		    unsigned int count);
enum s7d_rdma_result s7d_rdma_irq(struct s7d_rdma *rdma);
/* Only after NEEDS_DRAIN, from the serialized threaded IRQ. May sleep. */
int s7d_rdma_finish(struct s7d_rdma *rdma);

/*
 * Serialize prepare/quiesce in the modeset path. Quiesce cancels a pending
 * list, it does not complete a page flip. On error retain DMA and scanout
 * references. Successful quiesce leaves reset asserted. Before fini, quiesce
 * successfully and synchronize the IRQ. Serialize fini against all API calls.
 */
int s7d_rdma_quiesce(struct s7d_rdma *rdma);
int s7d_rdma_fini(struct s7d_rdma *rdma);

#endif
