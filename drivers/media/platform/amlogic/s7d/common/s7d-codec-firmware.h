/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_FIRMWARE_H
#define S7D_CODEC_FIRMWARE_H

#include "s7d-codec-types.h"

struct device;
struct module;
struct s7d_codec_session;
struct s7d_codec_firmware_owner;
struct s7d_codec_firmware_job;

enum s7d_codec_firmware {
	S7D_CODEC_FIRMWARE_H264_MH264,
	/* Non-MMU HEVC; the initial backend selects Main8/DW 0x10. */
	S7D_CODEC_FIRMWARE_HEVC,
	S7D_CODEC_FIRMWARE_VP9,
	S7D_CODEC_FIRMWARE_JPEG_ENC,
	S7D_CODEC_FIRMWARE_AV1_MMU,
};

struct s7d_codec_firmware_owner *
s7d_codec_firmware_create(struct device *dev, struct module *module);
int s7d_codec_firmware_destroy(struct s7d_codec_firmware_owner *owner);

/*
 * Allocate once after acquiring a new DOS lease; retain the cookie throughout
 * that lease, including preload failure, cancellation, retry and resume.
 * All engines use the provider allocator, including after decoder reload.
 * Caller keeps owner alive and may sleep. A valid out is zero on error; wrap
 * returns -EOVERFLOW.
 */
int s7d_codec_firmware_next_epoch(struct s7d_codec_firmware_owner *owner, u64 *out);

int s7d_codec_firmware_prepare(struct s7d_codec_firmware_owner *owner,
			       struct s7d_codec_session *session, u64 lease_epoch,
			       struct s7d_codec_firmware_job **job);
/*
 * Validate the engine/image pair, then prepare the same whole-bundle cache.
 * READY acknowledges archive upload only, not an engine firmware load.
 */
int s7d_codec_firmware_prepare_for_engine(struct s7d_codec_firmware_owner *owner,
					struct s7d_codec_session *session,
					u64 lease_epoch,
					enum s7d_codec_engine engine,
					enum s7d_codec_firmware firmware,
					struct s7d_codec_firmware_job **job);
void s7d_codec_firmware_invalidate(struct s7d_codec_firmware_owner *owner);

bool s7d_codec_firmware_job_queue(struct s7d_codec_firmware_job *job);
void s7d_codec_firmware_job_get(struct s7d_codec_firmware_job *job);
void s7d_codec_firmware_job_put(struct s7d_codec_firmware_job *job);
void s7d_codec_firmware_job_stop(struct s7d_codec_firmware_job *job);
/* Join without runtime control or backend transaction held. */
int s7d_codec_firmware_job_join(struct s7d_codec_firmware_job *job);
/* Joined result for the exact owner; cancellation never hides Linux/close faults. */
int s7d_codec_firmware_job_join_result(struct s7d_codec_firmware_job *job,
				       struct s7d_codec_session *session,
				       u64 lease_epoch, bool *cancelled);
/* Only the creator releases custody, outside runtime/backend lifecycle locks. */
void s7d_codec_firmware_job_release_custody(struct s7d_codec_firmware_job *job);

/*
 * Caller holds a powered, idle engine; zero acknowledges only the TEE load.
 * HEVC runtime captures SCRATCH_8 after success under that same lease. Its
 * opaque TEE-supplied value is never a kernel DMA allocation. The selected
 * post-load IQIT and normal CPU-start resets remain within that load sequence.
 * HEVC reloads after full halt/reset. VP9 may retain a successful image across
 * the selected per-job block/pipeline reset within one powered, exclusive
 * lease; the backend retains probability, segmentation, SWAP and MV backing. Both
 * reload after power loss, firmware replacement, lease change or load failure.
 * On failure, retain references until DOS halt proves idle.
 */
int s7d_codec_firmware_load(struct device *dev, enum s7d_codec_engine engine,
			    enum s7d_codec_firmware firmware);

/*
 * Set the engine's DMC access policy to nonsecure. The TEE reprograms the DMC
 * for that requester, so call it while the engine's DMC port is still closed.
 */
int s7d_codec_firmware_set_policy(struct device *dev, enum s7d_codec_engine engine);

#endif
