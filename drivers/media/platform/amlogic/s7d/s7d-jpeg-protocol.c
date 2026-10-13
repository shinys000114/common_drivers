// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/videodev2.h>

#include "s7d-jpeg-protocol.h"

#define JPEG_REG(word)			((word) * 4)
#define JPEG_MFDIN_CTRL			JPEG_REG(0x1009)
#define JPEG_MFDIN_CANV			JPEG_REG(0x100b)
#define JPEG_MFDIN_STRIDE		JPEG_REG(0x100c)
#define JPEG_MFDIN_Y			JPEG_REG(0x100d)
#define JPEG_MFDIN_DCFG			JPEG_REG(0x100e)
#define JPEG_MFDIN_SCMD			JPEG_REG(0x100f)
#define JPEG_MFDIN_SIZE			JPEG_REG(0x1010)
#define JPEG_MFDIN_ENDIAN		JPEG_REG(0x1011)
#define JPEG_MFDIN_U			JPEG_REG(0x101f)
#define JPEG_MFDIN_V			JPEG_REG(0x1020)
#define JPEG_MFDIN_UV_STRIDE		JPEG_REG(0x1021)
#define JPEG_AMR_INT0			JPEG_REG(0x1025)
#define JPEG_AMR_INT1			JPEG_REG(0x1026)
#define JPEG_AMR_INT3			JPEG_REG(0x1028)
#define JPEG_MBOX2_CLEAR		JPEG_REG(0x1079)
#define JPEG_MBOX2_MASK			JPEG_REG(0x107a)
#define JPEG_MPSR			JPEG_REG(0x1301)
#define JPEG_CPSR			JPEG_REG(0x1321)
#define JPEG_IMEM_DMA_CTRL		JPEG_REG(0x1340)
#define JPEG_STATUS			JPEG_REG(0x1ac0)
#define JPEG_MCU			JPEG_REG(0x1ac1)
#define JPEG_CANVAS_DATAL		JPEG_REG(0x19ea)
#define JPEG_CANVAS_DATAH		JPEG_REG(0x19eb)
#define JPEG_CANVAS_ADDR		JPEG_REG(0x19ec)
#define JPEG_CANVAS_CONFIG		JPEG_REG(0x19ed)
#define JPEG_VB_START			JPEG_REG(0x1d10)
#define JPEG_VB_END			JPEG_REG(0x1d11)
#define JPEG_VB_WRITE			JPEG_REG(0x1d12)
#define JPEG_VB_READ			JPEG_REG(0x1d14)
#define JPEG_VB_CONTROL			JPEG_REG(0x1d16)
#define JPEG_VB_MEMORY			JPEG_REG(0x1d17)
#define JPEG_TOTAL_BYTES		JPEG_REG(0x1d1a)
#define JPEG_VLC_INTERRUPT		JPEG_REG(0x1d30)
#define JPEG_PICTURE_SIZE		JPEG_REG(0x1d31)
#define JPEG_PICTURE_POSITION		JPEG_REG(0x1d33)
#define JPEG_VLC_CONTROL		JPEG_REG(0x1d58)
#define JPEG_HUFFMAN_ADDR		JPEG_REG(0x1d5a)
#define JPEG_HUFFMAN_DATA		JPEG_REG(0x1d5b)
#define JPEG_MB_CONTROL			JPEG_REG(0x1f15)
#define JPEG_QDCT_CONTROL		JPEG_REG(0x1f25)
#define JPEG_X_START_END		JPEG_REG(0x1f26)
#define JPEG_Y_START_END		JPEG_REG(0x1f27)
#define JPEG_QUANT_ADDR			JPEG_REG(0x1f28)
#define JPEG_QUANT_DATA			JPEG_REG(0x1f29)

static const u8 jpeg_huffman_dc[2][28] = {
	{
		0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b,
	},
	{
		0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b,
	},
};

static const u8 jpeg_huffman_ac[2][178] = {
	{
		0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04,
		0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
		0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32,
		0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
		0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a,
		0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
		0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
		0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
		0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85,
		0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
		0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2,
		0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
		0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8,
		0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
		0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
	},
	{
		0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04,
		0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
		0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81,
		0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
		0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17,
		0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
		0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54,
		0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
		0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83,
		0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
		0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
		0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
		0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
		0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
		0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
	},
};

static int jpeg_dma_range(dma_addr_t dma, u32 bytes, u32 alignment)
{
	if (!bytes || (dma & (alignment - 1)) || dma > U32_MAX ||
	    bytes - 1 > U32_MAX - dma)
		return -ERANGE;
	return 0;
}

static bool jpeg_overlap(dma_addr_t a, u32 a_bytes, dma_addr_t b, u32 b_bytes)
{
	return (u64)a < (u64)b + b_bytes && (u64)b < (u64)a + a_bytes;
}

int s7d_jpeg_protocol_check(const struct s7d_jpeg_job *job)
{
	u32 stride, y_bytes, uv_bytes;
	int ret;

	if (!job)
		return -EINVAL;
	if (job->input_format != V4L2_PIX_FMT_NV12 &&
	    job->input_format != V4L2_PIX_FMT_NV21)
		return -EOPNOTSUPP;
	if (job->width < 16 || job->width > 8192 || job->height < 16 ||
	    job->height > 8190 || (job->width | job->height) & 1)
		return -ERANGE;
	stride = ALIGN(job->width, 32);
	if (job->y.stride != stride || job->uv.stride != stride)
		return -EINVAL;
	y_bytes = stride * job->height;
	uv_bytes = y_bytes / 2;
	if (job->y.bytes < y_bytes || job->uv.bytes < uv_bytes)
		return -ENOSPC;
	ret = jpeg_dma_range(job->y.dma, y_bytes, 8);
	if (ret)
		return ret;
	ret = jpeg_dma_range(job->uv.dma, uv_bytes, 8);
	if (ret)
		return ret;
	ret = jpeg_dma_range(job->scan_dma, job->scan_capacity, SZ_4K);
	if (ret)
		return ret;
	if (job->scan_capacity & (SZ_4K - 1))
		return -ERANGE;
	if (jpeg_overlap(job->y.dma, y_bytes, job->uv.dma, uv_bytes) ||
	    jpeg_overlap(job->y.dma, y_bytes, job->scan_dma, job->scan_capacity) ||
	    jpeg_overlap(job->uv.dma, uv_bytes, job->scan_dma, job->scan_capacity))
		return -EINVAL;
	return 0;
}

static int jpeg_cpu_idle(struct regmap *map)
{
	u32 value;
	int ret;

	ret = regmap_read(map, JPEG_MPSR, &value);
	if (ret)
		return ret;
	if (value)
		return -EBUSY;
	ret = regmap_read(map, JPEG_CPSR, &value);
	if (ret)
		return ret;
	if (value)
		return -EBUSY;
	ret = regmap_read(map, JPEG_IMEM_DMA_CTRL, &value);
	if (ret)
		return ret;
	return value & BIT(15) ? -EBUSY : 0;
}

static int jpeg_canvas(struct regmap *map, const struct s7d_jpeg_plane *plane,
			 u32 height, u32 index)
{
	u32 width = plane->stride / 8;
	const struct reg_sequence registers[] = {
		{ JPEG_CANVAS_DATAL, (u32)(plane->dma >> 3) | ((width & 7) << 29) },
		{ JPEG_CANVAS_DATAH, (width >> 3) | (height << 9) },
		{ JPEG_CANVAS_ADDR, index },
	};

	return regmap_multi_reg_write(map, registers, ARRAY_SIZE(registers));
}

static int jpeg_input(struct regmap *map, const struct s7d_jpeg_job *job)
{
	u32 format = job->input_format == V4L2_PIX_FMT_NV21 ? 2 : 3;
	const struct reg_sequence registers[] = {
		{ JPEG_MFDIN_SIZE, (ALIGN(job->width, 16) << 16) | job->height },
		{ JPEG_MFDIN_CTRL, format | BIT(13) | (2 << 29) },
		{ JPEG_MFDIN_CANV, BIT(28) | BIT(26) | BIT(24) | (2 << 16) | BIT(8) },
		{ JPEG_MFDIN_STRIDE, job->y.stride },
		{ JPEG_MFDIN_Y, job->y.dma },
		{ JPEG_MFDIN_U, job->uv.dma },
		{ JPEG_MFDIN_V, job->uv.dma },
		{ JPEG_MFDIN_UV_STRIDE, (job->uv.stride << 16) | job->uv.stride },
		{ JPEG_MFDIN_ENDIAN, 7 | (6 << 3) | (5 << 6) | (4 << 9) |
				     (3 << 12) | (2 << 15) | BIT(18) },
	};
	int ret;

	ret = regmap_write(map, JPEG_CANVAS_CONFIG, 0);
	if (ret)
		return ret;
	ret = jpeg_canvas(map, &job->y, job->height, 0x64);
	if (ret)
		return ret;
	ret = jpeg_canvas(map, &job->uv, job->height / 2, 0x65);
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(map, registers, ARRAY_SIZE(registers));
	if (ret)
		return ret;
	return regmap_update_bits(map, JPEG_MFDIN_DCFG, GENMASK(31, 10), BIT(17));
}

static int jpeg_huffman(struct regmap *map, const u8 *table, bool ac)
{
	u32 lut[162] = {};
	u32 code = 0, value, symbol, address;
	unsigned int length, i, position = 16;
	unsigned int entries = ac ? 162 : 12;
	int ret;

	for (length = 1; length <= 16; length++) {
		for (i = 0; i < table[length - 1]; i++) {
			symbol = table[position++];
			value = ((length - 1) << 16) | code++;
			if (!ac)
				address = symbol;
			else if (!symbol)
				address = 0;
			else if (symbol == 0xf0)
				address = 161;
			else
				address = 1 + 16 * ((symbol & 15) - 1) + (symbol >> 4);
			lut[address] = value;
		}
		code <<= 1;
	}
	for (i = 0; i < entries; i++) {
		ret = regmap_write(map, JPEG_HUFFMAN_DATA, lut[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int jpeg_tables(struct regmap *map, const struct s7d_jpeg_quantization *quant)
{
	unsigned int table, i;
	int ret;

	ret = regmap_write(map, JPEG_QUANT_ADDR, 0);
	if (ret)
		return ret;
	for (table = 0; table < S7D_JPEG_QUANT_TABLES; table++) {
		for (i = 0; i < S7D_JPEG_QUANT_COEFFICIENTS / 2; i++) {
			ret = regmap_write(map, JPEG_QUANT_DATA, quant->lut[table][i]);
			if (ret)
				return ret;
		}
	}
	ret = regmap_write(map, JPEG_HUFFMAN_ADDR, 0);
	if (ret)
		return ret;
	for (table = 0; table < 2; table++) {
		ret = jpeg_huffman(map, jpeg_huffman_dc[table], false);
		if (ret)
			return ret;
	}
	ret = regmap_write(map, JPEG_HUFFMAN_ADDR, 24);
	if (ret)
		return ret;
	for (table = 0; table < 2; table++) {
		ret = jpeg_huffman(map, jpeg_huffman_ac[table], true);
		if (ret)
			return ret;
	}
	return 0;
}

static int jpeg_encoder(struct regmap *map, const struct s7d_jpeg_job *job)
{
	const struct reg_sequence registers[] = {
		{ JPEG_PICTURE_SIZE, job->width | (job->height << 16) },
		{ JPEG_PICTURE_POSITION, 3 | BIT(4) },
		{ JPEG_X_START_END, (job->width - 1) << 16 },
		{ JPEG_Y_START_END, (job->height - 1) << 16 },
	};

	return regmap_multi_reg_write(map, registers, ARRAY_SIZE(registers));
}

static int jpeg_output(struct regmap *map, const struct s7d_jpeg_job *job)
{
	const struct reg_sequence registers[] = {
		{ JPEG_QDCT_CONTROL, BIT(16) | BIT(15) | BIT(12) | BIT(11) |
				     BIT(8) | BIT(6) | BIT(5) | BIT(4) | BIT(0) },
		{ JPEG_VLC_CONTROL, BIT(29) | BIT(28) | BIT(24) | BIT(21) |
				    BIT(20) | BIT(16) | (3 << 9) | BIT(8) },
		{ JPEG_MB_CONTROL, BIT(9) | BIT(0) },
		{ JPEG_MB_CONTROL, BIT(13) | BIT(12) },
		{ JPEG_TOTAL_BYTES, 0 },
		{ JPEG_VLC_INTERRUPT, 0 },
		{ JPEG_STATUS, 0 },
		{ JPEG_MCU, U32_MAX },
		{ JPEG_AMR_INT0, 0x15 },
		{ JPEG_AMR_INT1, 8 },
		{ JPEG_AMR_INT3, 0x14 },
		{ JPEG_VB_MEMORY, BIT(31) | (0x3f << 24) | (0x20 << 16) | 2 },
		{ JPEG_VB_START, job->scan_dma },
		{ JPEG_VB_WRITE, job->scan_dma },
		{ JPEG_VB_READ, job->scan_dma },
		{ JPEG_VB_END, job->scan_dma + job->scan_capacity - 1 },
		{ JPEG_VB_CONTROL, 1 },
		{ JPEG_VB_CONTROL, (7 << 3) | BIT(1) },
	};

	return regmap_multi_reg_write(map, registers, ARRAY_SIZE(registers));
}

int s7d_jpeg_protocol_program(struct regmap *map, const struct s7d_jpeg_job *job,
			      const struct s7d_jpeg_quantization *quant)
{
	int ret;

	if (!map || !quant)
		return -EINVAL;
	ret = s7d_jpeg_protocol_check(job);
	if (ret)
		return ret;
	ret = jpeg_cpu_idle(map);
	if (ret)
		return ret;
	ret = regmap_write(map, JPEG_MBOX2_MASK, 0);
	if (ret)
		return ret;
	ret = jpeg_input(map, job);
	if (ret)
		return ret;
	ret = jpeg_encoder(map, job);
	if (ret)
		return ret;
	ret = jpeg_tables(map, quant);
	if (ret)
		return ret;
	ret = jpeg_output(map, job);
	if (ret)
		return ret;
	ret = regmap_write(map, JPEG_MBOX2_CLEAR, 1);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, JPEG_MFDIN_SCMD, BIT(28), BIT(28));
	if (ret)
		return ret;
	return regmap_write(map, JPEG_MBOX2_MASK, U32_MAX);
}

int s7d_jpeg_protocol_irq(struct regmap *map,
			 struct s7d_jpeg_irq_snapshot *snapshot)
{
	struct s7d_jpeg_irq_snapshot result = {};
	int ret;

	if (!map || !snapshot)
		return -EINVAL;
	ret = regmap_write(map, JPEG_MBOX2_CLEAR, 1);
	if (ret)
		return ret;
	ret = regmap_read(map, JPEG_STATUS, &result.status);
	if (ret)
		return ret;
	if (result.status == S7D_JPEG_ENCODER_DONE) {
		ret = regmap_read(map, JPEG_TOTAL_BYTES, &result.scan_bytes);
		if (ret)
			return ret;
	}
	*snapshot = result;
	return 0;
}
