// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/amlogic/tee.h>

#include "s7d-codec-bundle.h"
#include "s7d-codec-firmware.h"
#include "s7d-codec-session.h"
#include "s7d-codec-tee.h"

#define S7D_TEE_FW_H264_MH264	15
#define S7D_TEE_CORE_VDEC		0
#define S7D_TEE_FW_HEVC		16
#define S7D_TEE_CORE_HEVC		2
#define S7D_TEE_FW_JPEG_ENC	21
#define S7D_TEE_CORE_HCODEC	3

enum s7d_codec_firmware_state {
	S7D_FIRMWARE_EMPTY,
	S7D_FIRMWARE_PRE_FAILED,
	S7D_FIRMWARE_READY,
	S7D_FIRMWARE_FAILED,
	S7D_FIRMWARE_INVALID,
};

struct s7d_codec_firmware_owner {
	struct device *dev;
	struct module *module;
	struct workqueue_struct *queue;
	const struct firmware *firmware;
	atomic_t jobs;
	/* Protects only lease_sequence, not cache or consumer lifetime. */
	struct mutex epoch_lock;
	u64 lease_sequence;
	enum s7d_codec_firmware_state state;
	u64 failed_epoch;
	u32 cancel_sequence;
	int error;
};

struct s7d_codec_firmware_job {
	struct kref ref;
	struct work_struct work;
	struct completion done;
	struct s7d_codec_firmware_owner *owner;
	struct s7d_codec_session *session;
	struct device *dev;
	struct module *module;
	const struct firmware *firmware;
	struct s7d_codec_tee_result result;
	atomic_t cancel;
	u64 lease_epoch;
	u32 cancel_id;
	int status;
	bool retired;
	bool cancelled;
};

static void firmware_job_work(struct work_struct *work)
{
	struct s7d_codec_firmware_job *job =
		container_of(work, struct s7d_codec_firmware_job, work);
	struct s7d_codec_bundle_view view;
	int ret;

	if (atomic_read(&job->cancel)) {
		job->cancelled = true;
		ret = -ECANCELED;
	} else {
		ret = request_firmware_direct(&job->firmware, "video/video_ucode.bin", job->dev);
	}
	if (!ret) {
		if (atomic_read(&job->cancel)) {
			job->cancelled = true;
			ret = -ECANCELED;
		} else {
			ret = s7d_codec_bundle_parse(job->firmware->data, job->firmware->size, &view);
		}
	}
	if (!ret)
		ret = s7d_codec_tee_upload(job->dev, &view, job->cancel_id,
					 &job->cancel, &job->result);
	job->status = ret;
	complete_all(&job->done);
}

struct s7d_codec_firmware_owner *
s7d_codec_firmware_create(struct device *dev, struct module *module)
{
	struct s7d_codec_firmware_owner *owner;

	if (!dev)
		return ERR_PTR(-EINVAL);
#ifdef MODULE
	if (module != THIS_MODULE)
		return ERR_PTR(-EINVAL);
#endif
	owner = kzalloc(sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return ERR_PTR(-ENOMEM);
	owner->queue = alloc_ordered_workqueue("s7d-codec-preload", WQ_MEM_RECLAIM);
	if (!owner->queue) {
		kfree(owner);
		return ERR_PTR(-ENOMEM);
	}
	owner->dev = get_device(dev);
	owner->module = module;
	atomic_set(&owner->jobs, 0);
	mutex_init(&owner->epoch_lock);
	return owner;
}

int s7d_codec_firmware_destroy(struct s7d_codec_firmware_owner *owner)
{
	if (atomic_read(&owner->jobs))
		return -EBUSY;
	destroy_workqueue(owner->queue);
	release_firmware(owner->firmware);
	put_device(owner->dev);
	kfree(owner);
	return 0;
}

int s7d_codec_firmware_next_epoch(struct s7d_codec_firmware_owner *owner, u64 *out)
{
	int ret = 0;

	if (!out)
		return -EINVAL;
	*out = 0;
	if (!owner)
		return -EINVAL;
	mutex_lock(&owner->epoch_lock);
	if (owner->lease_sequence == U64_MAX)
		ret = -EOVERFLOW;
	else
		*out = ++owner->lease_sequence;
	mutex_unlock(&owner->epoch_lock);
	return ret;
}

int s7d_codec_firmware_prepare(struct s7d_codec_firmware_owner *owner,
			       struct s7d_codec_session *session, u64 lease_epoch,
			       struct s7d_codec_firmware_job **out)
{
	struct s7d_codec_firmware_job *job;

	if (!session || !lease_epoch || !out)
		return -EINVAL;
	*out = NULL;
	if (owner->state == S7D_FIRMWARE_READY)
		return 0;
	if (owner->state == S7D_FIRMWARE_FAILED || owner->state == S7D_FIRMWARE_INVALID ||
	    (owner->state == S7D_FIRMWARE_PRE_FAILED && owner->failed_epoch == lease_epoch))
		return owner->error;
	if (owner->cancel_sequence == U32_MAX)
		return -EOVERFLOW;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	if (!try_module_get(owner->module)) {
		kfree(job);
		return -ESHUTDOWN;
	}
	kref_init(&job->ref);
	INIT_WORK(&job->work, firmware_job_work);
	init_completion(&job->done);
	atomic_set(&job->cancel, 0);
	job->owner = owner;
	job->session = session;
	s7d_codec_session_get(session);
	job->dev = get_device(owner->dev);
	job->module = owner->module;
	job->lease_epoch = lease_epoch;
	job->cancel_id = ++owner->cancel_sequence;
	job->status = -EINPROGRESS;
	atomic_inc(&owner->jobs);
	*out = job;
	return 0;
}

bool s7d_codec_firmware_job_queue(struct s7d_codec_firmware_job *job)
{
	return queue_work(job->owner->queue, &job->work);
}

void s7d_codec_firmware_job_get(struct s7d_codec_firmware_job *job)
{
	kref_get(&job->ref);
}

static void firmware_job_free(struct kref *ref)
{
	kfree(container_of(ref, struct s7d_codec_firmware_job, ref));
}

void s7d_codec_firmware_job_put(struct s7d_codec_firmware_job *job)
{
	kref_put(&job->ref, firmware_job_free);
}

void s7d_codec_firmware_job_stop(struct s7d_codec_firmware_job *job)
{
	atomic_set(&job->cancel, 1);
}

int s7d_codec_firmware_job_join(struct s7d_codec_firmware_job *job)
{
	wait_for_completion(&job->done);
	flush_work(&job->work);
	return job->status;
}

int s7d_codec_firmware_job_join_result(struct s7d_codec_firmware_job *job,
				       struct s7d_codec_session *session,
				       u64 lease_epoch, bool *cancelled)
{
	int ret;

	if (!cancelled)
		return -EINVAL;
	*cancelled = false;
	if (!job || !session || !lease_epoch)
		return -EINVAL;
	ret = s7d_codec_firmware_job_join(job);
	if (job->session != session || job->lease_epoch != lease_epoch)
		return -ESTALE;
	if (ret != -ECANCELED)
		return ret;
	if (job->result.linux_status)
		return job->result.linux_status < 0 ? job->result.linux_status : -EIO;
	if (job->result.close_status)
		return job->result.close_status < 0 ? job->result.close_status : -EIO;
	*cancelled = job->cancelled || job->result.cancelled;
	return ret;
}

void s7d_codec_firmware_retire(struct s7d_codec_firmware_owner *owner,
			       struct s7d_codec_firmware_job *job)
{
	if (job->retired)
		return;
	job->retired = true;
	if (!job->status && job->result.complete) {
		owner->firmware = job->firmware;
		job->firmware = NULL;
		owner->state = S7D_FIRMWARE_READY;
		owner->error = 0;
		return;
	}
	owner->error = job->status ? job->status : -EIO;
	owner->failed_epoch = job->lease_epoch;
	owner->state = job->result.phase == S7D_CODEC_TEE_NONE ?
		S7D_FIRMWARE_PRE_FAILED : S7D_FIRMWARE_FAILED;
}

void s7d_codec_firmware_invalidate(struct s7d_codec_firmware_owner *owner)
{
	if (owner->state == S7D_FIRMWARE_READY) {
		owner->state = S7D_FIRMWARE_INVALID;
		owner->error = -ESTALE;
	}
}

void s7d_codec_firmware_job_release_custody(struct s7d_codec_firmware_job *job)
{
	struct s7d_codec_firmware_owner *owner = job->owner;
	struct module *module = job->module;

	release_firmware(job->firmware);
	job->firmware = NULL;
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
