/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_GAMMA_H
#define __S7D_GAMMA_H

#include <linux/types.h>

#define S7D_GAMMA_LUT_SIZE 65
/* BIN_ADDR, 3 x 33 BIN_DATA words, CTRL. */
#define S7D_GAMMA_UPDATE_COUNT (1 + 3 * 33 + 1)

struct drm_property_blob;
struct s7d_rdma_entry;

/* Pure size validation; NULL requests pass-through. No resampling. */
int s7d_gamma_check(const struct drm_property_blob *lut);
/* False for NULL or a LUT that quantizes to the 10-bit hardware identity. */
bool s7d_gamma_active(const struct drm_property_blob *lut);

/*
 * Register writes that leave the LUT as requested, for CPU or RDMA: CTRL 0
 * alone, or the table and CTRL 3. Limited RGB always programs the LUT, with
 * identity outputs when the request has none. @update holds
 * S7D_GAMMA_UPDATE_COUNT entries. Returns the count or -errno.
 */
int s7d_gamma_build(const struct drm_property_blob *lut, bool limited,
		    struct s7d_rdma_entry *update);
bool s7d_gamma_update_valid(u32 reg, u32 value);
u32 s7d_gamma_reg_mask(u32 reg);

/*
 * Caller owns powered, stopped VPP; keep the LUT blob alive until return.
 * On error retain touched ownership and resources for stop/disable cleanup.
 */
int s7d_gamma_program(void __iomem *vcbus, const struct drm_property_blob *lut,
		      bool limited, u32 *failed_reg);
int s7d_gamma_disable(void __iomem *vcbus, u32 *failed_reg);

#endif
