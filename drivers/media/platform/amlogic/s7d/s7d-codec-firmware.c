// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/amlogic/s7d-video-firmware.h>
#include <linux/amlogic/tee.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "s7d-codec-firmware.h"
#include "s7d-codec-session.h"

#define S7D_TEE_FW_H264_MH264	15
#define S7D_TEE_CORE_VDEC		0
#define S7D_TEE_FW_HEVC		16
#define S7D_TEE_CORE_HEVC		2
#define S7D_TEE_FW_JPEG_ENC	21
#define S7D_TEE_CORE_HCODEC	3

struct s7d_codec_firmware_owner {
	struct device *dev;
	struct module *module;
	struct s7d_video_fw_handle *handle;
	atomic_t jobs;
	bool invalid;
};

struct s7d_codec_firmware_job {
	struct kref ref;
	struct s7d_codec_firmware_owner *owner;
	struct s7d_codec_session *session;
	struct device *dev;
	struct module *module;
	struct s7d_video_fw_ticket *ticket;
	u64 lease_epoch;
};

struct s7d_codec_firmware_owner *
s7d_codec_firmware_create(struct device *dev, struct module *module)
{
	struct s7d_codec_firmware_owner *owner;
	struct s7d_video_fw_handle *handle;

	if (!dev)
		return ERR_PTR(-EINVAL);
#ifdef MODULE
	if (module != THIS_MODULE)
		return ERR_PTR(-EINVAL);
#endif
	handle = s7d_video_fw_get(dev);
	if (IS_ERR(handle))
		return ERR_CAST(handle);
	owner = kzalloc(sizeof(*owner), GFP_KERNEL);
	if (!owner) {
		s7d_video_fw_put(handle);
		return ERR_PTR(-ENOMEM);
	}
	owner->dev = get_device(dev);
	owner->module = module;
	owner->handle = handle;
	atomic_set(&owner->jobs, 0);
	return owner;
}

int s7d_codec_firmware_destroy(struct s7d_codec_firmware_owner *owner)
{
	if (atomic_read(&owner->jobs))
		return -EBUSY;
	s7d_video_fw_put(owner->handle);
	put_device(owner->dev);
	kfree(owner);
	return 0;
}

int s7d_codec_firmware_next_epoch(struct s7d_codec_firmware_owner *owner, u64 *out)
{
	if (!out)
		return -EINVAL;
	*out = 0;
	if (!owner)
		return -EINVAL;
	return s7d_video_fw_next_epoch(owner->handle, out);
}

int s7d_codec_firmware_prepare(struct s7d_codec_firmware_owner *owner,
			       struct s7d_codec_session *session, u64 lease_epoch,
			       struct s7d_codec_firmware_job **out)
{
	struct s7d_codec_firmware_job *job;
	struct s7d_video_fw_ticket *ticket;
	int ret;

	if (!session || !lease_epoch || !out)
		return -EINVAL;
	*out = NULL;
	if (owner->invalid)
		return -ESTALE;
	ret = s7d_video_fw_prepare(owner->handle, lease_epoch, &ticket);
	if (ret || !ticket)
		return ret;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		s7d_video_fw_ticket_put(ticket);
		return -ENOMEM;
	}
	if (!try_module_get(owner->module)) {
		s7d_video_fw_ticket_put(ticket);
		kfree(job);
		return -ESHUTDOWN;
	}
	kref_init(&job->ref);
	job->owner = owner;
	job->session = session;
	s7d_codec_session_get(session);
	job->dev = get_device(owner->dev);
	job->module = owner->module;
	job->ticket = ticket;
	job->lease_epoch = lease_epoch;
	atomic_inc(&owner->jobs);
	*out = job;
	return 0;
}

bool s7d_codec_firmware_job_queue(struct s7d_codec_firmware_job *job)
{
	return s7d_video_fw_ticket_queue(job->ticket);
}

void s7d_codec_firmware_job_get(struct s7d_codec_firmware_job *job)
{
	kref_get(&job->ref);
}

static void firmware_job_free(struct kref *ref)
{
	struct s7d_codec_firmware_job *job =
		container_of(ref, struct s7d_codec_firmware_job, ref);

	s7d_video_fw_ticket_put(job->ticket);
	kfree(job);
}

void s7d_codec_firmware_job_put(struct s7d_codec_firmware_job *job)
{
	kref_put(&job->ref, firmware_job_free);
}

void s7d_codec_firmware_job_stop(struct s7d_codec_firmware_job *job)
{
	s7d_video_fw_ticket_cancel(job->ticket);
}

int s7d_codec_firmware_job_join(struct s7d_codec_firmware_job *job)
{
	return s7d_video_fw_ticket_wait(job->ticket, NULL);
}

int s7d_codec_firmware_job_join_result(struct s7d_codec_firmware_job *job,
				       struct s7d_codec_session *session,
				       u64 lease_epoch, bool *cancelled)
{
	bool clean_cancel;
	int ret;

	if (!cancelled)
		return -EINVAL;
	*cancelled = false;
	if (!job || !session || !lease_epoch)
		return -EINVAL;
	ret = s7d_video_fw_ticket_wait(job->ticket, &clean_cancel);
	if (job->session != session || job->lease_epoch != lease_epoch)
		return -ESTALE;
	*cancelled = clean_cancel;
	return ret;
}

void s7d_codec_firmware_invalidate(struct s7d_codec_firmware_owner *owner)
{
	owner->invalid = true;
}

void s7d_codec_firmware_job_release_custody(struct s7d_codec_firmware_job *job)
{
	struct s7d_codec_firmware_owner *owner = job->owner;
	struct module *module = job->module;

	s7d_codec_session_put(job->session);
	job->session = NULL;
	put_device(job->dev);
	job->dev = NULL;
	job->owner = NULL;
	job->module = NULL;
	atomic_dec(&owner->jobs);
	module_put(module);
}

static int firmware_load_hevc(struct device *dev)
{
	int ret;

	if (!tee_enabled())
		return -EOPNOTSUPP;

	ret = tee_config_device_state_lock(DMC_DEV_ID_HEVC, 0);
	if (ret != TEEC_SUCCESS) {
		dev_err(dev, "HEVC nonsecure policy failed, TEE status %#x\n", (u32)ret);
		return -EIO;
	}

	ret = tee_load_video_fw_swap(S7D_TEE_FW_HEVC, S7D_TEE_CORE_HEVC, true);
	if (ret != TEEC_SUCCESS) {
		dev_err(dev, "HEVC firmware load failed, TEE status %#x\n", (u32)ret);
		return -EIO;
	}

	return 0;
}

static int firmware_load_jpeg(struct device *dev)
{
	int ret;

	if (!tee_enabled())
		return -EOPNOTSUPP;

	ret = tee_config_device_state_lock(DMC_DEV_ID_HCODEC, 0);
	if (ret != TEEC_SUCCESS) {
		dev_err(dev, "HCODEC nonsecure policy failed, TEE status %#x\n", (u32)ret);
		return -EIO;
	}

	ret = tee_load_video_fw(S7D_TEE_FW_JPEG_ENC, S7D_TEE_CORE_HCODEC);
	if (ret != TEEC_SUCCESS) {
		dev_err(dev, "JPEG firmware load failed, TEE status %#x\n", (u32)ret);
		return -EIO;
	}

	return 0;
}

int s7d_codec_firmware_load(struct device *dev, enum s7d_codec_engine engine,
			    enum s7d_codec_firmware firmware)
{
	int ret;

	if (!dev)
		return -EINVAL;
	if (engine == S7D_CODEC_ENGINE_HEVC && firmware == S7D_CODEC_FIRMWARE_HEVC)
		return firmware_load_hevc(dev);
	if (engine == S7D_CODEC_ENGINE_HCODEC && firmware == S7D_CODEC_FIRMWARE_JPEG_ENC)
		return firmware_load_jpeg(dev);
	if (engine != S7D_CODEC_ENGINE_VDEC ||
	    firmware != S7D_CODEC_FIRMWARE_H264_MH264)
		return -EOPNOTSUPP;
	if (!tee_enabled())
		return -EOPNOTSUPP;

	ret = tee_config_device_state_lock(DMC_DEV_ID_VDEC, 0);
	if (ret != TEEC_SUCCESS) {
		dev_err(dev, "VDEC nonsecure policy failed, TEE status %#x\n", (u32)ret);
		return -EIO;
	}

	ret = tee_load_video_fw(S7D_TEE_FW_H264_MH264, S7D_TEE_CORE_VDEC);
	if (ret != TEEC_SUCCESS) {
		dev_err(dev, "H.264 firmware load failed, TEE status %#x\n", (u32)ret);
		return -EIO;
	}

	return 0;
}

int s7d_codec_firmware_prepare_for_engine(struct s7d_codec_firmware_owner *owner,
					struct s7d_codec_session *session,
					u64 lease_epoch,
					enum s7d_codec_engine engine,
					enum s7d_codec_firmware firmware,
					struct s7d_codec_firmware_job **out)
{
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!owner)
		return -EINVAL;
	if (!((engine == S7D_CODEC_ENGINE_VDEC &&
	       firmware == S7D_CODEC_FIRMWARE_H264_MH264) ||
	      (engine == S7D_CODEC_ENGINE_HEVC &&
	       firmware == S7D_CODEC_FIRMWARE_HEVC) ||
	      (engine == S7D_CODEC_ENGINE_HCODEC &&
	       firmware == S7D_CODEC_FIRMWARE_JPEG_ENC)))
		return -EOPNOTSUPP;

	return s7d_codec_firmware_prepare(owner, session, lease_epoch, out);
}
