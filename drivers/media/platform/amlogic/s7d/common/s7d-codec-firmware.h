/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_FIRMWARE_H
#define S7D_CODEC_FIRMWARE_H

#include "s7d-codec-types.h"

struct device;
struct module;
struct s7d_codec_lease;
struct s7d_codec_session;
struct s7d_codec_firmware_owner;
struct s7d_codec_firmware_job;

enum s7d_codec_firmware {
	S7D_CODEC_FIRMWARE_H264_MH264,
	/* Non-MMU HEVC (hevc): 8-bit, linear double write 0x10 only. */
	S7D_CODEC_FIRMWARE_HEVC,
	/* MMU HEVC (hevc_mmu_swap): compressed references, Main and Main10. */
	S7D_CODEC_FIRMWARE_HEVC_MMU,
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
/*
 * After a failed TEE load: every engine's later prepare fails. The TEE path is
 * shared, so a broken TA or BL32 breaks all engines, not only the caller's.
 */
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
 * Caller runs a powered, idle engine with @lease; zero acknowledges only the
 * TEE load. Loads from different engines run one at a time.
 * HEVC runtime captures SCRATCH_8 after success under that same lease. Its
 * opaque TEE-supplied value is never a kernel DMA allocation. The selected
 * post-load IQIT and normal CPU-start resets remain within that load sequence.
 * HEVC and AV1 load after every full halt/reset. The VP9 image survives the
 * per-picture block/pipeline reset, so it is not loaded again while the engine
 * still holds it from an earlier load by any session; the backend retains its
 * probability, segmentation, SWAP and MV backing. Any other image's load, a
 * power-up, a fault or a failed load make the next VP9 picture load again.
 * On failure, retain references until DOS halt proves idle.
 */
int s7d_codec_firmware_load(struct device *dev, struct s7d_codec_lease *lease,
			    enum s7d_codec_firmware firmware);
/* The engine @lease runs holds @firmware from an earlier load, and keeps it. */
bool s7d_codec_firmware_resident(struct s7d_codec_lease *lease,
				 enum s7d_codec_firmware firmware);
/* The runner's picture failed: whatever the engine holds is loaded again. */
void s7d_codec_firmware_forget(struct s7d_codec_lease *lease);

/*
 * Set the engine's DMC access policy to nonsecure. The TEE reprograms the DMC
 * for that requester, so call it while the engine's DMC port is still closed.
 */
int s7d_codec_firmware_set_policy(struct device *dev, enum s7d_codec_engine engine);

#endif
