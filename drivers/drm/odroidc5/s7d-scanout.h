/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_SCANOUT_H
#define __S7D_SCANOUT_H

#include <linux/workqueue.h>

#include "s7d-rdma.h"
#include "s7d-afbc.h"
#include "s7d-csc.h"
#include "s7d-postblend.h"
#include "s7d-video-pipeline.h"

struct drm_framebuffer;

#define S7D_SCANOUT_MAX_PLANES 4

struct s7d_scanout_video {
	struct drm_framebuffer *fb;
	struct s7d_video_pipeline_state pipeline;
	struct s7d_csc_state csc;
	struct s7d_postblend_state postblend;
};

struct s7d_scanout_afbc {
	struct drm_framebuffer *fb;
	struct s7d_afbc_state plan;
};

struct s7d_scanout_buffers {
	struct drm_framebuffer *fb[S7D_SCANOUT_MAX_PLANES];
	/* video.fb aliases an owned fb[] reference. */
	struct s7d_scanout_video video;
	/* afbc.fb aliases an owned fb[] reference. */
	struct s7d_scanout_afbc afbc;
	u64 generation;
};

struct s7d_frame_state {
	u8 field;
	bool idle;
	bool early;
	bool afbc_gate;
	u64 afbc_completed_generation;
	u64 afbc_completed_epoch;
};

enum s7d_scanout_phase {
	S7D_SCANOUT_STOPPED,
	S7D_SCANOUT_INITIAL,
	S7D_SCANOUT_IDLE,
	S7D_SCANOUT_RDMA,
	S7D_SCANOUT_VBLANK,
};

struct s7d_scanout {
	struct s7d_rdma *rdma;
	/* Protects buffer sets, phase and fault against IRQ/work. */
	spinlock_t lock;
	struct work_struct retire_work;
	struct s7d_scanout_buffers active;
	struct s7d_scanout_buffers pending;
	struct s7d_scanout_buffers retired;
	enum s7d_scanout_phase phase;
	u64 vblank_seq;
	u64 generation;
	u64 pending_afbc_epoch;
	u8 applied_field;
	u8 vblank_field;
	bool vblank_valid;
	bool early_complete;
	bool wait_field;
	bool fault;
};

enum s7d_scanout_result {
	S7D_SCANOUT_NO_CHANGE,
	S7D_SCANOUT_FRAME_COMPLETE,
	S7D_SCANOUT_FAULT,
};

void s7d_scanout_init(struct s7d_scanout *scanout, struct s7d_rdma *rdma);
bool s7d_scanout_video_equal(const struct s7d_scanout_buffers *a,
			     const struct s7d_scanout_buffers *b);

/*
 * Serialize all process-context calls in the CRTC commit path, after waiting
 * for every plane's fences. Only IRQ handling may run concurrently. This owner
 * is for GEM DMA framebuffers: framebuffer references retain their backing
 * objects/import mappings. It does not replace any future pin/unpin API.
 *
 * Initial setup: take a reference BEFORE writing the OSD address. Call ready
 * only after programming and VENC start succeed. On any partial failure
 * call fail and keep resources until stop/drain succeeds.
 * Firmware scanout reservations are owned by the parent, not adopted here.
 */
int s7d_scanout_begin_initial(struct s7d_scanout *scanout,
			      const struct s7d_scanout_buffers *buffers);
int s7d_scanout_initial_ready(struct s7d_scanout *scanout);

/* One pending flip only. RDMA submit errors never arm the new list. */
int s7d_scanout_submit(struct s7d_scanout *scanout,
			const struct s7d_scanout_buffers *buffers,
			const struct s7d_rdma_entry *entries, unsigned int count,
			bool video_unchanged);

/* Flush before taking the backend frame lock. Staged callbacks cannot sleep. */
void s7d_scanout_flush_retired(struct s7d_scanout *scanout);
int s7d_scanout_submit_staged(struct s7d_scanout *scanout,
			     const struct s7d_scanout_buffers *buffers,
			     const struct s7d_rdma_entry *entries, unsigned int count,
			     bool video_unchanged,
			     int (*stage)(void *data,
				const struct s7d_scanout_buffers *active,
				const struct s7d_scanout_buffers *candidate,
				u64 generation),
			     int (*cancel)(void *data, u64 generation), void *data);
u64 s7d_scanout_pending_generation(struct s7d_scanout *scanout);
int s7d_scanout_afbc_started(struct s7d_scanout *scanout, u64 generation, u64 epoch);

/*
 * Feed each acknowledged RDMA result and VENC vblank exactly once, whether
 * delivered together or on separate IRQs. No framebuffer is put in IRQ.
 * The CRTC sends its event only on FRAME_COMPLETE. RDMA FAULT is sticky.
 *
 * Caller serializes sampling with delivery across all display IRQs. After
 * RDMA reset may finish inside the programmed FIFO hold window. Complete in
 * that field only with idle fetchers and an accounted matching vblank.
 * Otherwise require a different field and idle OSD/arbiter observations.
 */
enum s7d_scanout_result
s7d_scanout_irq(struct s7d_scanout *scanout, bool vblank,
		enum s7d_rdma_result rdma_result, const struct s7d_frame_state *frame);
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
