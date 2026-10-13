/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_JPEG_PROTOCOL_H
#define S7D_JPEG_PROTOCOL_H

#include "s7d-jpeg-quant.h"

struct regmap;

struct s7d_jpeg_plane {
	dma_addr_t dma;
	u32 bytes;
	u32 stride;
};

struct s7d_jpeg_job {
	u32 input_format;
	u32 width;
	u32 height;
	struct s7d_jpeg_plane y;
	struct s7d_jpeg_plane uv;
	dma_addr_t scan_dma;
	u32 scan_capacity;
};

#define S7D_JPEG_ENCODER_DONE	4

struct s7d_jpeg_irq_snapshot {
	u32 status;
	u32 scan_bytes;
};

int s7d_jpeg_protocol_check(const struct s7d_jpeg_job *job);

/* Caller completed fresh JPEG prepare, port enable and firmware load. */
/* It owns the same DOS lease/transaction, idle CPU and live DMA/FW refs. */
/* quant is the immutable builder result also used for this job's CPU header. */
/* IRQ is installed; a partial failure requires halt/join or retained backing. */
int s7d_jpeg_protocol_program(struct regmap *map, const struct s7d_jpeg_job *job,
			      const struct s7d_jpeg_quantization *quant);

/* Acknowledge mailbox2 and snapshot only; DONE/count do not prove DMA idle. */
/* No buffer may be returned before halt/join, CPU sync and output validation. */
int s7d_jpeg_protocol_irq(struct regmap *map,
			 struct s7d_jpeg_irq_snapshot *snapshot);

#endif
