/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_HEVC_CONTEXT_H
#define S7D_HEVC_CONTEXT_H

#include <linux/types.h>

#define S7D_HEVC_INPUT_MAX_BYTES (2U * 1024U * 1024U)

struct regmap;
struct s7d_codec_session;
struct s7d_hevc_sps;
struct s7d_hevc_context;

struct s7d_hevc_dma_region {
	u32 address;
	u32 bytes;
};

struct s7d_hevc_workspace {
	struct s7d_hevc_dma_region ipp;
	struct s7d_hevc_dma_region sao_abv;
	struct s7d_hevc_dma_region sao_vb;
	struct s7d_hevc_dma_region short_term_rps;
	struct s7d_hevc_dma_region vps;
	struct s7d_hevc_dma_region sps;
	struct s7d_hevc_dma_region pps;
	struct s7d_hevc_dma_region sao_up;
	struct s7d_hevc_dma_region swap1;
	/* Reserved workspace only; never the TEE-owned SCRATCH_8 value. */
	struct s7d_hevc_dma_region swap2;
	struct s7d_hevc_dma_region scalelut;
	struct s7d_hevc_dma_region dblk_para;
	struct s7d_hevc_dma_region dblk_data;
	struct s7d_hevc_dma_region dblk_data2;
	struct s7d_hevc_dma_region mpred_above;
	struct s7d_hevc_dma_region rpm;
	struct s7d_hevc_dma_region lmem;
	struct s7d_hevc_dma_region aux;
	struct s7d_hevc_dma_region mv;
	u32 aux_prefix_bytes;
	u32 aux_suffix_bytes;
};

struct s7d_hevc_input_plan {
	u32 start;
	/* HEVC ring uses an exclusive end, unlike the VLD FIFO. */
	u32 end;
	u32 read_pointer;
	u32 write_pointer;
	u32 decode_size;
	u32 max_shift_bytes;
	u64 token;
};

/* VPS/SPS/PPS/one first-slice IDR19/20, layer0/temporal id1, SPS ID < 8. */
int s7d_hevc_input_validate_annexb(const u8 *data, size_t bytes,
				   struct s7d_hevc_sps *sps);
/* Pure allocation: caller already owns an OPEN HEVC session. */
struct s7d_hevc_context *
s7d_hevc_context_create(struct s7d_codec_session *session,
			const struct s7d_hevc_sps *sps, size_t input_capacity);
/* Join all users first. Session core owns DMA free/quarantine after quiesce. */
void s7d_hevc_context_release(struct s7d_hevc_context *context);
const struct s7d_hevc_workspace *
s7d_hevc_context_workspace(const struct s7d_hevc_context *context);
/* Copy before validating; immutable single AU, only one successful stage. */
int s7d_hevc_context_stage(struct s7d_hevc_context *context,
			   const u8 *data, size_t bytes, u64 token,
			   struct s7d_hevc_input_plan *plan);
/* Serialized calls: parent proves CPU/fetch stopped; four addresses only. */
int s7d_hevc_context_program_input(struct s7d_hevc_context *context,
				   struct regmap *map);
/* Parent proves actual firmware pause and no RPM writer, on the same lease. */
int s7d_hevc_context_snapshot_rpm(struct s7d_hevc_context *context,
				  void *snapshot, size_t bytes);

#endif
