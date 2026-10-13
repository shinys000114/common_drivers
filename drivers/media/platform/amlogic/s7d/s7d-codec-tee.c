// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/amlogic/tee_drv.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/uuid.h>

#include "s7d-codec-bundle.h"
#include "s7d-codec-tee.h"

#define S7D_CODEC_TEE_BUSY	0xffff000d

static int s7d_codec_tee_match(struct tee_ioctl_version_data *version,
			     const void *data)
{
	return version->impl_id == TEE_IMPL_ID_OPTEE &&
	       (version->gen_caps & TEE_GEN_CAP_GP);
}

static bool s7d_codec_tee_cancelled(const atomic_t *cancel,
				   struct s7d_codec_tee_result *result)
{
	if (!atomic_read(cancel))
		return false;
	result->cancelled = true;
	return true;
}

static int s7d_codec_tee_status(struct device *dev,
				struct s7d_codec_tee_result *result,
				enum s7d_codec_tee_phase phase, int ret,
				u32 status, u32 origin)
{
	result->phase = phase;
	result->linux_status = ret;
	result->tee_status = status;
	result->tee_origin = origin;
	result->tee_status_valid = !ret;
	if (!ret)
		return 0;
	dev_err(dev, "TEE preload phase %u failed: linux=%d\n", phase, ret);
	return ret < 0 ? ret : -EIO;
}

int s7d_codec_tee_upload(struct device *dev,
			 const struct s7d_codec_bundle_view *view, u32 cancel_id,
			 const atomic_t *cancel, struct s7d_codec_tee_result *result)
{
	const uuid_t uuid = UUID_INIT(0x526fc4fc, 0x7ee6, 0x4a12, 0x96, 0xe3,
				    0x83, 0xda, 0x95, 0x65, 0xbc, 0xe8);
	struct tee_ioctl_open_session_arg open = { };
	struct tee_context *context;
	struct tee_param params[4] = { };
	struct tee_shm *shm;
	size_t offset = 0;
	void *buffer;
	u32 session;
	int ret;

	if (!result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (!dev || !view || !view->data || !view->size ||
	    view->size > U32_MAX || !cancel)
		return -EINVAL;
	if (s7d_codec_tee_cancelled(cancel, result))
		return -ECANCELED;

	context = tee_client_open_context(NULL, s7d_codec_tee_match, NULL, NULL);
	if (IS_ERR(context)) {
		result->linux_status = PTR_ERR(context);
		return result->linux_status;
	}
	context->supp_nowait = true;
	if (s7d_codec_tee_cancelled(cancel, result)) {
		ret = -ECANCELED;
		goto out_context;
	}
	shm = tee_shm_alloc_kernel_buf(context, SZ_128K);
	if (IS_ERR(shm)) {
		ret = PTR_ERR(shm);
		result->linux_status = ret;
		goto out_context;
	}
	if (s7d_codec_tee_cancelled(cancel, result)) {
		ret = -ECANCELED;
		goto out_shm;
	}
	buffer = tee_shm_get_va(shm, 0);
	if (IS_ERR(buffer)) {
		ret = PTR_ERR(buffer);
		result->linux_status = ret;
		goto out_shm;
	}
	if (s7d_codec_tee_cancelled(cancel, result)) {
		ret = -ECANCELED;
		goto out_shm;
	}

	export_uuid(open.uuid, &uuid);
	open.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	open.cancel_id = cancel_id;
	open.num_params = ARRAY_SIZE(params);
	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = view->size;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[1].u.value.a = 1;
	ret = tee_client_open_session(context, &open, params);
	ret = s7d_codec_tee_status(dev, result, S7D_CODEC_TEE_OPEN, ret,
				   open.ret, open.ret_origin);
	if (ret)
		goto out_shm;
	if (open.ret) {
		dev_err(dev, "TEE preload open failed: status=%#x origin=%#x\n",
			open.ret, open.ret_origin);
		ret = -EIO;
		goto out_shm;
	}
	session = open.session;

	while (offset < view->size) {
		struct tee_ioctl_invoke_arg invoke = { };
		size_t length = min_t(size_t, SZ_128K, view->size - offset);

		if (s7d_codec_tee_cancelled(cancel, result)) {
			ret = -ECANCELED;
			goto out_session;
		}
		memset(buffer, 0, SZ_128K);
		memcpy(buffer, view->data + offset, length);
		memset(params, 0, sizeof(params));
		params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
		params[0].u.memref.shm = shm;
		params[0].u.memref.size = length;
		invoke.session = session;
		invoke.cancel_id = cancel_id;
		invoke.num_params = ARRAY_SIZE(params);
		result->submitted_bytes = offset + length;
		result->upload_started = true;
		ret = tee_client_invoke_func(context, &invoke, params);
		ret = s7d_codec_tee_status(dev, result, S7D_CODEC_TEE_INVOKE,
					   ret, invoke.ret, invoke.ret_origin);
		if (ret)
			goto out_session;
		if (invoke.ret && (invoke.ret != S7D_CODEC_TEE_BUSY ||
				   offset + length == view->size)) {
			dev_err(dev,
				"TEE preload invoke failed: status=%#x origin=%#x bytes=%zu\n",
				invoke.ret, invoke.ret_origin, offset + length);
			ret = -EIO;
			goto out_session;
		}
		offset += length;
		result->accepted_bytes = offset;
	}
	result->upload_complete = true;

out_session:
	if (s7d_codec_tee_cancelled(cancel, result) && !ret)
		ret = -ECANCELED;
	result->close_attempted = true;
	result->close_status = tee_client_close_session(context, session);
	if (result->close_status) {
		dev_err(dev, "TEE preload close failed: %d\n", result->close_status);
		if (!ret)
			ret = result->close_status < 0 ? result->close_status : -EIO;
	}
out_shm:
	tee_shm_free(shm);
out_context:
	tee_client_close_context(context);
	if (s7d_codec_tee_cancelled(cancel, result) && !ret)
		ret = -ECANCELED;
	result->complete = !ret && result->upload_complete;
	result->partial_upload = result->upload_started && !result->complete;
	return ret;
}
