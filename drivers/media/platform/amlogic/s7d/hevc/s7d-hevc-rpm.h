/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_RPM_H
#define S7D_HEVC_RPM_H

#include <linux/types.h>

#define S7D_HEVC_RPM_BYTES 256U
#define S7D_HEVC_RPM_VALID_WORDS 120U
#define S7D_HEVC_SLICE_HEADER_DONE 0x08U
#define S7D_HEVC_PICTURE_DATA_DONE 0x0aU

struct s7d_hevc_sps;
struct s7d_hevc_slice_info;
struct device;

struct s7d_hevc_geometry {
	u32 coded_width;
	u32 coded_height;
	u32 ctb_size;
	u32 ctb_columns;
	u32 ctb_rows;
	u32 ctb_count;
};

struct s7d_hevc_rpm {
	u16 word[S7D_HEVC_RPM_VALID_WORDS];
};

void s7d_hevc_rpm_report(struct device *dev, const struct s7d_hevc_rpm *rpm,
		const struct s7d_hevc_sps *host, const struct s7d_hevc_slice_info *slice);

enum s7d_hevc_event {
	S7D_HEVC_EVENT_SLICE_HEADER,
	S7D_HEVC_EVENT_NAL_SEARCH,
	S7D_HEVC_EVENT_PICTURE_DATA,
	S7D_HEVC_EVENT_NEED_DATA,
	S7D_HEVC_EVENT_INPUT_EMPTY,
	S7D_HEVC_EVENT_NAL_DONE,
	S7D_HEVC_EVENT_SEI,
};

struct s7d_hevc_picture_done {
	u32 status;
	u32 parser_lcu_start;
	u32 shift_status;
	u32 shift_byte_count;
	/* Fresh frame counter, reset to zero; includes checked transport padding. */
	u32 max_shift_bytes;
	u32 accepted_headers;
	bool header_armed;
	bool error_latched;
};

/* Immutable DMA snapshot; caller proves firmware pause and DMA visibility. */
int s7d_hevc_rpm_parse(const void *snapshot, size_t bytes,
		       bool efficiency_mode, struct s7d_hevc_rpm *rpm);
int s7d_hevc_rpm_check_sequence(const struct s7d_hevc_rpm *rpm,
			       const struct s7d_hevc_sps *host,
			       struct s7d_hevc_geometry *geometry);
/* Paused immutable RPM, checked against the original CPU-parsed picture. */
int s7d_hevc_rpm_decode_slice(const struct s7d_hevc_rpm *rpm,
			     const struct s7d_hevc_sps *host,
			     const struct s7d_hevc_slice_info *cpu,
			     struct s7d_hevc_slice_info *slice);
int s7d_hevc_classify_status(u32 status, enum s7d_hevc_event *event);

#endif
