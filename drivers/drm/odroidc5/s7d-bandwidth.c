// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/overflow.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>

#include "s7d-bandwidth.h"
#include "s7d-rdma.h"

static int s7d_bandwidth_bytes(const struct s7d_bandwidth_input *input,
			      struct s7d_bandwidth_fetch *fetch)
{
	const struct drm_framebuffer *fb = input->fb;
	const struct drm_format_info *format = fb->format;
	u32 width, height;
	unsigned int i;

	if (!format || !format->num_planes || format->num_planes > DRM_FORMAT_MAX_PLANES ||
	    input->src.x1 < 0 || input->src.y1 < 0 ||
	    input->src.x2 <= input->src.x1 || input->src.y2 <= input->src.y1 ||
	    ((input->src.x1 | input->src.y1 | input->src.x2 | input->src.y2) & 0xffff) ||
	    (u32)(input->src.x2 >> 16) > fb->width || (u32)(input->src.y2 >> 16) > fb->height)
		return -EINVAL;
	width = drm_rect_width(&input->src) >> 16;
	height = drm_rect_height(&input->src) >> 16;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR) {
		if (!input->compressed_size)
			return -EINVAL;
		fetch->payload_bytes = (u64)width * height * 4;
		fetch->read_bytes = input->compressed_size;
		return 0;
	}
	if (input->compressed_size)
		return -EINVAL;
	for (i = 0; i < format->num_planes; i++) {
		u32 hsub = i ? format->hsub : 1;
		u32 vsub = i ? format->vsub : 1;
		u32 cpp = format->cpp[i];
		u64 line;

		if (!cpp || !hsub || !vsub ||
		    (input->src.x1 >> 16) % hsub || (input->src.y1 >> 16) % vsub ||
		    width % hsub || height % vsub)
			return -EINVAL;
		line = (u64)(width / hsub) * cpp;
		if (line > fb->pitches[i])
			return -EINVAL;
		fetch->payload_bytes += line * (height / vsub);
		/* Full rows cover padding and partial edge bursts without a DDR ratio. */
		fetch->read_bytes += (u64)fb->pitches[i] * (height / vsub);
	}
	return 0;
}

static int s7d_bandwidth_rate(u64 bytes, u64 pixel_rate, u64 pixels, u64 *rate)
{
	u64 scaled;

	if (check_mul_overflow(bytes, pixel_rate, &scaled) ||
	    check_add_overflow(scaled, pixels - 1, &scaled))
		return -ERANGE;
	*rate = div64_u64(scaled, pixels);
	return 0;
}

int s7d_bandwidth_build(const struct drm_display_mode *mode,
			const struct s7d_bandwidth_input inputs[S7D_INPUT_COUNT],
			unsigned int rdma_entries, struct s7d_bandwidth_state *state)
{
	struct s7d_bandwidth_state next = {0};
	u64 pixel_rate, frame_pixels;
	unsigned int i;
	int ret;

	if (!mode || !inputs || !state || s7d_display_mode_valid(mode) != MODE_OK ||
	    mode->hdisplay > mode->htotal || mode->vdisplay > mode->vtotal ||
	    (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)) || mode->vscan > 1)
		return -EINVAL;
	pixel_rate = (u64)mode->clock * 1000;
	frame_pixels = (u64)mode->htotal * mode->vtotal;
	next.rdma_bytes = (u64)rdma_entries * sizeof(struct s7d_rdma_entry);
	next.read_bytes = next.rdma_bytes;
	ret = s7d_bandwidth_rate(next.rdma_bytes, pixel_rate, frame_pixels, &next.window_bps);
	if (ret)
		return ret;
	for (i = 0; i < S7D_INPUT_COUNT; i++) {
		const struct s7d_bandwidth_input *input = &inputs[i];
		struct s7d_bandwidth_fetch *fetch = &next.input[i];
		u32 height;

		if (!input->fb)
			continue;
		if (input->dst.x1 < 0 || input->dst.y1 < 0 ||
		    input->dst.x2 <= input->dst.x1 || input->dst.y2 <= input->dst.y1 ||
		    input->dst.x2 > mode->hdisplay || input->dst.y2 > mode->vdisplay)
			return -EINVAL;
		height = drm_rect_height(&input->dst);
		ret = s7d_bandwidth_bytes(input, fetch);
		if (ret)
			return ret;
		ret = s7d_bandwidth_rate(fetch->read_bytes, pixel_rate, frame_pixels,
					&fetch->average_bps);
		if (ret)
			return ret;
		ret = s7d_bandwidth_rate(fetch->read_bytes, pixel_rate,
					(u64)mode->htotal * height, &fetch->window_bps);
		if (ret)
			return ret;
		next.payload_bytes += fetch->payload_bytes;
		next.read_bytes += fetch->read_bytes;
		next.window_bps += fetch->window_bps;
	}
	ret = s7d_bandwidth_rate(next.read_bytes, pixel_rate, frame_pixels, &next.average_bps);
	if (!ret)
		*state = next;
	return ret;
}
