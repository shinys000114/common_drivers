/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_RDMA_H
#define __S7D_RDMA_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct device;
struct reset_control;

#define S7D_RDMA_AFBC_LOOP_ENTRIES 4

struct s7d_rdma_entry {
	__le32 reg;
	__le32 value;
};

struct s7d_rdma {
	struct device *dev;
	void __iomem *vcbus;
	struct reset_control *reset;
	struct s7d_rdma_entry *table;
	struct s7d_rdma_entry *staged;
	dma_addr_t dma;
	/* Protects ready/pending/fault, the descriptor table and MMIO vs IRQ. */
	spinlock_t lock;
	u32 vd1_free_clk;
	u32 vd2_free_clk;
	unsigned int staged_count;
	u64 staged_cookie;
	u64 loop_cookie;
	u64 cookie_floor;
	bool ready;
	bool pending;
	bool fault;
	bool loop;
};

enum s7d_rdma_result {
	S7D_RDMA_NO_IRQ,
	S7D_RDMA_STALE_IRQ,
	S7D_RDMA_COMPLETE,
	S7D_RDMA_NEEDS_DRAIN,
	S7D_RDMA_FAULT,
	S7D_RDMA_APPLIED,
	S7D_RDMA_REPLAY,
};

enum s7d_rdma_afbc_command {
	S7D_RDMA_AFBC_DIRECT = 1,
	S7D_RDMA_AFBC_PENDING = 2,
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
int s7d_rdma_check_idle(struct s7d_rdma *rdma);
int s7d_rdma_submit(struct s7d_rdma *rdma, const struct s7d_rdma_entry *entries,
		    unsigned int count);
enum s7d_rdma_result s7d_rdma_irq(struct s7d_rdma *rdma);
/* Only after NEEDS_DRAIN, from the serialized threaded IRQ. May sleep. */
int s7d_rdma_finish(struct s7d_rdma *rdma);

/*
 * Loop mode requires the exclusive S7D MMIO reset provider and non-RT kernel.
 * Stage only prepares CPU memory; entries remain the ordinary display list.
 * The backend adds the header pair, swap acknowledgement and exact command 1/2.
 * The owner validates the AFBC layout, holds its buffers and supplies a
 * strictly increasing nonzero submission cookie, separate from buffer generation.
 * Cancellation/quiesce do not permit cookie reuse. A running immutable list
 * never changes its command.
 */
int s7d_rdma_loop_stage(struct s7d_rdma *rdma,
			const struct s7d_rdma_entry *entries, unsigned int count,
			unsigned int surface, u64 header,
			enum s7d_rdma_afbc_command command, u64 cookie);
int s7d_rdma_loop_cancel(struct s7d_rdma *rdma);
/* CPU-only staging; the current loop's first DONE may still be pending. */
int s7d_rdma_loop_check_stage(struct s7d_rdma *rdma);
bool s7d_rdma_loop_can_replace(struct s7d_rdma *rdma);
/*
 * Serialized with modeset/shutdown. freshness runs with the lock held, IRQs off,
 * after verified reset and zero sources, before publication. It cannot sleep
 * or reenter RDMA. Failure preserves fault/custody until successful quiesce.
 * Pending first application/one-shot completion must be consumed before replace.
 */
int s7d_rdma_loop_replace(struct s7d_rdma *rdma,
			  int (*freshness)(void *data), void *data);
/*
 * Loop consumers use this instead of irq(): cookie is captured under the same
 * lock as APPLIED/REPLAY. Verify it under the VPU frame lock before completion.
 * APPLIED means list completion, not AFBC swap/readout completion.
 * Non-loop IRQ handling is unchanged and returns cookie zero.
 */
enum s7d_rdma_result s7d_rdma_loop_irq(struct s7d_rdma *rdma, u64 *cookie);

/*
 * Serialize prepare/quiesce in the modeset path. Quiesce cancels a pending
 * list, it does not complete a page flip. On error retain DMA and scanout
 * references. Successful quiesce leaves reset asserted. Before fini, quiesce
 * successfully and synchronize the IRQ. Serialize fini against all API calls.
 */
int s7d_rdma_quiesce(struct s7d_rdma *rdma);
int s7d_rdma_fini(struct s7d_rdma *rdma);

#endif
