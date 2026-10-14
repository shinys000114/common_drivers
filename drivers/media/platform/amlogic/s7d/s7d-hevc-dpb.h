/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_DPB_H
#define S7D_HEVC_DPB_H

#include "s7d-codec-session.h"
#include "s7d-hevc-input.h"

#define S7D_HEVC_DPB_SLOTS 24U
#define S7D_HEVC_NO_SLOT S7D_HEVC_DPB_SLOTS

struct s7d_hevc_picture_metadata {
	u64 input_token;
	u64 timestamp_ns;
	struct s7d_codec_timestamp_metadata timestamp_meta;
};

struct s7d_hevc_picture_ref {
	s32 poc;
	u32 slot;
};

struct s7d_hevc_poc_state {
	s32 prev_tid0_poc;
	s32 irap_poc;
	u32 irap_nal_type;
	bool prev_tid0_valid;
	bool irap_valid;
	bool no_rasl_output;
	bool started;
};

enum s7d_hevc_plan_kind {
	S7D_HEVC_PLAN_PICTURE,
	S7D_HEVC_PLAN_SKIP_RASL,
	S7D_HEVC_PLAN_BUMP,
};

struct s7d_hevc_picture_plan {
	u64 generation;
	u64 parameter_epoch;
	u64 base_revision;
	u64 plan_id;
	enum s7d_hevc_plan_kind kind;
	u32 slot_count;
	u32 current_slot;
	s32 poc;
	u32 slice_type;
	u32 retain_mask;
	u32 drop_output_mask;
	u32 list_count[2];
	struct s7d_hevc_picture_ref list[2][S7D_HEVC_RPS_ENTRIES];
	struct s7d_hevc_picture_ref collocated;
	struct s7d_hevc_picture_metadata metadata;
	struct s7d_hevc_poc_state next_poc;
	bool reference;
	bool output_needed;
	bool temporal_mvp;
	bool collocated_valid;
	bool low_delay;
	bool clear_dpb;
};

struct s7d_hevc_dpb_actions {
	u64 generation;
	u64 revision;
	u32 surface_release_mask;
	u32 mv_release_mask;
	u32 discard_mask;
	u64 discard_write_token[S7D_HEVC_DPB_SLOTS];
};

struct s7d_hevc_dpb_picture {
	struct s7d_hevc_picture_metadata metadata;
	u64 write_token;
	u32 latency;
	u32 slice_type;
	s32 poc;
	bool valid;
	bool decoded;
	bool reference;
	bool output_needed;
	bool published;
};

struct s7d_hevc_dpb {
	u64 generation;
	u64 parameter_epoch;
	u64 revision;
	u64 pending_reset_generation;
	u64 active_write_token;
	u64 last_input_token;
	u32 slot_count;
	u32 max_dec_pic_buffering;
	u32 num_reorder_pics;
	u32 max_latency_pictures;
	u32 log2_max_poc_lsb;
	u32 retiring_mask;
	s32 last_output_poc;
	bool last_output_valid;
	bool active;
	struct s7d_hevc_poc_state poc;
	struct s7d_hevc_picture_plan active_plan;
	struct s7d_hevc_dpb_actions pending_actions;
	struct s7d_hevc_dpb_picture picture[S7D_HEVC_DPB_SLOTS];
};

struct s7d_hevc_output_plan {
	u64 generation;
	u64 revision;
	u32 slot;
	u64 write_token;
	s32 poc;
	u32 flags;
	struct s7d_hevc_picture_metadata metadata;
};

/* Heap objects; caller serializes every operation under one transaction lock. */
int s7d_hevc_dpb_init(struct s7d_hevc_dpb *dpb,
		      const struct s7d_hevc_sps *sps, u64 generation,
		      u64 parameter_epoch, u32 slot_count);
int s7d_hevc_dpb_plan(const struct s7d_hevc_dpb *dpb,
		      const struct s7d_hevc_slice_info *slice,
		      const struct s7d_hevc_picture_metadata *metadata,
		      u32 queued_slot_mask, struct s7d_hevc_picture_plan *plan);
/* Current surface acquired/pinned and reference mappings live; before MMIO. */
int s7d_hevc_dpb_begin(struct s7d_hevc_dpb *dpb,
		       const struct s7d_hevc_picture_plan *plan, u64 write_token);
int s7d_hevc_dpb_skip_committed(struct s7d_hevc_dpb *dpb,
				const struct s7d_hevc_picture_plan *plan);
/* Both flags require actual matching completion and joined DMA/IRQ/work. */
int s7d_hevc_dpb_complete(struct s7d_hevc_dpb *dpb, u64 plan_id,
			  u32 slot, u64 write_token, bool picture_done,
			  bool physical_idle, struct s7d_hevc_dpb_actions *actions);
int s7d_hevc_dpb_peek_output(const struct s7d_hevc_dpb *dpb, bool drain,
			     struct s7d_hevc_output_plan *out);
/* Only after frame_ready succeeds for this exact output plan. */
int s7d_hevc_dpb_output_committed(struct s7d_hevc_dpb *dpb,
				  const struct s7d_hevc_output_plan *out,
				  struct s7d_hevc_dpb_actions *actions);
int s7d_hevc_dpb_abort(struct s7d_hevc_dpb *dpb, u64 plan_id,
		       u32 slot, u64 write_token, bool physical_idle,
		       struct s7d_hevc_dpb_actions *actions);
int s7d_hevc_dpb_reset(struct s7d_hevc_dpb *dpb, u64 new_generation,
		       bool physical_idle, struct s7d_hevc_dpb_actions *actions);
/* Acknowledge every action, including an empty reset, after real releases. */
int s7d_hevc_dpb_release_committed(struct s7d_hevc_dpb *dpb,
				   const struct s7d_hevc_dpb_actions *actions);

#endif
