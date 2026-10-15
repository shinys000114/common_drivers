// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/amlogic/s7d-video-firmware.h>
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
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

#include "s7d-codec-bundle.h"
#include "s7d-codec-tee.h"

enum s7d_video_fw_state {
	S7D_VIDEO_FW_EMPTY,
	S7D_VIDEO_FW_LOADING,
	S7D_VIDEO_FW_PRE_FAILED,
	S7D_VIDEO_FW_READY,
	S7D_VIDEO_FW_UNKNOWN,
};

struct s7d_video_fw_provider {
	struct device *dev;
	struct device_node *node;
	struct device_node *tee_node;
	struct device *tee_dev;
	struct module *tee_owner;
	struct mutex lock;
	struct workqueue_struct *queue;
	const struct firmware *archive;
	struct s7d_video_fw_ticket *active;
	enum s7d_video_fw_state state;
	u64 lease_sequence;
	u64 failed_epoch;
	u32 cancel_sequence;
	int error;
	bool available;
	bool blocked;
};

struct s7d_video_fw_handle {
	struct kref ref;
	struct s7d_video_fw_provider *provider;
	struct device *consumer;
	struct device *supplier;
};

enum s7d_video_fw_ticket_state {
	S7D_VIDEO_FW_PREPARED,
	S7D_VIDEO_FW_QUEUED,
	S7D_VIDEO_FW_ABANDONED,
};

struct s7d_video_fw_ticket {
	struct kref ref;
	struct work_struct work;
	struct completion done;
	struct s7d_video_fw_handle *handle;
	const struct firmware *firmware;
	struct s7d_codec_tee_result result;
	atomic_t queued;
	atomic_t cancel;
	u64 lease_epoch;
	u32 cancel_id;
	int status;
	bool cancelled;
};

static DEFINE_MUTEX(s7d_video_fw_registry_lock);
/* Preserve archive identity and UNKNOWN state after forced device removal. */
static struct s7d_video_fw_provider *s7d_video_fw_provider;
static struct platform_driver s7d_video_fw_driver;

static void video_fw_handle_free(struct kref *ref)
{
	struct s7d_video_fw_handle *handle =
		container_of(ref, struct s7d_video_fw_handle, ref);

	put_device(handle->supplier);
	put_device(handle->consumer);
	kfree(handle);
}

void s7d_video_fw_put(struct s7d_video_fw_handle *handle)
{
	kref_put(&handle->ref, video_fw_handle_free);
}
EXPORT_SYMBOL_GPL(s7d_video_fw_put);

static void video_fw_ticket_free(struct kref *ref)
{
	struct s7d_video_fw_ticket *ticket =
		container_of(ref, struct s7d_video_fw_ticket, ref);

	release_firmware(ticket->firmware);
	s7d_video_fw_put(ticket->handle);
	kfree(ticket);
}

void s7d_video_fw_ticket_put(struct s7d_video_fw_ticket *ticket)
{
	struct s7d_video_fw_provider *provider = ticket->handle->provider;
	bool abandoned = false;

	mutex_lock(&provider->lock);
	if (provider->active == ticket &&
	    atomic_cmpxchg(&ticket->queued, S7D_VIDEO_FW_PREPARED,
			   S7D_VIDEO_FW_ABANDONED) == S7D_VIDEO_FW_PREPARED) {
		provider->active = NULL;
		if (provider->state != S7D_VIDEO_FW_UNKNOWN) {
			provider->state = S7D_VIDEO_FW_PRE_FAILED;
			provider->failed_epoch = ticket->lease_epoch;
			provider->error = -ECANCELED;
		}
		abandoned = true;
	}
	mutex_unlock(&provider->lock);
	if (abandoned)
		kref_put(&ticket->ref, video_fw_ticket_free);
	kref_put(&ticket->ref, video_fw_ticket_free);
}
EXPORT_SYMBOL_GPL(s7d_video_fw_ticket_put);

static void video_fw_work(struct work_struct *work)
{
	struct s7d_video_fw_ticket *ticket =
		container_of(work, struct s7d_video_fw_ticket, work);
	struct s7d_video_fw_provider *provider = ticket->handle->provider;
	struct s7d_codec_bundle_view view;
	int ret;

	if (atomic_read(&ticket->cancel)) {
		ticket->cancelled = true;
		ret = -ECANCELED;
	} else {
		ret = request_firmware_direct(&ticket->firmware,
					      "video/video_ucode.bin", provider->dev);
	}
	if (!ret) {
		if (atomic_read(&ticket->cancel)) {
			ticket->cancelled = true;
			ret = -ECANCELED;
		} else {
			ret = s7d_codec_bundle_parse(ticket->firmware->data,
						   ticket->firmware->size, &view);
		}
	}
	if (!ret)
		ret = s7d_codec_tee_upload(provider->dev, &view, ticket->cancel_id,
					 &ticket->cancel, &ticket->result);

	mutex_lock(&provider->lock);
	if (!provider->available || provider->state == S7D_VIDEO_FW_UNKNOWN) {
		ret = provider->error;
	} else if (!ret && ticket->result.complete) {
		provider->archive = ticket->firmware;
		ticket->firmware = NULL;
		provider->state = S7D_VIDEO_FW_READY;
		provider->error = 0;
	} else {
		ret = ret ? ret : -EIO;
		provider->error = ret;
		provider->failed_epoch = ticket->lease_epoch;
		provider->state = ticket->result.phase == S7D_CODEC_TEE_NONE ?
			S7D_VIDEO_FW_PRE_FAILED : S7D_VIDEO_FW_UNKNOWN;
	}
	provider->active = NULL;
	ticket->status = ret;
	mutex_unlock(&provider->lock);
	complete_all(&ticket->done);
	kref_put(&ticket->ref, video_fw_ticket_free);
}

bool s7d_video_fw_ticket_queue(struct s7d_video_fw_ticket *ticket)
{
	if (atomic_cmpxchg(&ticket->queued, S7D_VIDEO_FW_PREPARED,
			   S7D_VIDEO_FW_QUEUED) != S7D_VIDEO_FW_PREPARED)
		return false;
	if (queue_work(ticket->handle->provider->queue, &ticket->work))
		return true;
	atomic_set(&ticket->queued, S7D_VIDEO_FW_PREPARED);
	return false;
}
EXPORT_SYMBOL_GPL(s7d_video_fw_ticket_queue);

void s7d_video_fw_ticket_cancel(struct s7d_video_fw_ticket *ticket)
{
	atomic_set(&ticket->cancel, 1);
}
EXPORT_SYMBOL_GPL(s7d_video_fw_ticket_cancel);

int s7d_video_fw_ticket_wait(struct s7d_video_fw_ticket *ticket, bool *cancelled)
{
	int ret;

	if (cancelled)
		*cancelled = false;
	wait_for_completion(&ticket->done);
	flush_work(&ticket->work);
	ret = ticket->status;
	if (!cancelled || ret != -ECANCELED)
		return ret;
	if (ticket->result.linux_status)
		return ticket->result.linux_status < 0 ? ticket->result.linux_status : -EIO;
	if (ticket->result.close_status)
		return ticket->result.close_status < 0 ? ticket->result.close_status : -EIO;
	*cancelled = ticket->cancelled || ticket->result.cancelled;
	return ret;
}
EXPORT_SYMBOL_GPL(s7d_video_fw_ticket_wait);

struct s7d_video_fw_handle *s7d_video_fw_get(struct device *consumer)
{
	struct s7d_video_fw_handle *handle;
	struct s7d_video_fw_provider *provider;
	struct device_node *node, *tee, *service_tee;
	struct platform_device *pdev;
	struct device *supplier;
	int ret = -EINVAL;

	if (!consumer || !consumer->of_node)
		return ERR_PTR(-EINVAL);
	node = of_parse_phandle(consumer->of_node, "amlogic,video-firmware", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	if (!of_device_is_available(node) ||
	    !of_device_is_compatible(node, "amlogic,s7d-video-firmware"))
		goto put_node;
	tee = of_parse_phandle(consumer->of_node, "amlogic,tee", 0);
	if (!tee)
		goto put_node;
	service_tee = of_parse_phandle(node, "amlogic,tee", 0);
	if (!service_tee || service_tee != tee) {
		of_node_put(service_tee);
		of_node_put(tee);
		goto put_node;
	}
	of_node_put(service_tee);
	mutex_lock(&s7d_video_fw_registry_lock);
	provider = s7d_video_fw_provider;
	if (provider && provider->node == node) {
		mutex_lock(&provider->lock);
		ret = provider->tee_node != tee ? -EINVAL :
			!provider->available ? -ESTALE : 0;
		mutex_unlock(&provider->lock);
	} else {
		ret = -EPROBE_DEFER;
	}
	mutex_unlock(&s7d_video_fw_registry_lock);
	of_node_put(tee);
	if (ret)
		goto put_node;
	pdev = of_find_device_by_node(node);
	if (!pdev) {
		ret = -EPROBE_DEFER;
		goto put_node;
	}
	supplier = &pdev->dev;
	device_lock(supplier);
	if (supplier->driver != &s7d_video_fw_driver.driver ||
	    !device_is_bound(supplier) ||
	    READ_ONCE(supplier->links.status) != DL_DEV_DRIVER_BOUND) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	mutex_lock(&provider->lock);
	if (!provider->available || provider->blocked) {
		ret = provider->available ? -EBUSY : -ESTALE;
		goto unlock_provider;
	}
	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle) {
		ret = -ENOMEM;
		goto unlock_provider;
	}
	if (!device_link_add(consumer, supplier, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		kfree(handle);
		ret = -EINVAL;
		goto unlock_provider;
	}
	kref_init(&handle->ref);
	handle->provider = provider;
	handle->consumer = get_device(consumer);
	handle->supplier = get_device(supplier);
	mutex_unlock(&provider->lock);
	device_unlock(supplier);
	put_device(supplier);
	of_node_put(node);
	return handle;

unlock_provider:
	mutex_unlock(&provider->lock);
unlock:
	device_unlock(supplier);
	put_device(supplier);
put_node:
	of_node_put(node);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(s7d_video_fw_get);

int s7d_video_fw_next_epoch(struct s7d_video_fw_handle *handle, u64 *out)
{
	struct s7d_video_fw_provider *provider = handle->provider;
	int ret = 0;

	if (!out)
		return -EINVAL;
	*out = 0;
	mutex_lock(&provider->lock);
	if (!provider->available)
		ret = -ESTALE;
	else if (provider->blocked)
		ret = -EBUSY;
	else if (provider->lease_sequence == U64_MAX)
		ret = -EOVERFLOW;
	else
		*out = ++provider->lease_sequence;
	mutex_unlock(&provider->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(s7d_video_fw_next_epoch);

int s7d_video_fw_prepare(struct s7d_video_fw_handle *handle, u64 lease_epoch,
			 struct s7d_video_fw_ticket **out)
{
	struct s7d_video_fw_provider *provider = handle->provider;
	struct s7d_video_fw_ticket *ticket;
	int ret = 0;

	if (!lease_epoch || !out)
		return -EINVAL;
	*out = NULL;
	mutex_lock(&provider->lock);
	if (!provider->available || provider->state == S7D_VIDEO_FW_UNKNOWN) {
		ret = provider->error;
		goto unlock;
	}
	if (provider->blocked || provider->active) {
		ret = -EBUSY;
		goto unlock;
	}
	if (provider->state == S7D_VIDEO_FW_READY)
		goto unlock;
	if (provider->state == S7D_VIDEO_FW_PRE_FAILED &&
	    provider->failed_epoch == lease_epoch) {
		ret = provider->error;
		goto unlock;
	}
	if (provider->cancel_sequence == U32_MAX) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	ticket = kzalloc(sizeof(*ticket), GFP_KERNEL);
	if (!ticket) {
		ret = -ENOMEM;
		goto unlock;
	}
	kref_init(&ticket->ref);
	kref_get(&ticket->ref);
	kref_get(&handle->ref);
	INIT_WORK(&ticket->work, video_fw_work);
	init_completion(&ticket->done);
	atomic_set(&ticket->queued, S7D_VIDEO_FW_PREPARED);
	atomic_set(&ticket->cancel, 0);
	ticket->handle = handle;
	ticket->lease_epoch = lease_epoch;
	ticket->cancel_id = ++provider->cancel_sequence;
	ticket->status = -EINPROGRESS;
	provider->active = ticket;
	provider->state = S7D_VIDEO_FW_LOADING;
	*out = ticket;
unlock:
	mutex_unlock(&provider->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(s7d_video_fw_prepare);

static int video_fw_get_tee(struct platform_device *pdev,
			    struct s7d_video_fw_provider *provider)
{
	struct device_node *node;
	struct platform_device *tee;
	struct device *supplier;
	struct module *owner;
	const char *method;
	int ret = -EINVAL;

	node = of_parse_phandle(pdev->dev.of_node, "amlogic,tee", 0);
	if (!node)
		return -EINVAL;
	if (!of_device_is_available(node) ||
	    !of_device_is_compatible(node, "linaro,optee-tz") ||
	    of_property_read_string(node, "method", &method) ||
	    strcmp(method, "smc"))
		goto put_node;
	tee = of_find_device_by_node(node);
	if (!tee) {
		ret = -EPROBE_DEFER;
		goto put_node;
	}
	supplier = &tee->dev;
	device_lock(supplier);
	if (!supplier->driver || !device_is_bound(supplier) ||
	    READ_ONCE(supplier->links.status) != DL_DEV_DRIVER_BOUND) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	owner = supplier->driver->owner;
	if (!try_module_get(owner)) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	if (!device_link_add(&pdev->dev, supplier, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		module_put(owner);
		goto unlock;
	}
	provider->tee_node = node;
	provider->tee_dev = supplier;
	provider->tee_owner = owner;
	device_unlock(supplier);
	return 0;
unlock:
	device_unlock(supplier);
	put_device(supplier);
put_node:
	of_node_put(node);
	return ret;
}

static int video_fw_probe(struct platform_device *pdev)
{
	struct s7d_video_fw_provider *provider;
	int ret;

	mutex_lock(&s7d_video_fw_registry_lock);
	if (s7d_video_fw_provider) {
		ret = s7d_video_fw_provider->node == pdev->dev.of_node ? -ESTALE : -EBUSY;
		goto unlock;
	}
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider) {
		ret = -ENOMEM;
		goto unlock;
	}
	provider->queue = alloc_ordered_workqueue("s7d-video-firmware", WQ_MEM_RECLAIM);
	if (!provider->queue) {
		ret = -ENOMEM;
		goto free_provider;
	}
	ret = video_fw_get_tee(pdev, provider);
	if (ret)
		goto destroy_queue;
	provider->dev = get_device(&pdev->dev);
	provider->node = of_node_get(pdev->dev.of_node);
	mutex_init(&provider->lock);
	provider->available = true;
	platform_set_drvdata(pdev, provider);
	s7d_video_fw_provider = provider;
	mutex_unlock(&s7d_video_fw_registry_lock);
	return 0;

destroy_queue:
	destroy_workqueue(provider->queue);
free_provider:
	kfree(provider);
unlock:
	mutex_unlock(&s7d_video_fw_registry_lock);
	return ret;
}

static void video_fw_stop(struct s7d_video_fw_provider *provider)
{
	struct s7d_video_fw_ticket *ticket;

	mutex_lock(&provider->lock);
	provider->available = false;
	provider->state = S7D_VIDEO_FW_UNKNOWN;
	provider->error = -ESTALE;
	ticket = provider->active;
	if (ticket) {
		kref_get(&ticket->ref);
		s7d_video_fw_ticket_cancel(ticket);
		s7d_video_fw_ticket_queue(ticket);
	}
	mutex_unlock(&provider->lock);
	if (ticket) {
		s7d_video_fw_ticket_wait(ticket, NULL);
		kref_put(&ticket->ref, video_fw_ticket_free);
	}
	flush_workqueue(provider->queue);
}

static void video_fw_remove(struct platform_device *pdev)
{
	struct s7d_video_fw_provider *provider = platform_get_drvdata(pdev);

	video_fw_stop(provider);
	platform_set_drvdata(pdev, NULL);
	module_put(provider->tee_owner);
	provider->tee_owner = NULL;
}

static void video_fw_shutdown(struct platform_device *pdev)
{
	video_fw_stop(platform_get_drvdata(pdev));
}

static int video_fw_pm_prepare(struct device *dev)
{
	struct s7d_video_fw_provider *provider = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&provider->lock);
	if (provider->active || (provider->state == S7D_VIDEO_FW_READY &&
				 pm_suspend_target_state != PM_SUSPEND_TO_IDLE))
		ret = -EBUSY;
	else
		provider->blocked = true;
	mutex_unlock(&provider->lock);
	if (!ret)
		flush_workqueue(provider->queue);
	return ret;
}

static void video_fw_pm_complete(struct device *dev)
{
	struct s7d_video_fw_provider *provider = dev_get_drvdata(dev);

	mutex_lock(&provider->lock);
	provider->blocked = false;
	mutex_unlock(&provider->lock);
}

static int video_fw_pm_restore(struct device *dev)
{
	struct s7d_video_fw_provider *provider = dev_get_drvdata(dev);

	mutex_lock(&provider->lock);
	provider->state = S7D_VIDEO_FW_UNKNOWN;
	provider->error = -ESTALE;
	mutex_unlock(&provider->lock);
	return 0;
}

static const struct dev_pm_ops video_fw_pm_ops = {
	.prepare = video_fw_pm_prepare,
	.complete = video_fw_pm_complete,
	.restore = video_fw_pm_restore,
};

static const struct of_device_id video_fw_of_match[] = {
	{ .compatible = "amlogic,s7d-video-firmware" },
	{ }
};
MODULE_DEVICE_TABLE(of, video_fw_of_match);

static struct platform_driver s7d_video_fw_driver = {
	.probe = video_fw_probe,
	.remove = video_fw_remove,
	.shutdown = video_fw_shutdown,
	.driver = {
		.name = "s7d-video-firmware",
		.of_match_table = video_fw_of_match,
		.pm = &video_fw_pm_ops,
		.suppress_bind_attrs = true,
	},
};

static int __init video_fw_init(void)
{
	return platform_driver_register(&s7d_video_fw_driver);
}
/* The TA has no verified archive-unload API; keep its owner resident this boot. */
module_init(video_fw_init);

MODULE_DESCRIPTION("Amlogic S7D resident video firmware archive provider");
MODULE_FIRMWARE("video/video_ucode.bin");
MODULE_LICENSE("GPL");
