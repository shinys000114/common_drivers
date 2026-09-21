/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef _MALI_KBASE_C5_CONFIG_H_
#define _MALI_KBASE_C5_CONFIG_H_

#define POWER_MANAGEMENT_CALLBACKS (&c5_pm_callbacks)
#define PLATFORM_FUNCS (&c5_platform_funcs)
#define CLK_RATE_TRACE_OPS (&clk_rate_trace_ops)

extern struct kbase_pm_callback_conf c5_pm_callbacks;
extern struct kbase_platform_funcs_conf c5_platform_funcs;
extern struct kbase_clk_rate_trace_op_conf clk_rate_trace_ops;

#endif
