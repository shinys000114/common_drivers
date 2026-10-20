/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VPU_H
#define __S7D_VPU_H

#include <linux/clk.h>
#include <linux/reset.h>

#include "s7d-crtc.h"
#include "s7d-afbc-engine.h"

struct platform_device;
struct regmap;

/* HDMI owns PHY/packets. Return only after its transmitter is safely off. */
struct s7d_vpu_link {
	int (*quiesce)(void *data);
	/* Prepare TX/branch clocks while VENC is stopped; PHY stays off. */
	int (*prepare)(void *data, unsigned long pixel_rate);
	void *data;
};

struct s7d_vpu {
	struct device *dev;
	struct s7d_vpu_link link;
	void __iomem *regs;
	struct regmap *regmap;
	struct clk_bulk_data clocks[4];
	struct clk_bulk_data pixel_route[4];
	struct clk *xtal;
	struct reset_control_bulk_data resets[3];
	struct reset_control_bulk_data local_resets[4];
	struct s7d_afbc_engine afbc;
	struct delayed_work afbc_timeout;
	unsigned long afbc_deadline;
	struct s7d_rdma rdma;
	struct s7d_scanout scanout;
	struct drm_crtc *crtc;
	spinlock_t frame_lock;
	int vsync_irq;
	int rdma_irq;
	atomic_t vsync_enabled;
	bool crc_requested;
	bool crc_armed;
	u8 crc_field;
	bool rdma_enabled;
	bool acquired;
	bool pixel_enabled;
	bool boot_held;
	bool touched;
	bool osd2_enable;
	bool afbc_available;
	bool gfcd_available;
	bool gfcd_enabled;
	bool gfcd_unchanged;
	bool gfcd_fault;
	u32 gfcd_hdr_saved;
	u32 afbc_ready_mask;
	u32 afbc_surface_mask;
	u32 afbc_unpack_mask;
	u8 normal_unpack_owned;
	bool afbc_enabled;
	bool afbc_replay;
	u64 afbc_replay_cookie;
	u64 afbc_replay_pending_cookie;
	u64 afbc_replay_expected_sequence;
	u64 afbc_replay_armed_ns;
	u64 afbc_replay_frame_ns;
	u8 afbc_replay_applied_field;
	bool afbc_replay_latch_pending;
	bool afbc_replay_apply_pending;
	bool afbc_replay_afbc_changed;
	int afbc_admission_error[2];
	u32 afbc_command_counter;
	bool video_enable[2];
	bool pixel_protected;
	bool core_protected;
	bool video_draining[2];
	bool osd_draining;
	u32 video_free_clk[2];
	u32 osd_free_clk[2];
	u32 osd_scaler_route_saved;
	bool osd_scaler_route_owned;
	u16 flip_start;
	u16 flip_end;
	u16 replay_prepare_end;
	u32 failed_reg;
};

extern const struct s7d_crtc_ops s7d_vpu_crtc_ops;

/*
 * Resource acquisition only: no register writes or reset at probe. Caller
 * sets DMA masks (streaming 36, coherent 32 bits) and enables runtime PM.
 * IRQs stay disabled until a CRTC is assigned and its first modeset starts.
 * A mandatory link callback stops the HDMI transmitter before any VPU
 * takeover/clock change, including first boot and asynchronous error recovery.
 * It must be idempotent; failure retains VPU references and scanout buffers.
 * Its data must outlive VPU shutdown. Do not rely on bridge atomic_pre_enable:
 * Linux 6.12 calls CRTC atomic_enable first.
 * No legacy VPU/RDMA writers or video/capture consumers may be active.
 */
int s7d_vpu_init(struct platform_device *pdev, struct s7d_vpu *vpu,
		 const struct s7d_vpu_link *link);
/* Retain the firmware clock/PM references until the first CRTC acquire. */
int s7d_vpu_hold_boot(struct s7d_vpu *vpu);
/* Only before first modeset, or after successful s7d_crtc_shutdown(). */
int s7d_vpu_fini(struct s7d_vpu *vpu);

#endif
