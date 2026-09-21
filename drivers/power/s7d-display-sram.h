/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_DISPLAY_SRAM_H
#define __S7D_DISPLAY_SRAM_H

#include <linux/errno.h>
#include <linux/io.h>

#define S7D_VPU_SRAM_BANKS 5
#define S7D_VPU_SRAM_BYTES (S7D_VPU_SRAM_BANKS * sizeof(u32))

/*
 * MEM_PD5..9 are the documented VPU SRAM banks. The first display profile
 * retains all five banks ON: do not infer individual S7D bits from SC2's table.
 * MEM_PD10/11 and their disputed HDMI assignment are deliberately excluded.
 * The provider owns the mapping. This only verifies retained state and never
 * writes memory-power controls or pretends to implement cold power-up.
 */
static inline int s7d_display_sram_check(void __iomem *banks, unsigned int *failed)
{
	unsigned int i;

	if (!banks)
		return -ENODEV;
	for (i = 0; i < S7D_VPU_SRAM_BANKS; i++) {
		if (readl(banks + i * sizeof(u32))) {
			if (failed)
				*failed = i + 5;
			return -EIO;
		}
	}
	return 0;
}

#endif
