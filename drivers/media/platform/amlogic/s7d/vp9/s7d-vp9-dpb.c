// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "s7d-vp9-dpb.h"
#include "s7d-vp9-input.h"

static int map_mask(const struct s7d_vp9_dpb *dpb, u32 *mask)
{
	u32 i, value = 0;

	if (!dpb || !mask || !dpb->generation ||
	    dpb->physical_count < S7D_VP9_CAPTURE_MINIMUM ||
	    dpb->physical_count > S7D_VP9_MAX_PHYSICAL)
		return -EINVAL;
	for (i = 0; i < S7D_VP9_REFERENCE_SLOTS; i++) {
		if (dpb->map[i] == S7D_VP9_NO_PHYSICAL)
			continue;
		if (dpb->map[i] >= dpb->physical_count)
			return -EPROTO;
		value |= BIT(dpb->map[i]);
	}
	*mask = value;
	return 0;
}

int s7d_vp9_dpb_init(struct s7d_vp9_dpb *dpb, u64 generation, u32 physical_count)
{
	struct s7d_vp9_dpb next = { .generation = generation,
		.physical_count = physical_count };
	u32 i;

	if (!dpb || !generation || physical_count < S7D_VP9_CAPTURE_MINIMUM ||
	    physical_count > S7D_VP9_MAX_PHYSICAL)
		return -EINVAL;
	for (i = 0; i < S7D_VP9_REFERENCE_SLOTS; i++)
		next.map[i] = S7D_VP9_NO_PHYSICAL;
	*dpb = next;
	return 0;
}

int s7d_vp9_dpb_begin(struct s7d_vp9_dpb *dpb,
		       const struct s7d_vp9_header_prefix *header,
		       u32 current_slot, u64 write_token, struct s7d_vp9_dpb_plan *plan)
{
	struct s7d_vp9_dpb_plan next = { };
	u32 i;
	int ret;

	if (!header || !plan || !write_token)
		return -EINVAL;
	ret = map_mask(dpb, &next.old_mask);
	if (ret)
		return ret;
	if (dpb->active)
		return -EBUSY;
	if (dpb->serial == U64_MAX)
		return -EOVERFLOW;
	if (current_slot >= dpb->physical_count)
		return -EINVAL;
	if (next.old_mask & BIT(current_slot))
		return -EBUSY;
	if (header->show_existing)
		return -EOPNOTSUPP;
	if (header->keyframe && header->refresh_flags != 0xff)
		return -EINVAL;
	next.serial = dpb->serial + 1;
	next.generation = dpb->generation;
	next.current_slot = current_slot;
	next.write_token = write_token;
	for (i = 0; i < S7D_VP9_FRAME_REFERENCES; i++) {
		next.reference[i] = S7D_VP9_NO_PHYSICAL;
		if (header->keyframe || header->intra_only)
			continue;
		if (header->reference_index[i] >= S7D_VP9_REFERENCE_SLOTS)
			return -EINVAL;
		next.reference[i] = dpb->map[header->reference_index[i]];
		if (next.reference[i] == S7D_VP9_NO_PHYSICAL)
			return -ENODATA;
	}
	for (i = 0; i < S7D_VP9_REFERENCE_SLOTS; i++) {
		next.logical_to_physical[i] = dpb->map[i];
		next.next_map[i] = header->refresh_flags & BIT(i) ? current_slot : dpb->map[i];
		if (next.next_map[i] != S7D_VP9_NO_PHYSICAL)
			next.next_mask |= BIT(next.next_map[i]);
	}
	next.release_mask = (next.old_mask | BIT(current_slot)) & ~next.next_mask;
	dpb->serial = next.serial;
	dpb->pending = next;
	dpb->active = true;
	*plan = next;
	return 0;
}

static int pending_matches(const struct s7d_vp9_dpb *dpb, u64 serial, u64 token)
{
	if (!dpb || !serial || !token)
		return -EINVAL;
	if (!dpb->active || dpb->pending.serial != serial ||
	    dpb->pending.write_token != token || dpb->pending.generation != dpb->generation ||
	    dpb->pending.serial != dpb->serial)
		return -ESTALE;
	return 0;
}

int s7d_vp9_dpb_commit(struct s7d_vp9_dpb *dpb, u64 serial, u64 write_token)
{
	int ret = pending_matches(dpb, serial, write_token);

	if (ret)
		return ret;
	memcpy(dpb->map, dpb->pending.next_map, sizeof(dpb->map));
	dpb->revision = dpb->pending.serial;
	dpb->pending = (struct s7d_vp9_dpb_plan) { };
	dpb->active = false;
	return 0;
}

int s7d_vp9_dpb_abort(struct s7d_vp9_dpb *dpb, u64 serial, u64 write_token)
{
	int ret = pending_matches(dpb, serial, write_token);

	if (ret)
		return ret;
	dpb->pending = (struct s7d_vp9_dpb_plan) { };
	dpb->active = false;
	return 0;
}

int s7d_vp9_dpb_owned_mask(const struct s7d_vp9_dpb *dpb, u32 *mask)
{
	u32 value;
	int ret = map_mask(dpb, &value);

	if (ret || !mask)
		return ret ?: -EINVAL;
	if (dpb->active && dpb->pending.current_slot >= dpb->physical_count)
		return -EPROTO;
	if (dpb->active)
		value |= BIT(dpb->pending.current_slot);
	*mask = value;
	return 0;
}
