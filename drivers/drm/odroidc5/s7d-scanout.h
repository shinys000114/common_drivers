/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_SCANOUT_H
#define __S7D_SCANOUT_H

#include <linux/workqueue.h>

#include "s7d-rdma.h"

struct drm_framebuffer;

enum s7d_scanout_phase {
	S7D_SCANOUT_STOPPED,
	S7D_SCANOUT_INITIAL,
	S7D_SCANOUT_IDLE,
	S7D_SCANOUT_RDMA,
	S7D_SCANOUT_VBLANK,
};

struct s7d_scanout {
	struct s7d_rdma *rdma;
	/* Protects framebuffer pointers, phase and fault against IRQ/work. */
	spinlock_t lock;
	struct work_struct retire_work;
	struct drm_framebuffer *active_fb;
	struct drm_framebuffer *pending_fb;
	struct drm_framebuffer *retired_fb;
	enum s7d_scanout_phase phase;
	u64 vblank_seq;
	unsigned int vblanks_left;
	bool fault;
};

enum s7d_scanout_result {
	S7D_SCANOUT_NO_CHANGE,
	S7D_SCANOUT_FRAME_COMPLETE,
	S7D_SCANOUT_FAULT,
};

void s7d_scanout_init(struct s7d_scanout *scanout, struct s7d_rdma *rdma);

/*
 * Serialize all process-context calls in the CRTC commit path, after waiting
 * for the plane's fences. Only IRQ handling may run concurrently. This owner
 * is for GEM DMA framebuffers: framebuffer references retain their backing
 * objects/import mappings. It does not replace any future pin/unpin API.
 *
 * Initial setup: take a reference BEFORE writing the OSD address. Call ready
 * after programming succeeds, with VENC still stopped, then enable VENC. On
 * any partial failure call fail and keep resources until stop/drain succeeds.
 * Firmware scanout reservations are owned by the parent, not adopted here.
 */
int s7d_scanout_begin_initial(struct s7d_scanout *scanout,
			      struct drm_framebuffer *fb);
int s7d_scanout_initial_ready(struct s7d_scanout *scanout);

/* One pending flip only. RDMA submit errors never arm the new list. */
int s7d_scanout_submit(struct s7d_scanout *scanout, struct drm_framebuffer *fb,
			const struct s7d_rdma_entry *entries, unsigned int count);

/*
 * Feed each acknowledged RDMA result and VENC vblank exactly once, whether
 * delivered together or on separate IRQs. No framebuffer is put in IRQ.
 * The CRTC sends its event only on FRAME_COMPLETE. RDMA FAULT is sticky.
 *
 * Wait two observed vblanks after RDMA completion because separate IRQs can
 * be serviced in either order for the same physical vsync. This deliberately
 * delays retirement/events until a later frame; it is not a hardware DMA
 * drain proof. The RDMA backend must first stop triggers and verify reset
 * in threaded IRQ context before reporting COMPLETE. OSD latch/fetch timing
 * still needs silicon validation.
 */
enum s7d_scanout_result
s7d_scanout_irq(struct s7d_scanout *scanout, bool vblank,
		enum s7d_rdma_result rdma_result);
void s7d_scanout_fail(struct s7d_scanout *scanout);

/*
 * Stop AND drain OSD/other fetchers, then mask and synchronize display IRQs
 * before calling this, with clocks/PM held. In particular, no previously
 * captured RDMA result may be delivered after quiesce has returned.
 * Quiesce verifies RDMA reset; on failure retain current/pending references.
 * On success release them in process context. Keep the owner, RDMA table,
 * mappings and PM/clock references alive until this succeeds. Before fini,
 * synchronize IRQs and prevent further commits; fini refuses a live owner.
 */
int s7d_scanout_quiesce(struct s7d_scanout *scanout);
int s7d_scanout_fini(struct s7d_scanout *scanout);

#endif
