// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/unaligned.h>

#include "s7d-codec-bundle.h"

#define S7D_BUNDLE_NEWP		0x4e455750U
#define S7D_BUNDLE_PACK		0x5041434bU
#define S7D_BUNDLE_VERSION	4U
#define S7D_BUNDLE_VERSION_OFFSET	16U
#define S7D_BUNDLE_PROBE_OFFSET	0x8100U
#define S7D_BUNDLE_PREFIX_SIZE	0x8000U
#define S7D_BUNDLE_MIN_PAYLOAD	256U

int s7d_codec_bundle_parse(const u8 *data, size_t size,
			   struct s7d_codec_bundle_view *view)
{
	size_t offset = 0;

	if (!data || !view)
		return -EINVAL;
	if (size > U32_MAX)
		return -EOVERFLOW;
	if (size < sizeof(u32))
		return -EMSGSIZE;
	if (get_unaligned_le32(data) != S7D_BUNDLE_NEWP)
		return -EOPNOTSUPP;
	if (size < S7D_BUNDLE_PROBE_OFFSET + sizeof(u32))
		return -EMSGSIZE;
	if (get_unaligned_le32(data + S7D_BUNDLE_VERSION_OFFSET) !=
	    S7D_BUNDLE_VERSION)
		return -EOPNOTSUPP;
	if (get_unaligned_le32(data + S7D_BUNDLE_PROBE_OFFSET) != S7D_BUNDLE_PACK)
		offset = S7D_BUNDLE_PREFIX_SIZE;
	if (size < offset || size - offset < S7D_BUNDLE_MIN_PAYLOAD)
		return -EMSGSIZE;

	view->data = data + offset;
	view->size = size - offset;
	return 0;
}
