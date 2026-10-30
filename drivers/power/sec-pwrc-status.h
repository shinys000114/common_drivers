/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __MESON_SEC_PWRC_STATUS_H
#define __MESON_SEC_PWRC_STATUS_H

#include <linux/errno.h>
#include <linux/types.h>

/* SMCCC errors may be sign-extended or zero-extended by 32-bit services. */
static inline int meson_pwrc_error(unsigned long result)
{
	return (s32)result == -1 ? -EOPNOTSUPP : -EIO;
}

static inline int meson_pwrc_set_result(unsigned long result)
{
	return result ? meson_pwrc_error(result) : 0;
}

static inline int meson_pwrc_get_result(unsigned long result)
{
	/* GET returns 0 for ON and 1 for OFF; these are not SET results. */
	return result <= 1 ? (int)result : meson_pwrc_error(result);
}

#endif
