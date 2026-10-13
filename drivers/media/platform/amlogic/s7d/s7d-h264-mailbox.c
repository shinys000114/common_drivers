// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "s7d-h264-context.h"
#include "s7d-h264-mailbox.h"

#define DOS_REG(word) ((word) << 2)
#define LMEM_DMA_CTRL DOS_REG(0x0350)
#define DPB_STATUS_REG DOS_REG(0x09d3)
#define AV_SCRATCH_1 DOS_REG(0x09c1)
#define AV_SCRATCH_2 DOS_REG(0x09c2)
#define AV_SCRATCH_B DOS_REG(0x09cb)
#define LMEM_DMA_BUSY (1U << 15)
#define LMEM_POLL_US 10U
#define LMEM_TIMEOUT_US 10000U
#define H264_SLICE_HEAD_DONE 0x01U

static struct s7d_h264_mailbox_config *
s7d_h264_mailbox_read(struct regmap *map, struct s7d_h264_context *context,
		      u32 expected_status)
{
	struct s7d_h264_mailbox_config *next;
	struct s7d_codec_dma *lmem;
	const void *cpu;
	u32 status, control;
	int ret;

	if (in_interrupt())
		return ERR_PTR(-EWOULDBLOCK);
	lmem = s7d_h264_context_lmem(context);
	if (!lmem || s7d_codec_dma_size(lmem) != SZ_4K)
		return ERR_PTR(-EINVAL);
	cpu = s7d_codec_dma_cpu(lmem);
	if (!cpu)
		return ERR_PTR(-EINVAL);
	next = kzalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return ERR_PTR(-ENOMEM);
	ret = regmap_read(map, DPB_STATUS_REG, &status);
	if (ret)
		goto failed;
	if (status != expected_status) {
		ret = -EAGAIN;
		goto failed;
	}
	ret = regmap_read_poll_timeout(map, LMEM_DMA_CTRL, control,
				       !(control & LMEM_DMA_BUSY),
				       LMEM_POLL_US, LMEM_TIMEOUT_US);
	if (ret)
		goto failed;
	dma_rmb();
	if (expected_status == S7D_H264_CONFIG_REQUEST) {
		ret = regmap_read(map, AV_SCRATCH_1, &next->config.scratch1);
		if (ret)
			goto failed;
		ret = regmap_read(map, AV_SCRATCH_2, &next->config.scratch2);
		if (ret)
			goto failed;
		ret = regmap_read(map, AV_SCRATCH_B, &next->config.scratchb);
		if (ret)
			goto failed;
	}
	memcpy(next->rpm, cpu, sizeof(next->rpm));
	ret = regmap_read(map, LMEM_DMA_CTRL, &control);
	if (ret)
		goto failed;
	if (control & LMEM_DMA_BUSY) {
		ret = -ESTALE;
		goto failed;
	}
	ret = regmap_read(map, DPB_STATUS_REG, &status);
	if (ret)
		goto failed;
	if (status != expected_status) {
		ret = -ESTALE;
		goto failed;
	}
	next->config.status = status;
	next->config.rpm_bytes = sizeof(next->rpm);
	return next;
failed:
	kfree(next);
	return ERR_PTR(ret);
}

int s7d_h264_mailbox_read_config(struct regmap *map,
				 struct s7d_h264_context *context,
				 struct s7d_h264_mailbox_config *packet)
{
	struct s7d_h264_mailbox_config *next;

	if (!map || !context || !packet)
		return -EINVAL;
	next = s7d_h264_mailbox_read(map, context, S7D_H264_CONFIG_REQUEST);
	if (IS_ERR(next))
		return PTR_ERR(next);
	*packet = *next;
	packet->config.rpm = packet->rpm;
	kfree(next);
	return 0;
}

int s7d_h264_mailbox_read_slice(struct regmap *map,
				struct s7d_h264_context *context,
				void *rpm, size_t rpm_bytes)
{
	struct s7d_h264_mailbox_config *next;

	if (!map || !context || !rpm || rpm_bytes < S7D_H264_MAILBOX_RPM_BYTES)
		return -EINVAL;
	next = s7d_h264_mailbox_read(map, context, H264_SLICE_HEAD_DONE);
	if (IS_ERR(next))
		return PTR_ERR(next);
	memcpy(rpm, next->rpm, sizeof(next->rpm));
	kfree(next);
	return 0;
}
