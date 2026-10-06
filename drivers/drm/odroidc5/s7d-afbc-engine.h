/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#ifndef __S7D_AFBC_ENGINE_H
#define __S7D_AFBC_ENGINE_H

#include "s7d-afbc.h"

enum s7d_afbc_engine_phase {
	S7D_AFBC_STOPPED,
	S7D_AFBC_PREPARED,
	S7D_AFBC_RUNNING,
	S7D_AFBC_STOPPING,
	S7D_AFBC_ERROR,
};

struct s7d_afbc_engine_io {
	int (*read)(void *data, u32 reg, u32 *value);
	int (*write)(void *data, u32 reg, u32 value);
	int (*check_start)(void *data, u64 sequence, u8 field);
};

struct s7d_afbc_observation {
	u32 raw;
	u32 status;
	u32 top;
	u32 surfaces;
};

struct s7d_afbc_frame {
	u64 sequence;
	u8 field;
	bool vblank;
	bool idle;
	bool window;
};

struct s7d_afbc_engine {
	const struct s7d_afbc_engine_io *io;
	void *data;
	enum s7d_afbc_engine_phase phase;
	struct s7d_afbc_state bound;
	struct s7d_afbc_state pending;
	struct s7d_afbc_observation observed;
	struct s7d_afbc_frame sampled;
	u64 generation;
	u64 pending_generation;
	u64 accepted_generation;
	u64 completed_generation;
	u64 completed_generation_epoch;
	u64 epoch;
	u64 sampled_epoch;
	u64 completed_epoch;
	u64 started_sequence;
	u64 last_start_sequence;
	u64 failed_generation;
	u64 failed_epoch;
	enum s7d_afbc_engine_phase failed_phase;
	u32 failed_reg;
	u32 failed_expected;
	u32 failed_observed;
	int last_error;
	u8 started_field;
	bool bound_valid;
	bool pending_valid;
	bool pending_ready;
	bool started_valid;
	bool last_start_valid;
	bool sampled_valid;
	bool swapped;
	bool readout;
	bool stop_unstarted;
	bool stop_decode_done;
	bool failed_readback;
};

/* VPU owns MMIO/PM and serializes calls; scanout retains every framebuffer. */
bool s7d_afbc_same_layout(const struct s7d_afbc_state *a,
			 const struct s7d_afbc_state *b);
void s7d_afbc_engine_init(struct s7d_afbc_engine *e,
			  const struct s7d_afbc_engine_io *io, void *data);
int s7d_afbc_engine_fail(struct s7d_afbc_engine *e, int error, u32 reg);
int s7d_afbc_engine_prepare(struct s7d_afbc_engine *e,
			     const struct s7d_afbc_state *plan, u64 generation);
int s7d_afbc_engine_start_initial(struct s7d_afbc_engine *e);
int s7d_afbc_engine_stage(struct s7d_afbc_engine *e,
			   const struct s7d_afbc_state *plan, u64 generation);
int s7d_afbc_engine_cancel_stage(struct s7d_afbc_engine *e, u64 generation);
int s7d_afbc_engine_rdma_drained(struct s7d_afbc_engine *e, u64 generation);
int s7d_afbc_engine_read(struct s7d_afbc_engine *e,
			  struct s7d_afbc_observation *out);
int s7d_afbc_engine_sample(struct s7d_afbc_engine *e,
			    const struct s7d_afbc_frame *frame,
			    const struct s7d_afbc_observation *observation);
int s7d_afbc_engine_restart(struct s7d_afbc_engine *e,
			     const struct s7d_afbc_frame *frame);
int s7d_afbc_engine_stop_begin(struct s7d_afbc_engine *e);
int s7d_afbc_engine_stop_sample(struct s7d_afbc_engine *e,
				 const struct s7d_afbc_observation *observation);
/* Call after native RDMA/OSD drain; STOPPED alone does not retire buffers. */
int s7d_afbc_engine_stop_finish(struct s7d_afbc_engine *e);
bool s7d_afbc_engine_can_abort_prepare(const struct s7d_afbc_engine *e);
/* First config failure only; caller has drained native RDMA/OSD reads. */
int s7d_afbc_engine_abort_prepare(struct s7d_afbc_engine *e, u32 *failed_reg);

#endif
