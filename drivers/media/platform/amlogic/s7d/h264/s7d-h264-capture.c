// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/regmap.h>

#include "s7d-h264-capture.h"

#define DOS_REG(word)		((word) * 4U)
#define VDEC_CANVAS_BLK32		DOS_REG(0x0005)
#define MDEC_PIC_DC_CTRL		DOS_REG(0x098e)
#define MDEC_CAV_LUT_DATAL	DOS_REG(0x09ea)
#define MDEC_CAV_LUT_DATAH	DOS_REG(0x09eb)
#define MDEC_CAV_LUT_ADDR		DOS_REG(0x09ec)
#define MDEC_CAV_CFG0		DOS_REG(0x09ed)

int s7d_h264_capture_program(struct regmap *map,
			     const struct s7d_codec_layout *layout, u32 slot,
			     const struct s7d_h264_capture_plane planes[2])
{
	struct s7d_h264_capture_descriptor descriptor;
	unsigned int i;
	int ret;

	if (!map)
		return -EINVAL;
	ret = s7d_h264_build_capture_descriptor(layout, slot, planes, &descriptor);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++) {
		const struct s7d_h264_canvas_plan *canvas = &descriptor.canvas[i];

		ret = regmap_write(map, MDEC_CAV_CFG0, descriptor.canvas_cfg0);
		if (ret)
			return ret;
		ret = regmap_write(map, MDEC_CAV_LUT_DATAL, canvas->data_low);
		if (ret)
			return ret;
		ret = regmap_write(map, MDEC_CAV_LUT_DATAH, canvas->data_high);
		if (ret)
			return ret;
		ret = regmap_write(map, MDEC_CAV_LUT_ADDR, canvas->index);
		if (ret)
			return ret;
		ret = regmap_write(map, VDEC_CANVAS_BLK32, canvas->block32);
		if (ret)
			return ret;
	}
	return regmap_write(map, descriptor.anc_offset, descriptor.anc_value);
}

int s7d_h264_capture_set_format(struct regmap *map,
				enum s7d_codec_capture_format format)
{
	u32 value = BIT(17);
	u32 actual;
	int ret;

	if (!map)
		return -EINVAL;
	if (format == S7D_CODEC_CAPTURE_NV21M)
		value |= BIT(16);
	else if (format != S7D_CODEC_CAPTURE_NV12M)
		return -EOPNOTSUPP;
	ret = regmap_update_bits(map, MDEC_PIC_DC_CTRL, BIT(17) | BIT(16), value);
	if (ret)
		return ret;
	ret = regmap_read(map, MDEC_PIC_DC_CTRL, &actual);
	if (ret)
		return ret;
	return (actual & (BIT(17) | BIT(16))) == value ? 0 : -EIO;
}
