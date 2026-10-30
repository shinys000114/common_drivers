/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef _LINUX_S7D_HDMI_PHY_H
#define _LINUX_S7D_HDMI_PHY_H

struct phy;

/* Return 1 for active lanes, 0 for inactive lanes, or a negative errno. */
int s7d_hdmi_phy_is_enabled(struct phy *phy);

#endif
