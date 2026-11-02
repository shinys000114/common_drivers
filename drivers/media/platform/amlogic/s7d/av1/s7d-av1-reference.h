/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_REFERENCE_H
#define S7D_AV1_REFERENCE_H

#include "s7d-av1-rpm.h"

#define S7D_AV1_PARSER_REFERENCE_WORDS 12U

struct s7d_codec_session;
struct s7d_codec_dma;
struct s7d_av1_references;

struct s7d_av1_reference_backing {
	struct s7d_av1_picture_id id;
	struct s7d_av1_region body;
	struct s7d_av1_region header;
	struct s7d_av1_region mv;
	/* NV12 double write: luma, then interleaved CbCr, both at the pitch. */
	struct s7d_av1_region luma;
	struct s7d_av1_region chroma;
	struct s7d_av1_region cdf;
	struct s7d_av1_region segment_map;
	u32 pitch;
};

struct s7d_av1_reference_state {
	s8 ref_delta[8];
	s8 mode_delta[2];
	u16 seg_lf_y[8];
	u16 seg_lf_c[8];
	u32 segment_feature[8];
	u32 mi_rows;
	u32 mi_cols;
	u32 seg_mi_rows;
	u32 seg_mi_cols;
	bool segmentation_enabled;
	bool segmentation_update_map;
	bool prev_segmentation_enabled;
	bool intra_only;
};

struct s7d_av1_reference_picture {
	struct s7d_av1_dpb_plan plan;
	const struct s7d_av1_reference_backing *backing;
	u32 count;
	const struct s7d_av1_reference_state *primary;
	struct s7d_av1_reference_state *state;
};

/*
 * The caller memory (a CAPTURE buffer) a decoded picture is written to: its
 * NV12 double write, and with AM08C/AM10C (fbc) its FBC header and body too.
 * Otherwise the slot keeps the FBC header and body itself; the MV always stays
 * with the slot.
 */
struct s7d_av1_slot_memory {
	/* Nonzero caller handle, reported back once the DPB lets go of the slot. */
	u64 cookie;
	/* fbc only; zero otherwise. */
	struct s7d_av1_region header;
	struct s7d_av1_region body;
	struct s7d_av1_region luma;
	struct s7d_av1_region chroma;
};

struct s7d_av1_reference_output {
	struct s7d_av1_dpb_output output;
	u32 pitch;
	/* The cookie of the memory bound to the shown slot. */
	u64 cookie;
};

/*
 * Caller retains the workspace owner; halt/join precedes initial allocation.
 * Each decoded picture brings its memory; with fbc the slots allocate only
 * their MV, otherwise also the FBC header and body.
 */
struct s7d_av1_references *
s7d_av1_references_create(struct s7d_codec_session *session,
			  const struct s7d_av1_workspace *workspace,
			  const struct s7d_av1_sequence *sequence,
			  u64 lease_epoch, int halt_status, bool joined, bool fbc);
bool s7d_av1_references_fbc(const struct s7d_av1_references *references);
/*
 * Reserve/recycle before the run's first protocol/CPU MMIO, while fully idle.
 * Binds @memory (NULL for a show-existing frame) to the reserved slot; unowned
 * slots must have been unbound first.
 */
int s7d_av1_references_run_begin(struct s7d_av1_references *references,
				 u64 lease_epoch, u64 input_token,
				 const struct s7d_av1_slot_memory *memory);
/* Header pause is not idle; this consumes only the pre-reserved backing. */
int s7d_av1_references_prepare(struct s7d_av1_references *references,
			       u64 lease_epoch, u64 input_token,
			       const struct s7d_av1_header *header,
			       const struct s7d_av1_dpb_metadata *metadata,
			       struct s7d_av1_reference_picture *picture);
/* Only after all picture MMIO and the actual feature snapshot succeeded. */
int s7d_av1_references_configured(struct s7d_av1_references *references,
				  u64 lease_epoch, u64 input_token);
/* completed requires matching picture IRQ or validated show-existing. */
int s7d_av1_references_finish(struct s7d_av1_references *references,
			      u64 lease_epoch, u64 input_token,
			      int halt_status, bool joined, bool completed);
int s7d_av1_references_output(struct s7d_av1_references *references,
			      struct s7d_av1_reference_output *output);
/* The copy/publication and every CPU reader have finished. */
int s7d_av1_references_output_committed(struct s7d_av1_references *references,
					const struct s7d_av1_reference_output *output);
/*
 * Idle: drop the memory of every slot the DPB (references, a pending output)
 * no longer owns; returns their cookies. The hardware reads no
 * unowned slot again, so the caller may reuse that memory at once.
 */
int s7d_av1_references_unbind(struct s7d_av1_references *references,
			      u64 cookies[S7D_AV1_DPB_MAX_SLOTS], u32 *count);
int s7d_av1_references_parser_words(const struct s7d_av1_references *references,
				    u64 lease_epoch,
				    u32 words[S7D_AV1_PARSER_REFERENCE_WORDS]);
/* Output pins must be published/released; backing stays owned until core teardown. */
int s7d_av1_references_destroy(struct s7d_av1_references *references,
			       u64 lease_epoch, int halt_status, bool joined);

#endif
