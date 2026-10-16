// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "s7d-vp9-context.h"

static const u32 vp9_workspace_size[S7D_VP9_WORKSPACE_PARTS] = {
	0x8000, 0, 0, 0x800, 0x800, 0x800, 0x2000, 0,
	0x800, 0x800, 0, 0xa4800, 0xa4800, 0x36000, 0x9000,
	0x800, 0x800, 0x5000, 0x3000,
};

static bool vp9_overlap(const struct s7d_vp9_region *a,
			 const struct s7d_vp9_region *b)
{
	return (u64)a->address < (u64)b->address + b->bytes &&
	       (u64)b->address < (u64)a->address + a->bytes;
}

static int vp9_region_validate(const struct s7d_vp9_region *region,
			       u32 minimum, u32 alignment)
{
	if (!region->address || !IS_ALIGNED(region->address, alignment) ||
	    region->bytes < minimum ||
	    (u64)region->address + region->bytes > (1ULL << 32))
		return -EINVAL;
	return 0;
}

int s7d_vp9_workspace_plan(u64 base, u32 bytes,
			    const struct s7d_vp9_region *rpm,
			    const struct s7d_vp9_region *lmem,
			    struct s7d_vp9_workspace *workspace)
{
	struct s7d_vp9_workspace next = { 0 };
	u64 cursor = base;
	u32 i;

	if (!rpm || !lmem || !workspace || !base || base > U32_MAX ||
	    !IS_ALIGNED(base, 4096) || !bytes ||
	    base + bytes > (1ULL << 32))
		return -EINVAL;
	next.backing.address = base;
	next.backing.bytes = bytes;
	if (vp9_region_validate(rpm, S7D_VP9_RPM_BYTES, 4096) ||
	    vp9_region_validate(lmem, S7D_VP9_LMEM_BYTES, 4096) ||
	    vp9_overlap(&next.backing, rpm) ||
	    vp9_overlap(&next.backing, lmem) || vp9_overlap(rpm, lmem))
		return -EINVAL;
	for (i = 0; i < S7D_VP9_WORKSPACE_PARTS; i++) {
		if (cursor > U32_MAX ||
		    cursor + vp9_workspace_size[i] > base + bytes)
			return -ENOSPC;
		next.part[i].address = cursor;
		next.part[i].bytes = vp9_workspace_size[i];
		cursor = ALIGN(cursor + vp9_workspace_size[i], 4096);
	}
	if (cursor > base + bytes)
		return -ENOSPC;
	next.used = cursor - base;
	next.rpm = *rpm;
	next.lmem = *lmem;
	*workspace = next;
	return 0;
}

int s7d_vp9_workspace_validate(const struct s7d_vp9_workspace *workspace)
{
	struct s7d_vp9_workspace expected;
	u32 i;
	int ret;

	if (!workspace)
		return -EINVAL;
	ret = s7d_vp9_workspace_plan(workspace->backing.address,
				     workspace->backing.bytes,
				     &workspace->rpm, &workspace->lmem,
				     &expected);
	if (ret)
		return ret;
	if (expected.used != workspace->used)
		return -EINVAL;
	for (i = 0; i < S7D_VP9_WORKSPACE_PARTS; i++)
		if (expected.part[i].address != workspace->part[i].address ||
		    expected.part[i].bytes != workspace->part[i].bytes)
			return -EINVAL;
	return 0;
}

int s7d_vp9_rpm_snapshot(const u8 *data, size_t bytes, bool efficiency,
			 struct s7d_vp9_rpm *rpm)
{
	struct s7d_vp9_rpm next;
	u32 i;

	if (!data || !rpm || bytes != S7D_VP9_RPM_BYTES)
		return -EINVAL;
	if (!efficiency)
		return -EOPNOTSUPP;
	for (i = 0; i < S7D_VP9_RPM_WORDS; i++)
		next.word[i] = get_unaligned_le16(data + i * 2);
	*rpm = next;
	return 0;
}

int s7d_vp9_stage_keyframe(const u8 *data, size_t bytes, u8 *output,
			   size_t capacity, u32 *staged_bytes,
			   struct s7d_vp9_keyframe_prefix *prefix)
{
	struct s7d_vp9_keyframe_prefix next;
	struct s7d_vp9_packet packet;
	u32 size, i;
	int ret;

	if (!output || !staged_bytes || !prefix)
		return -EINVAL;
	ret = s7d_vp9_split_packet(data, bytes, &packet);
	if (ret)
		return ret;
	if (packet.count != 1)
		return -EOPNOTSUPP;
	size = packet.frame[0].size;
	if (size > U32_MAX - 16 || capacity < (u64)size + 16)
		return -ENOSPC;
	ret = s7d_vp9_parse_keyframe_prefix(data, size, &next);
	if (ret)
		return ret;
	memmove(output + 16, data, size);
	put_unaligned_be32(size + 16, output);
	for (i = 0; i < 4; i++)
		output[i + 4] = ~output[i];
	put_unaligned_be32(1, output + 8);
	memcpy(output + 12, "AMLV", 4);
	*prefix = next;
	*staged_bytes = size + 16;
	return 0;
}

static u32 vp9_threshold(u32 level, u32 sharpness)
{
	u32 limit = level >> ((sharpness > 0) + (sharpness > 4));

	if (sharpness)
		limit = min(limit, 9 - sharpness);
	limit = max(limit, 1U);
	return ((limit & 0x3f) << 8) | (2 * (level + 2) + limit);
}

static void vp9_filter_plan(const struct s7d_vp9_rpm *rpm,
			    struct s7d_vp9_picture *picture)
{
	u8 level[8][4][2] = { 0 };
	u32 i, ref, mode;
	int base = rpm->word[23], scale = 1 << (base >> 5);

	for (i = 0; i < 32; i++)
		picture->threshold[i] =
			(vp9_threshold(i * 2 + 1, rpm->word[24]) << 16) |
			vp9_threshold(i * 2, rpm->word[24]);
	for (i = 0; i < 8; i++) {
		int current = base;
		u16 feature = rpm->word[36 + i];

		if (rpm->word[34] && (feature & BIT(15))) {
			int delta = feature & 0x3f;

			if (feature & BIT(8))
				delta = -delta;
			current = clamp(rpm->word[35] ? delta : base + delta,
					0, 63);
		}
		if (!rpm->word[16]) {
			memset(level[i], current, sizeof(level[i]));
			continue;
		}
		level[i][0][0] = clamp(current + (s8)rpm->word[17] * scale,
					 0, 63);
		for (ref = 1; ref < 4; ref++)
			for (mode = 0; mode < 2; mode++)
				level[i][ref][mode] = clamp(current +
					(s8)rpm->word[17 + ref] * scale +
					(s8)rpm->word[21 + mode] * scale, 0, 63);
	}
	for (i = 0; i < 16; i++) {
		u32 value = 0;

		for (ref = 0; ref < 4; ref++)
			value |= (u32)level[i >> 1][ref][i & 1] << (ref * 8);
		picture->level[i] = base ? value : 0;
	}
}

int s7d_vp9_picture_plan(const struct s7d_vp9_workspace *workspace,
			 const struct s7d_vp9_rpm *rpm,
			 const struct s7d_vp9_keyframe_prefix *prefix,
			 const struct s7d_codec_layout *layout,
			 const struct s7d_vp9_region surface[2],
			 struct s7d_vp9_picture *picture)
{
	struct s7d_vp9_picture next = { 0 };
	u32 width, height, pitch, storage, i;
	int ret;

	if (!rpm || !prefix || !layout || !surface || !picture)
		return -EINVAL;
	ret = s7d_vp9_workspace_validate(workspace);
	if (ret)
		return ret;
	width = rpm->word[10];
	height = rpm->word[11];
	if (rpm->word[0] || rpm->word[1] || rpm->word[3] ||
	    rpm->word[4] != 1 || rpm->word[25] != 8)
		return -EOPNOTSUPP;
	if (!prefix->render_width || prefix->render_width > 65535 ||
	    !prefix->render_height || prefix->render_height > 65535 ||
	    !width || width > 4096 || !height || height > 2304 ||
	    width != prefix->width || height != prefix->height ||
	    rpm->word[5] != prefix->error_resilient || rpm->word[7] > 1 ||
	    rpm->word[16] > 1 || rpm->word[23] > 63 || rpm->word[24] > 7 ||
	    rpm->word[34] > 1 || (rpm->word[34] && rpm->word[35] > 1))
		return -EINVAL;
	if (rpm->word[7]) {
		if (rpm->word[12] != prefix->render_width ||
		    rpm->word[13] != prefix->render_height)
			return -EINVAL;
	} else if (prefix->render_width != width ||
		   prefix->render_height != height) {
		return -EINVAL;
	}
	if (rpm->word[16])
		for (i = 17; i < 23; i++)
			if ((s8)rpm->word[i] < -63 || (s8)rpm->word[i] > 63)
				return -EINVAL;
	pitch = ALIGN(width, 64);
	storage = ALIGN(height, 64);
	if ((layout->format != S7D_CODEC_CAPTURE_NV12M &&
	     layout->format != S7D_CODEC_CAPTURE_NV21M) ||
	    layout->coded_width != width || layout->coded_height != height ||
	    layout->visible_left || layout->visible_top ||
	    layout->visible_width != width || layout->visible_height != height ||
	    layout->plane_count != 2 || layout->pitch[0] != pitch ||
	    layout->pitch[1] != pitch || layout->storage_height != storage ||
	    layout->size[0] != pitch * storage ||
	    layout->size[1] != pitch * storage / 2 ||
	    layout->total_size != layout->size[0] + layout->size[1])
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		if (vp9_region_validate(&surface[i], layout->size[i], 32) ||
		    vp9_overlap(&surface[i], &workspace->backing) ||
		    vp9_overlap(&surface[i], &workspace->rpm) ||
		    vp9_overlap(&surface[i], &workspace->lmem))
			return -EINVAL;
	}
	if (vp9_overlap(&surface[0], &surface[1]))
		return -EINVAL;
	next.size = (height << 16) | width;
	next.body_bytes = DIV_ROUND_UP(width, 64) *
			  DIV_ROUND_UP(height, 32) * 3200;
	next.header_bytes = DIV_ROUND_UP(width, 128) *
			    DIV_ROUND_UP(height, 64) * 32;
	next.sao_ctrl1 = (2 << 14) | 0xff0 | 1;
	next.ipp_axiif = (2 << 8) | 0xf;
	if (layout->format == S7D_CODEC_CAPTURE_NV12M) {
		next.sao_ctrl1 |= BIT(8);
	} else {
		next.sao_ctrl1 &= ~BIT(8);
		next.ipp_axiif |= BIT(12);
	}
	next.dblk_cfgb = 0xfe01 | (width >= 1280 ? BIT(4) : 0);
	vp9_filter_plan(rpm, &next);
	*picture = next;
	return 0;
}
