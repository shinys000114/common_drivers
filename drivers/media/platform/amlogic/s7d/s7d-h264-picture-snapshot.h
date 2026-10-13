/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_PICTURE_SNAPSHOT_H
#define S7D_H264_PICTURE_SNAPSHOT_H

#include <linux/types.h>

#define S7D_H264_PICTURE_RPM_BYTES 2048U

struct s7d_h264_slice;

/*
 * Parse the selected non-MMU mh264 SLICE_HEAD_DONE RPM format: 1024 LE16
 * words, reversed within each group of four. The caller supplies a private,
 * CPU-visible snapshot that remains immutable for this call, taken while the
 * matching firmware header is paused, before the caller's decode continuation.
 * A live coherent LMEM allocation or mailbox IRQ alone does not meet this
 * contract; DMA completion, ordering, status association and copying belong to
 * the caller. The output must not overlap the input snapshot.
 *
 * The caller has accepted CONFIG_REQUEST through s7d_h264_parse_config() for
 * the same sequence and bound that SPS/layout to the DPB: progressive 8-bit
 * 4:2:0 only. Chroma/geometry are not inferred from this slice RPM. This parser
 * rejects unsupported fields, POC type 1 and FMO; it copies the typed fields
 * and bounded commands into the s7d_h264_slice declared in s7d-h264-dpb.h.
 *
 * No input pointer is retained. Output is unchanged on failure. Success is
 * header interpretation only, never PIC_DATA_DONE, input consumption, EOS,
 * capture publication, or proof that a decoder DMA engine is idle.
 */
int s7d_h264_parse_slice(const void *rpm, size_t bytes,
			 struct s7d_h264_slice *slice);

#endif
