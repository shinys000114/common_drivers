/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_INPUT_H
#define S7D_HEVC_INPUT_H

#include "s7d-hevc-sequence.h"

#define S7D_HEVC_RPS_ENTRIES 16U
#define S7D_HEVC_PARAMETER_BYTES (64U * 1024U)
#define S7D_HEVC_AU_MAX_BYTES (2U * 1024U * 1024U)

struct s7d_hevc_rps_entry {
	s32 delta_poc;
	bool used;
	bool long_term;
};

struct s7d_hevc_slice_info {
	u32 nal_type;
	u32 temporal_id_plus1;
	u32 slice_type;
	u32 poc_lsb;
	u32 slice_address;
	u32 rps_count;
	struct s7d_hevc_rps_entry rps[S7D_HEVC_RPS_ENTRIES];
	u32 active[2];
	u32 modification_index[2][S7D_HEVC_RPS_ENTRIES];
	u32 collocated_ref_idx;
	bool first_slice;
	bool dependent_slice;
	bool temporal_mvp;
	bool collocated_from_l0;
	bool no_output_of_prior_pics;
	bool pic_output_flag;
	bool modification[2];
	u64 parameter_epoch;
};

struct s7d_hevc_pps {
	u32 pps_id;
	u32 sps_id;
	u32 extra_slice_header_bits;
	u32 default_active[2];
	s32 init_qp_minus26;
	s32 cb_qp_offset;
	s32 cr_qp_offset;
	s32 beta_offset_div2;
	s32 tc_offset_div2;
	u32 log2_parallel_merge_level;
	u32 diff_cu_qp_delta_depth;
	bool dependent_slices;
	bool output_flag_present;
	bool cabac_init_present;
	bool cu_qp_delta_enabled;
	bool scaling_list_data_present;
	bool slice_chroma_qp_offsets_present;
	bool weighted_pred;
	bool weighted_bipred;
	bool tiles_enabled;
	bool entropy_coding_sync;
	bool loop_filter_across_slices;
	bool deblocking_control_present;
	bool deblocking_override_enabled;
	bool deblocking_disabled;
	bool lists_modification_present;
	bool slice_header_extension_present;
};

enum s7d_hevc_input_kind {
	S7D_HEVC_INPUT_HEADERS,
	S7D_HEVC_INPUT_PICTURE,
};

struct s7d_hevc_input_result {
	const u8 *data;
	size_t bytes;
	enum s7d_hevc_input_kind kind;
	const struct s7d_hevc_sequence *sequence;
	const struct s7d_hevc_pps *pps;
	struct s7d_hevc_slice_info slice;
	u32 header_rbsp_bytes;
	u32 entry_points;
	u32 cabac_rbsp_bytes;
};

struct s7d_hevc_input_bank;
struct s7d_hevc_input;

struct s7d_hevc_input_bank *s7d_hevc_input_bank_create(void);
void s7d_hevc_input_bank_destroy(struct s7d_hevc_input_bank *bank);
/* Caller serializes bank operations; failed prepare changes neither bank nor out. */
int s7d_hevc_input_prepare(const struct s7d_hevc_input_bank *bank,
			   const u8 *annexb, size_t bytes,
			   struct s7d_hevc_input **out);
/* Commit only a successfully prepared input from the unchanged bank revision. */
int s7d_hevc_input_commit(struct s7d_hevc_input_bank *bank,
			  struct s7d_hevc_input *input);
/* View and its immutable AU/typed parameters live until input_release. */
const struct s7d_hevc_input_result *
s7d_hevc_input_view(const struct s7d_hevc_input *input);
/* Selected raw VPS/SPS/PPS snapshot, each with its four-byte Annex B prefix. */
size_t s7d_hevc_input_prefix_bytes(const struct s7d_hevc_input *input);
int s7d_hevc_input_copy_prefix(const struct s7d_hevc_input *input,
			       u8 *destination, size_t capacity);
void s7d_hevc_input_release(struct s7d_hevc_input *input);

#endif
