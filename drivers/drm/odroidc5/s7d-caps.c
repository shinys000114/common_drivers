// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bitops.h>
#include <linux/errno.h>

#include "s7d-caps.h"

enum drm_mode_status s7d_display_mode_valid(const struct drm_display_mode *mode)
{
	if (mode->clock < S7D_DISPLAY_MIN_CLOCK)
		return MODE_CLOCK_LOW;
	if (mode->clock > S7D_DISPLAY_MAX_CLOCK)
		return MODE_CLOCK_HIGH;
	if (mode->hdisplay <= 0 || mode->hdisplay > S7D_DISPLAY_MAX_WIDTH ||
	    mode->htotal <= 0 || mode->htotal > S7D_DISPLAY_MAX_TOTAL)
		return MODE_BAD_HVALUE;
	if (mode->vdisplay <= 0 || mode->vdisplay > S7D_DISPLAY_MAX_HEIGHT ||
	    mode->vtotal <= 0 || mode->vtotal > S7D_DISPLAY_MAX_TOTAL)
		return MODE_BAD_VVALUE;
	/* The selected S7D transmitter supports FHD120 and UHD60. */
	if (drm_mode_vrefresh(mode) > (mode->vdisplay > 1080 ? 60 : 120))
		return MODE_VSYNC;
	return MODE_OK;
}

int s7d_display_check_resources(const struct s7d_display_resources *r)
{
	if ((r->inputs & ~GENMASK(S7D_INPUT_COUNT - 1, 0)) ||
	    ((r->afbc | r->gfcd | r->scalers) & ~r->inputs) ||
	    (r->afbc & ~S7D_OSD_INPUTS) || (r->scalers & BIT(S7D_INPUT_VD2)))
		return -EINVAL;
	/* The native path carries one compressed plan and three postblend inputs. */
	if (hweight8(r->afbc) > 1 || hweight8(r->inputs) > 3)
		return -ENOSPC;
	if (r->gfcd && (r->gfcd != BIT(S7D_INPUT_OSD1) || r->afbc ||
			(r->inputs & S7D_VIDEO_INPUTS) ||
			(r->scalers & BIT(S7D_INPUT_OSD1))))
		return -EINVAL;
	if (r->afbc && (r->inputs & BIT(S7D_INPUT_VD2)))
		return -EINVAL;
	if (r->afbc && (r->scalers & BIT(S7D_INPUT_VD1)) &&
	    (r->scalers & S7D_OSD_INPUTS))
		return -EINVAL;
	return 0;
}

int s7d_display_claim(struct s7d_display_resources *resources,
		      enum s7d_display_input input, enum s7d_display_fetch fetch)
{
	struct s7d_display_resources next = *resources;
	int ret;

	if ((unsigned int)input >= S7D_INPUT_COUNT)
		return -EINVAL;
	if (next.inputs & BIT(input))
		return -ENOSPC;
	next.inputs |= BIT(input);
	switch (fetch) {
	case S7D_FETCH_LINEAR:
		break;
	case S7D_FETCH_AFBC:
		next.afbc |= BIT(input);
		break;
	case S7D_FETCH_GFCD:
		next.gfcd |= BIT(input);
		break;
	default:
		return -EINVAL;
	}
	ret = s7d_display_check_resources(&next);
	if (!ret)
		*resources = next;
	return ret;
}
