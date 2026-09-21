/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_VPP_H
#define __S7D_VPP_H

#include <linux/types.h>

/*
 * Configure the unscaled OSD1 -> postblend SDR RGB path. This is a modeset
 * operation, NEVER an atomic_check or probe operation. Caller owns the VPU,
 * has quiesced all RDMA/other writers and fetchers, and holds its clocks/PM
 * references. All VENCs and OSD1 must already be disabled. SRAM and AXI
 * arbitration must be ready; this does not control them or any CCF gates.
 *
 * Call before enabling OSD1/ENCP. The bridge must accept the resulting RGB
 * bus and configure depth/range/packets separately.
 *
 * On a readback mismatch return -EIO, with its word index in failed_reg if
 * provided. Partial configuration is not rolled back. VENC remains disabled;
 * the caller must stop the commit and retain buffers until DMA is drained.
 * A zero return is configuration readback, not a displayed-frame completion.
 */
int s7d_vpp_setup(void __iomem *vcbus, u32 width, u32 height, u32 *failed_reg);

#endif
