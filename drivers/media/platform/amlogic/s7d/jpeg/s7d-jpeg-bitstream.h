/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_JPEG_BITSTREAM_H
#define S7D_JPEG_BITSTREAM_H

#include "s7d-jpeg-quant.h"

#define S7D_JPEG_HEADER_BYTES 592

struct s7d_jpeg_header {
	u8 data[S7D_JPEG_HEADER_BYTES];
};

struct s7d_jpeg_output {
	u32 bytesused;
	u32 scan_bytes;
};

/* NV12/NV21, baseline 8-bit 420; dimensions describe SOF, not DMA admission. */
/* The caller uses the same immutable quant snapshot for header and HW LUT. */
int s7d_jpeg_build_header(u32 input_format, u32 width, u32 height,
			  const struct s7d_jpeg_quantization *quant,
			  struct s7d_jpeg_header *header);

/* Caller proves DMA completion/visibility and owns the immutable scan backing. */
/* produced_bytes is the VLC count alone; scan must already end with EOI. */
/* Errors leave outputs unchanged; success is framing, not entropy/HW proof. */
int s7d_jpeg_validate_output(const u8 *scan, size_t scan_capacity,
			     u32 produced_bytes, size_t capture_capacity,
			     struct s7d_jpeg_output *output);

#endif
