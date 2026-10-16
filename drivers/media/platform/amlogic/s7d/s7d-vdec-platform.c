// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>

#include "common/s7d-codec-dos.h"
#include "common/s7d-codec-firmware.h"
#include "h264/s7d-h264-backend.h"
#include "hevc/s7d-hevc-v4l2.h"
#include "common/s7d-hevc-engine.h"
#include "jpeg/s7d-jpeg-backend.h"
#include "jpeg/s7d-jpeg-v4l2.h"
#include "vp9/s7d-vp9-v4l2.h"
#include "common/s7d-v4l2-device.h"

struct s7d_vdec {
	struct list_head failed_node;
	struct device *dev;
	struct s7d_codec_dos *dos;
	struct s7d_codec_firmware_owner *firmware;
	struct s7d_h264_decoder *decoder;
	struct s7d_v4l2_device *video;
	struct s7d_hevc_v4l2 *hevc;
	struct s7d_hevc_engine *hevc_engine;
	struct s7d_v4l2_device *hevc_video;
	struct s7d_vp9_v4l2 *vp9;
	struct s7d_v4l2_device *vp9_video;
	struct s7d_jpeg_encoder *jpeg;
	struct s7d_jpeg_v4l2 *jpeg_video;
	struct module *tee_owner;
	bool reserved_memory;
};

static LIST_HEAD(s7d_vdec_failed);
static DEFINE_MUTEX(s7d_vdec_failed_lock);

static bool s7d_vdec_probe_blocked(struct device *dev)
{
	struct s7d_vdec *vdec;
	bool blocked = false;

	mutex_lock(&s7d_vdec_failed_lock);
	list_for_each_entry(vdec, &s7d_vdec_failed, failed_node) {
		if (vdec->dev == dev) {
			blocked = true;
			break;
		}
	}
	mutex_unlock(&s7d_vdec_failed_lock);
	return blocked;
}

static bool s7d_vdec_retain(struct s7d_vdec *vdec)
{
	if (!try_module_get(THIS_MODULE))
		return false;
	mutex_lock(&s7d_vdec_failed_lock);
	list_add_tail(&vdec->failed_node, &s7d_vdec_failed);
	mutex_unlock(&s7d_vdec_failed_lock);
	return true;
}

static int s7d_vdec_get_tee(struct s7d_vdec *vdec)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct device *supplier;
	struct module *owner;
	const char *method;
	int ret = -EINVAL;

	node = of_parse_phandle(vdec->dev->of_node, "amlogic,tee", 0);
	if (!node)
		return -EINVAL;
	if (!of_device_is_available(node) ||
	    !of_device_is_compatible(node, "linaro,optee-tz") ||
	    of_property_read_string(node, "method", &method) ||
	    strcmp(method, "smc"))
		goto put_node;
	pdev = of_find_device_by_node(node);
	if (!pdev) {
		ret = -EPROBE_DEFER;
		goto put_node;
	}
	supplier = &pdev->dev;
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
	if (!device_link_add(vdec->dev, supplier, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		module_put(owner);
		goto unlock;
	}
	vdec->tee_owner = owner;
	ret = 0;
unlock:
	device_unlock(supplier);
	put_device(supplier);
put_node:
	of_node_put(node);
	return ret;
}

static int s7d_vdec_attach_memory(struct device *dev)
{
	struct device_node *pool;
	struct reserved_mem *rmem;
	int index, ret = -EINVAL;

	index = of_property_match_string(dev->of_node, "memory-region-names",
					 "codec");
	if (index < 0)
		return index;
	pool = of_parse_phandle(dev->of_node, "memory-region", index);
	if (!pool)
		return -ENODEV;
	if (!of_device_is_available(pool) ||
	    !of_device_is_compatible(pool, "shared-dma-pool") ||
	    !of_property_read_bool(pool, "reusable") ||
	    of_property_read_bool(pool, "no-map"))
		goto put_pool;
	rmem = of_reserved_mem_lookup(pool);
	if (!rmem || !rmem->size || rmem->base > U32_MAX ||
	    rmem->size - 1 > U32_MAX - rmem->base)
		goto put_pool;
	ret = of_reserved_mem_device_init_by_idx(dev, dev->of_node, index);
put_pool:
	of_node_put(pool);
	return ret;
}

static int s7d_vdec_release(struct s7d_vdec *vdec)
{
	int ret;

	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) && vdec->jpeg_video) {
		ret = s7d_jpeg_v4l2_destroy(vdec->jpeg_video);
		if (ret)
			return ret;
		vdec->jpeg_video = NULL;
	}
	if (vdec->vp9_video) {
		ret = s7d_v4l2_device_destroy(vdec->vp9_video);
		if (ret)
			return ret;
		vdec->vp9_video = NULL;
	}
	if (vdec->hevc_video) {
		ret = s7d_v4l2_device_destroy(vdec->hevc_video);
		if (ret)
			return ret;
		vdec->hevc_video = NULL;
	}
	if (vdec->video) {
		ret = s7d_v4l2_device_destroy(vdec->video);
		if (ret)
			return ret;
		vdec->video = NULL;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) && vdec->jpeg) {
		ret = s7d_jpeg_encoder_destroy(vdec->jpeg);
		if (ret)
			return ret;
		vdec->jpeg = NULL;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_EXPERIMENTAL) && vdec->hevc) {
		ret = s7d_hevc_v4l2_destroy(vdec->hevc);
		if (ret)
			return ret;
		vdec->hevc = NULL;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_VP9_EXPERIMENTAL) && vdec->vp9) {
		ret = s7d_vp9_v4l2_destroy(vdec->vp9);
		if (ret)
			return ret;
		vdec->vp9 = NULL;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_ENGINE) && vdec->hevc_engine) {
		ret = s7d_hevc_engine_destroy(vdec->hevc_engine);
		if (ret)
			return ret;
		vdec->hevc_engine = NULL;
	}
	if (vdec->decoder) {
		ret = s7d_h264_decoder_destroy(vdec->decoder);
		if (ret)
			return ret;
		vdec->decoder = NULL;
	}
	if (vdec->firmware) {
		ret = s7d_codec_firmware_destroy(vdec->firmware);
		if (ret)
			return ret;
		vdec->firmware = NULL;
	}
	if (vdec->dos) {
		ret = s7d_codec_dos_destroy(vdec->dos);
		if (ret)
			return ret;
		vdec->dos = NULL;
	}
	if (vdec->reserved_memory)
		of_reserved_mem_device_release(vdec->dev);
	module_put(vdec->tee_owner);
	put_device(vdec->dev);
	kfree(vdec);
	return 0;
}

static int s7d_vdec_stop(struct s7d_vdec *vdec)
{
	int ret, error = 0;

	if (vdec->video)
		s7d_v4l2_device_unregister(vdec->video);
	if (vdec->hevc_video)
		s7d_v4l2_device_unregister(vdec->hevc_video);
	if (vdec->vp9_video)
		s7d_v4l2_device_unregister(vdec->vp9_video);
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) && vdec->jpeg_video)
		s7d_jpeg_v4l2_unregister(vdec->jpeg_video);
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_EXPERIMENTAL) && vdec->hevc) {
		ret = s7d_hevc_v4l2_shutdown(vdec->hevc);
		if (ret)
			error = ret;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_VP9_EXPERIMENTAL) && vdec->vp9) {
		ret = s7d_vp9_v4l2_shutdown(vdec->vp9);
		if (ret && !error)
			error = ret;
	}
	if (vdec->video) {
		ret = s7d_v4l2_device_shutdown(vdec->video);
		if (ret && !error)
			error = ret;
	}
	if (vdec->hevc_video) {
		ret = s7d_v4l2_device_shutdown(vdec->hevc_video);
		if (ret && !error)
			error = ret;
	}
	if (vdec->vp9_video) {
		ret = s7d_v4l2_device_shutdown(vdec->vp9_video);
		if (ret && !error)
			error = ret;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) && vdec->jpeg_video) {
		ret = s7d_jpeg_v4l2_shutdown(vdec->jpeg_video);
		if (ret && !error)
			error = ret;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) && vdec->jpeg) {
		ret = s7d_jpeg_encoder_shutdown(vdec->jpeg);
		if (ret && !error)
			error = ret;
	}
	if (vdec->decoder) {
		ret = s7d_h264_decoder_shutdown(vdec->decoder);
		if (ret && !error)
			error = ret;
	}
	return error;
}

static int s7d_vdec_probe(struct platform_device *pdev)
{
	struct s7d_vdec *vdec;
	int ret, cleanup;

	if (s7d_vdec_probe_blocked(&pdev->dev))
		return -EBUSY;
	if (pdev->dev.pm_domain)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "DOS requires named power domains\n");
	vdec = kzalloc(sizeof(*vdec), GFP_KERNEL);
	if (!vdec)
		return -ENOMEM;
	vdec->dev = get_device(&pdev->dev);
	ret = s7d_vdec_get_tee(vdec);
	if (ret)
		goto fail;
	ret = dma_set_mask_and_coherent(vdec->dev, DMA_BIT_MASK(32));
	if (ret)
		goto fail;
	ret = s7d_vdec_attach_memory(vdec->dev);
	if (ret)
		goto fail;
	vdec->reserved_memory = true;
	vdec->dos = s7d_codec_dos_create(pdev, THIS_MODULE);
	if (IS_ERR(vdec->dos)) {
		ret = PTR_ERR(vdec->dos);
		vdec->dos = NULL;
		goto fail;
	}
	vdec->firmware = s7d_codec_firmware_create(vdec->dev, THIS_MODULE);
	if (IS_ERR(vdec->firmware)) {
		ret = PTR_ERR(vdec->firmware);
		vdec->firmware = NULL;
		goto fail;
	}
	vdec->decoder = s7d_h264_decoder_create(vdec->dev, vdec->dos,
						vdec->firmware, THIS_MODULE);
	if (IS_ERR(vdec->decoder)) {
		ret = PTR_ERR(vdec->decoder);
		vdec->decoder = NULL;
		goto fail;
	}
	vdec->video = s7d_v4l2_device_create(vdec->dev,
					   s7d_h264_decoder_core(vdec->decoder),
					   s7d_h264_decoder_factory(vdec->decoder),
					   vdec->decoder);
	if (IS_ERR(vdec->video)) {
		ret = PTR_ERR(vdec->video);
		vdec->video = NULL;
		goto fail;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_ENGINE)) {
		vdec->hevc_engine = s7d_hevc_engine_create(vdec->dev, vdec->dos, THIS_MODULE);
		if (IS_ERR(vdec->hevc_engine)) {
			ret = PTR_ERR(vdec->hevc_engine);
			vdec->hevc_engine = NULL;
			goto fail;
		}
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_EXPERIMENTAL)) {
		vdec->hevc = s7d_hevc_v4l2_create(vdec->dev, vdec->dos, vdec->firmware,
						vdec->hevc_engine, THIS_MODULE);
		if (IS_ERR(vdec->hevc)) {
			ret = PTR_ERR(vdec->hevc);
			vdec->hevc = NULL;
			goto fail;
		}
		vdec->hevc_video = s7d_v4l2_device_create(vdec->dev,
							s7d_hevc_v4l2_core(vdec->hevc),
							s7d_hevc_v4l2_factory(vdec->hevc),
							vdec->hevc);
		if (IS_ERR(vdec->hevc_video)) {
			ret = PTR_ERR(vdec->hevc_video);
			vdec->hevc_video = NULL;
			goto fail;
		}
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_VP9_EXPERIMENTAL)) {
		vdec->vp9 = s7d_vp9_v4l2_create(vdec->dev, vdec->dos, vdec->firmware,
					       vdec->hevc_engine, THIS_MODULE);
		if (IS_ERR(vdec->vp9)) {
			ret = PTR_ERR(vdec->vp9);
			vdec->vp9 = NULL;
			goto fail;
		}
		vdec->vp9_video = s7d_v4l2_device_create(vdec->dev,
						       s7d_vp9_v4l2_core(vdec->vp9),
						       s7d_vp9_v4l2_factory(vdec->vp9),
						       vdec->vp9);
		if (IS_ERR(vdec->vp9_video)) {
			ret = PTR_ERR(vdec->vp9_video);
			vdec->vp9_video = NULL;
			goto fail;
		}
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL)) {
		vdec->jpeg = s7d_jpeg_encoder_create(vdec->dev, vdec->dos, vdec->firmware,
						    THIS_MODULE);
		if (IS_ERR(vdec->jpeg)) {
			ret = PTR_ERR(vdec->jpeg);
			vdec->jpeg = NULL;
			goto fail;
		}
		vdec->jpeg_video = s7d_jpeg_v4l2_create(vdec->dev, vdec->jpeg);
		if (IS_ERR(vdec->jpeg_video)) {
			ret = PTR_ERR(vdec->jpeg_video);
			vdec->jpeg_video = NULL;
			goto fail;
		}
	}
	ret = s7d_v4l2_device_register(vdec->video);
	if (ret)
		goto fail;
	if (vdec->hevc_video) {
		ret = s7d_v4l2_device_register(vdec->hevc_video);
		if (ret)
			goto fail;
	}
	if (vdec->vp9_video) {
		ret = s7d_v4l2_device_register(vdec->vp9_video);
		if (ret)
			goto fail;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) && vdec->jpeg_video) {
		ret = s7d_jpeg_v4l2_register(vdec->jpeg_video);
		if (ret)
			goto fail;
	}
	platform_set_drvdata(pdev, vdec);
	return 0;

fail:
	cleanup = s7d_vdec_stop(vdec);
	if (!cleanup)
		cleanup = s7d_vdec_release(vdec);
	if (cleanup) {
		if (s7d_vdec_retain(vdec))
			dev_err(&pdev->dev, "probe unwind failed: %d; resources retained\n",
				cleanup);
		else
			dev_crit(&pdev->dev, "probe unwind failed after owner teardown: %d\n",
				 cleanup);
	}
	return dev_err_probe(&pdev->dev, ret, "decoder initialization failed\n");
}

static void s7d_vdec_remove(struct platform_device *pdev)
{
	struct s7d_vdec *vdec = platform_get_drvdata(pdev);
	int ret;

	ret = s7d_vdec_stop(vdec);
	if (!ret)
		ret = s7d_vdec_release(vdec);
	if (ret) {
		if (s7d_vdec_retain(vdec))
			dev_err(&pdev->dev, "decoder retirement failed: %d; resources retained\n",
				ret);
		else
			dev_crit(&pdev->dev, "decoder retirement failed after owner teardown: %d\n",
				 ret);
		return;
	}
	platform_set_drvdata(pdev, NULL);
}

static void s7d_vdec_shutdown(struct platform_device *pdev)
{
	struct s7d_vdec *vdec = platform_get_drvdata(pdev);
	int ret;

	ret = s7d_vdec_stop(vdec);
	if (ret)
		dev_err(vdec->dev, "decoder shutdown failed: %d\n", ret);
}

static int s7d_vdec_prepare(struct device *dev)
{
	struct s7d_vdec *vdec = dev_get_drvdata(dev);
	int ret;

	ret = s7d_h264_decoder_suspend_check(vdec->decoder);
	if (ret)
		return ret;
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_EXPERIMENTAL) && vdec->hevc) {
		ret = s7d_hevc_v4l2_suspend_check(vdec->hevc);
		if (ret)
			return ret;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_VP9_EXPERIMENTAL) && vdec->vp9) {
		ret = s7d_vp9_v4l2_suspend_check(vdec->vp9);
		if (ret)
			return ret;
	}
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_ENGINE) && vdec->hevc_engine) {
		ret = s7d_hevc_engine_suspend_check(vdec->hevc_engine);
		if (ret)
			return ret;
	}
	return s7d_codec_dos_suspend_check(vdec->dos);
}

static const struct dev_pm_ops s7d_vdec_pm_ops = {
	.prepare = s7d_vdec_prepare,
};

static const struct of_device_id s7d_vdec_of_match[] = {
	{ .compatible = "amlogic,s7d-vdec" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_vdec_of_match);

static struct platform_driver s7d_vdec_driver = {
	.probe = s7d_vdec_probe,
	.remove = s7d_vdec_remove,
	.shutdown = s7d_vdec_shutdown,
	.driver = {
		.name = "s7d-vdec",
		.of_match_table = s7d_vdec_of_match,
		.pm = pm_ptr(&s7d_vdec_pm_ops),
		.suppress_bind_attrs = true,
	},
};

static int __init s7d_vdec_init(void)
{
	return platform_driver_register(&s7d_vdec_driver);
}
module_init(s7d_vdec_init);

static void __exit s7d_vdec_exit(void)
{
	unsigned int sleep_flags = lock_system_sleep();

	platform_driver_unregister(&s7d_vdec_driver);
	unlock_system_sleep(sleep_flags);
}
module_exit(s7d_vdec_exit);

MODULE_DESCRIPTION("Amlogic S7D video codecs");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("video/video_ucode.bin");
MODULE_IMPORT_NS(DMA_BUF);
