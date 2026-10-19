// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "s7d-av1-workspace.h"

static const u32 av1_workspace_size[2][S7D_AV1_WORKSPACE_PARTS] = {
	{
		0x1e00, 0, 0, 0x800, 0x800, 0xd8000, 0x1000, 0,
		0x800, 0x80000, 0x800, 0, 0xd00, 0x50800, 0x24200,
		0x63000, 0x20000, 0x3000, 0, 0x2a00, 0x100, 0x800,
	},
	{
		0x4000, 0, 0, 0x800, 0x800, 0x360000, 0x2000, 0,
		0x800, 0x80000, 0x800, 0, 0x1a00, 0x60800, 0x28200,
		0xc3000, 0x20000, 0x5000, 0, 0x5400, 0x100, 0x800,
	},
};

static int av1_spec(u32 width, u32 height)
{
	if (!width || width > 4096 || !height || height > 2304)
		return -EINVAL;
	return width <= 1920 && height <= 1088 ? 0 : 1;
}

int s7d_av1_workspace_size(u32 width, u32 height, u32 *bytes)
{
	u32 size = 0, i;
	int spec;

	if (!bytes)
		return -EINVAL;
	spec = av1_spec(width, height);
	if (spec < 0)
		return spec;
	for (i = 0; i < S7D_AV1_WORKSPACE_PARTS; i++)
		size = ALIGN(size + av1_workspace_size[spec][i], SZ_4K);
	*bytes = size;
	return 0;
}

static int av1_region_validate(const struct s7d_av1_region *region,
			       u32 minimum)
{
	if (!region->address || !IS_ALIGNED(region->address, SZ_4K) ||
	    region->bytes < minimum ||
	    (u64)region->address + region->bytes > (1ULL << 32))
		return -EINVAL;
	return 0;
}

static bool av1_overlap(const struct s7d_av1_region *first,
			const struct s7d_av1_region *second)
{
	return (u64)first->address < (u64)second->address + second->bytes &&
	       (u64)second->address < (u64)first->address + first->bytes;
}

int s7d_av1_workspace_plan(u64 base, u32 bytes, u32 width, u32 height,
			   const struct s7d_av1_region *rpm,
			   const struct s7d_av1_region *lmem,
			   struct s7d_av1_workspace *workspace)
{
	struct s7d_av1_workspace next = { 0 };
	u64 cursor = base;
	u32 i, used;
	int spec, ret;

	if (!workspace || !rpm || !lmem || !base || base > U32_MAX ||
	    !IS_ALIGNED(base, SZ_4K) || base + bytes > (1ULL << 32))
		return -EINVAL;
	spec = av1_spec(width, height);
	if (spec < 0)
		return spec;
	ret = s7d_av1_workspace_size(width, height, &used);
	if (ret)
		return ret;
	if (bytes < used)
		return -ENOSPC;
	next.backing.address = base;
	next.backing.bytes = bytes;
	if (av1_region_validate(rpm, S7D_AV1_RPM_BYTES) ||
	    av1_region_validate(lmem, S7D_AV1_LMEM_BYTES) ||
	    av1_overlap(&next.backing, rpm) ||
	    av1_overlap(&next.backing, lmem) || av1_overlap(rpm, lmem))
		return -EINVAL;
	for (i = 0; i < S7D_AV1_WORKSPACE_PARTS; i++) {
		if (cursor > U32_MAX)
			return -ERANGE;
		next.part[i].address = cursor;
		next.part[i].bytes = av1_workspace_size[spec][i];
		cursor = ALIGN(cursor + next.part[i].bytes, (u64)SZ_4K);
	}
	next.rpm = *rpm;
	next.lmem = *lmem;
	next.max_width = width;
	next.max_height = height;
	next.used = used;
	*workspace = next;
	return 0;
}

int s7d_av1_workspace_validate(const struct s7d_av1_workspace *workspace)
{
	struct s7d_av1_workspace expected;
	u32 i;
	int ret;

	if (!workspace)
		return -EINVAL;
	ret = s7d_av1_workspace_plan(workspace->backing.address,
				    workspace->backing.bytes,
				    workspace->max_width, workspace->max_height,
				    &workspace->rpm, &workspace->lmem, &expected);
	if (ret)
		return ret;
	if (workspace->used != expected.used)
		return -EINVAL;
	for (i = 0; i < S7D_AV1_WORKSPACE_PARTS; i++)
		if (workspace->part[i].address != expected.part[i].address ||
		    workspace->part[i].bytes != expected.part[i].bytes)
			return -EINVAL;
	return 0;
}

int s7d_av1_rpm_snapshot(const u8 *data, size_t bytes, bool efficiency,
			 struct s7d_av1_rpm *rpm)
{
	struct s7d_av1_rpm next;
	u32 i;

	if (!data || !rpm || bytes != S7D_AV1_RPM_BYTES)
		return -EINVAL;
	for (i = 0; i < S7D_AV1_RPM_WORDS; i++) {
		u32 word = efficiency ? i : (i & ~3U) + 3 - (i & 3);

		next.word[i] = get_unaligned_le16(data + word * 2);
	}
	*rpm = next;
	return 0;
}

int s7d_av1_reference_layout(u32 width, u32 height, u32 bit_depth,
			     u32 superblock_size,
			     struct s7d_av1_reference_layout *layout)
{
	struct s7d_av1_reference_layout next;
	u32 columns, rows;

	if (!layout || av1_spec(width, height) < 0 ||
	    (bit_depth != 8 && bit_depth != 10) ||
	    (superblock_size != 64 && superblock_size != 128))
		return -EINVAL;
	next.body_bytes = DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 32) *
		(bit_depth == 10 ? 4096 : 3200);
	next.body_pages = DIV_ROUND_UP(next.body_bytes, SZ_4K);
	if (next.body_pages > S7D_AV1_MMU_MAX_PAGES)
		return -E2BIG;
	next.body_allocation = next.body_pages * SZ_4K;
	next.header_bytes = DIV_ROUND_UP(width, 128) *
		DIV_ROUND_UP(height, 64) * 32;
	next.header_capacity = width <= 1920 && height <= 1088 ? 0x10000 : 0x48000;
	columns = DIV_ROUND_UP(width, superblock_size);
	rows = DIV_ROUND_UP(height, superblock_size);
	next.mv_bytes = columns * rows * 16 * (superblock_size == 64 ? 16 : 64) +
		columns * ((rows >> 3) + (rows & 7)) * 16;
	next.mv_allocation = ALIGN(next.mv_bytes, SZ_64K);
	*layout = next;
	return 0;
}

int s7d_av1_mmu_map_contiguous(const struct s7d_av1_region *body,
			       u32 pages, u8 *map, size_t bytes)
{
	u32 i;

	if (!body || !map || !pages || pages > S7D_AV1_MMU_MAX_PAGES ||
	    bytes != S7D_AV1_MMU_MAP_BYTES ||
	    av1_region_validate(body, pages * SZ_4K))
		return -EINVAL;
	for (i = 0; i < pages; i++)
		put_unaligned_le32((body->address >> 12) + i, map + i * 4);
	memset(map + pages * 4, 0, bytes - pages * 4);
	return 0;
}
