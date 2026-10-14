/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_VDEC_RUNTIME_H
#define S7D_VDEC_RUNTIME_H

#include "s7d-codec-session.h"

struct device;
struct module;
struct regmap;
struct mutex;
struct s7d_codec_dos;
struct s7d_codec_firmware_owner;
struct s7d_vdec_runtime;
struct s7d_h264_input;
struct s7d_h264_dpb;

/* IRQ callbacks and drained work never call core stop or runtime lifecycle. */
/* Backend owns protocol/FIFO only, never DOS reset/gates/DMC or mailbox ACK. */
struct s7d_vdec_runtime_backend_ops {
	int (*stage)(void *priv, struct s7d_codec_session *session,
		     struct regmap *map, bool *cancelled);
	int (*configure)(void *priv, struct regmap *map, bool *cancelled);
	int (*enable_input)(void *priv, struct regmap *map, bool *cancelled);
	int (*kick)(void *priv, struct regmap *map, bool *cancelled);
	/* Hard IRQ after ACK: zero handled, one wakes thread, negative is error. */
	int (*irq_snapshot)(void *priv, struct regmap *map);
	int (*irq_thread)(void *priv, struct regmap *map, int error);
	/* block holds transaction; drain waits with transaction released. */
	void (*block)(void *priv);
	int (*drain)(void *priv);
	/* Process executor only; each callback owns only its short transaction. */
	int (*save_context)(void *priv, struct regmap *map,
			    struct s7d_codec_session *session, u64 input_token,
			    u64 write_token, bool *cancelled);
	int (*restore_context)(void *priv, struct regmap *map,
			       struct s7d_codec_session *session, u64 input_token,
			       u64 write_token, u64 park_epoch, u64 next_input_token,
			       bool *cancelled);
};

/* DOS, shared firmware owner and backend storage outlive this borrower. */
/* Destroy the core and prevent new callbacks before destroying the runtime. */
struct s7d_vdec_runtime *
s7d_vdec_runtime_create(struct device *dev, struct module *owner,
			struct s7d_codec_dos *dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			const struct s7d_vdec_runtime_backend_ops *ops, void *priv,
			struct mutex *transaction);
int s7d_vdec_runtime_destroy(struct s7d_vdec_runtime *runtime);
const struct s7d_codec_resource_ops *s7d_vdec_runtime_resource_ops(void);

/* Cancellation is tagged only by an observed stop gate, not inferred from errno. */
/* Caller holds this core's session and live backing throughout preparation. */
int s7d_vdec_runtime_prepare(struct s7d_vdec_runtime *runtime,
			     struct s7d_codec_session *session, bool *cancelled);
/* Nonblocking; caller holds the original session and no runtime control lock. */
void s7d_vdec_runtime_request_preload_stop(struct s7d_vdec_runtime *runtime,
					 struct s7d_codec_session *session);
int s7d_vdec_runtime_start(struct s7d_vdec_runtime *runtime, bool *cancelled);

/* Call lifecycle without transaction held; live input/DPB storage stays borrowed. */
int s7d_vdec_runtime_park(struct s7d_vdec_runtime *runtime,
			  struct s7d_codec_session *session,
			  struct s7d_h264_input *input, struct s7d_h264_dpb *dpb,
			  u64 input_token, u64 write_token, u64 *park_epoch,
			  bool *cancelled);
int s7d_vdec_runtime_check_parked(struct s7d_vdec_runtime *runtime,
				  struct s7d_codec_session *session,
				  u64 input_token, u64 write_token, u64 park_epoch);
/* Confirm context and retire the matching DPB/input before starting next AU. */
int s7d_vdec_runtime_resume(struct s7d_vdec_runtime *runtime,
			    struct s7d_codec_session *session, u64 input_token,
			    u64 write_token, u64 park_epoch, u64 next_input_token,
			    bool *cancelled);

/* Caller gates preparation for its active lease; no native DMA has started. */
int s7d_vdec_runtime_join_unprepared(struct s7d_vdec_runtime *runtime);

/* Requires the caller's live session; only the same stopped lease qualifies. */
int s7d_vdec_runtime_check_quiesced(struct s7d_vdec_runtime *runtime,
				    struct s7d_codec_session *session);
int s7d_vdec_runtime_retry_release(struct s7d_vdec_runtime *runtime);
int s7d_vdec_runtime_suspend_check(struct s7d_vdec_runtime *runtime);

#endif
