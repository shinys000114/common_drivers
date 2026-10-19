/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_DPB_H
#define S7D_AV1_DPB_H

#include <linux/types.h>

#define S7D_AV1_DPB_REFERENCES 8U
#define S7D_AV1_DPB_FRAME_REFERENCES 7U
#define S7D_AV1_DPB_MAX_SLOTS 16U
#define S7D_AV1_DPB_NO_SLOT S7D_AV1_DPB_MAX_SLOTS
#define S7D_AV1_DPB_PRIMARY_NONE 7U

enum s7d_av1_frame_type {
	S7D_AV1_KEY_FRAME,
	S7D_AV1_INTER_FRAME,
	S7D_AV1_INTRA_ONLY_FRAME,
	S7D_AV1_SWITCH_FRAME,
};

struct s7d_av1_dpb_sequence {
	u32 order_hint_bits;
	u32 frame_id_bits;
	u32 delta_frame_id_bits;
};

struct s7d_av1_dpb_frame {
	u32 frame_type;
	u32 width;
	u32 height;
	u32 decode_width;
	u32 bit_depth;
	u32 order_hint;
	u32 current_frame_id;
	u32 display_frame_id;
	u32 existing_slot;
	u32 refresh_flags;
	u32 primary_ref;
	u32 last_ref;
	u32 golden_ref;
	u32 remap[S7D_AV1_DPB_FRAME_REFERENCES];
	u32 delta_frame_id_minus_1[S7D_AV1_DPB_FRAME_REFERENCES];
	u32 ref_order_hint[S7D_AV1_DPB_REFERENCES];
	bool show_existing;
	bool show_frame;
	bool showable_frame;
	bool error_resilient;
	bool short_signaling;
	bool display_frame_id_valid;
};

struct s7d_av1_picture_id {
	u64 backing_cookie;
	u32 slot;
};

struct s7d_av1_dpb_picture {
	struct s7d_av1_picture_id id;
	u32 width;
	u32 height;
	u32 decode_width;
	u32 bit_depth;
	u32 frame_type;
	u32 frame_id;
	u32 order_hint;
	u32 ref_order_hint[S7D_AV1_DPB_FRAME_REFERENCES];
	bool decoded;
	bool showable;
};

struct s7d_av1_dpb_metadata {
	u64 input_token;
	u64 timestamp_ns;
};

struct s7d_av1_dpb_plan {
	u64 generation;
	u64 serial;
	u64 base_revision;
	struct s7d_av1_dpb_metadata metadata;
	struct s7d_av1_dpb_picture current_picture;
	struct s7d_av1_dpb_picture reference[S7D_AV1_DPB_FRAME_REFERENCES];
	u32 remap[S7D_AV1_DPB_FRAME_REFERENCES];
	u32 next_map[S7D_AV1_DPB_REFERENCES];
	u32 sign_bias_mask;
	u32 primary_ref;
	u32 old_mask;
	u32 next_mask;
	bool decode;
	bool output;
	bool consume_key_showable;
};

struct s7d_av1_dpb_output {
	u64 generation;
	u64 serial;
	struct s7d_av1_dpb_metadata metadata;
	struct s7d_av1_dpb_picture picture;
};

struct s7d_av1_dpb {
	struct s7d_av1_dpb_sequence sequence;
	u64 generation;
	u64 serial;
	u64 revision;
	u64 backing_cookie[S7D_AV1_DPB_MAX_SLOTS];
	struct s7d_av1_dpb_picture picture[S7D_AV1_DPB_MAX_SLOTS];
	struct s7d_av1_dpb_plan pending;
	struct s7d_av1_dpb_output output;
	u32 map[S7D_AV1_DPB_REFERENCES];
	u32 slot_count;
	u32 previous_frame_id;
	bool previous_frame_id_valid;
	bool started;
	bool active;
	bool output_pending;
};

/* Fresh zeroed heap object; the backend serializes every frame transaction. */
int s7d_av1_dpb_init(struct s7d_av1_dpb *dpb,
		     const struct s7d_av1_dpb_sequence *sequence,
		     u64 generation, u32 slot_count);
/* Only after actual internal AVBC/header/MV/linear-shadow allocation and pinning. */
int s7d_av1_dpb_bind(struct s7d_av1_dpb *dpb, u32 slot, u64 backing_cookie);
/* Reference and current backing stay pinned before any picture MMIO. */
int s7d_av1_dpb_begin(struct s7d_av1_dpb *dpb,
		      const struct s7d_av1_dpb_frame *frame, u32 current_slot,
		      const struct s7d_av1_dpb_metadata *metadata,
		      struct s7d_av1_dpb_plan *plan);
/* physical_idle means matching DOS halt plus IRQ/worker join, not header pause. */
int s7d_av1_dpb_complete(struct s7d_av1_dpb *dpb, u64 serial,
			 u64 input_token, bool picture_done, bool physical_idle);
int s7d_av1_dpb_abort(struct s7d_av1_dpb *dpb, u64 serial,
		      u64 input_token, bool physical_idle);
int s7d_av1_dpb_peek_output(const struct s7d_av1_dpb *dpb,
			    struct s7d_av1_dpb_output *output);
/* CAPTURE publication/copy completed; no reader may retain the linear shadow. */
int s7d_av1_dpb_output_committed(struct s7d_av1_dpb *dpb,
				 const struct s7d_av1_dpb_output *output);
/* Recyclable slots are bound slots outside this mask; backing remains pinned. */
int s7d_av1_dpb_owned_mask(const struct s7d_av1_dpb *dpb, u32 *mask);
/* The sole unbind path: actual full stop/join before session backing release. */
int s7d_av1_dpb_reset(struct s7d_av1_dpb *dpb, u64 new_generation,
		      bool physical_idle, u32 *unbound_mask);
/* Terminal teardown, including a final U64_MAX generation; no reader remains. */
int s7d_av1_dpb_release(struct s7d_av1_dpb *dpb, bool physical_idle,
			u32 *unbound_mask);

#endif
