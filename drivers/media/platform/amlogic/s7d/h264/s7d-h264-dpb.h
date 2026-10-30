/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_H264_DPB_H
#define S7D_H264_DPB_H

#include <linux/types.h>
#include "../common/s7d-codec-session.h"
#include "s7d-h264-layout.h"

#define S7D_H264_REF_LIST_SIZE 32U
#define S7D_H264_MMCO_SIZE 44U

enum s7d_h264_slice_type {
	S7D_H264_SLICE_I,
	S7D_H264_SLICE_P,
	S7D_H264_SLICE_B,
};

struct s7d_h264_reorder {
	u32 operation;
	u32 argument;
};

struct s7d_h264_mmco {
	u32 operation;
	u32 difference_minus1;
	u32 long_term_index;
};

struct s7d_h264_slice {
	enum s7d_h264_slice_type type;
	u32 first_mb;
	u32 frame_num;
	u32 idr_pic_id;
	u32 poc_type;
	u32 log2_frame_num;
	u32 log2_poc_lsb;
	u32 poc_lsb;
	s32 delta_bottom;
	u32 max_references;
	u32 mode_8x8_flags;
	u32 active[2];
	u32 reorder_count[2];
	struct s7d_h264_reorder reorder[2][S7D_H264_REF_LIST_SIZE];
	u32 mmco_count;
	struct s7d_h264_mmco mmco[S7D_H264_MMCO_SIZE];
	bool idr;
	bool reference;
	bool long_term_reference;
	bool no_output_of_prior_pics;
};

struct s7d_h264_capture_binding {
	struct s7d_codec_surface *surface;
	u64 surface_id;
	u64 generation;
};

struct s7d_h264_picture_ref {
	u32 slot;
	u32 frame_num;
	s32 poc;
	s32 top_poc;
	s32 bottom_poc;
	s32 long_term_index;
	s32 mv_slot;
	bool direct_8x8;
};

struct s7d_h264_picture_table {
	struct s7d_h264_picture_ref picture;
	bool active;
	bool is_current;
	bool reference;
};

struct s7d_h264_picture_plan {
	enum s7d_h264_slice_type type;
	u32 first_mb;
	u32 frame_num;
	u32 mode_8x8_flags;
	u32 slot;
	u32 slot_count;
	struct s7d_codec_surface *surface;
	u64 surface_id;
	u64 generation;
	u64 write_token;
	u64 input_token;
	u64 timestamp_ns;
	struct s7d_codec_timestamp_metadata timestamp_meta;
	s32 poc;
	s32 top_poc;
	s32 bottom_poc;
	s32 mv_slot;
	bool direct_8x8;
	u32 list_count[2];
	struct s7d_h264_picture_ref list[2][S7D_H264_REF_LIST_SIZE];
	struct s7d_h264_picture_table table[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	bool reference;
	bool new_picture;
};

struct s7d_h264_dpb_state {
	u64 generation;
	u64 surface_id;
	u64 write_token;
	u64 input_token;
	int active_slot;
	u32 complete_unpublished;
	u32 reference_count;
	u32 publication_count;
	int failure;
};

struct s7d_h264_dpb;

int s7d_h264_dpb_create(struct s7d_codec_session *session,
			const struct s7d_h264_sps *sps,
			const struct s7d_codec_layout *layout,
			const struct s7d_h264_capture_binding *bindings,
			unsigned int count, unsigned int mv_slots,
			struct s7d_h264_dpb **out);
/* Serialized process context; firmware remains paused until the plan is applied. */
int s7d_h264_dpb_prepare(struct s7d_h264_dpb *dpb,
			 const struct s7d_h264_slice *slice, u64 input_token,
			 u64 timestamp_ns,
			 const struct s7d_codec_timestamp_metadata *timestamp_meta,
			 struct s7d_h264_picture_plan *plan);
/* Only the matching real PIC_DATA_DONE may complete this write. */
int s7d_h264_dpb_picture_done(struct s7d_h264_dpb *dpb, u32 slot,
			      u64 write_token, u32 flags);
/* Publishes completed pictures only; this is not a drain acknowledgment. */
int s7d_h264_dpb_flush_output(struct s7d_h264_dpb *dpb);
int s7d_h264_dpb_finish_sequence(struct s7d_h264_dpb *dpb, bool discard_prior);
void s7d_h264_dpb_fail(struct s7d_h264_dpb *dpb, int error);
/* Same caller transaction lock as mutators, with live DPB/session references. */
int s7d_h264_dpb_get_state(const struct s7d_h264_dpb *dpb,
			   const struct s7d_codec_session *original_session,
			   struct s7d_h264_dpb_state *state);
/* Gate protocol work first. Failed full quiesce keeps this context and all pins. */
int s7d_h264_dpb_close(struct s7d_h264_dpb *dpb);

#endif
