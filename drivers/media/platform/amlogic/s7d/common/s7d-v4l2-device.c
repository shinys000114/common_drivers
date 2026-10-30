// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include "s7d-v4l2-device.h"

struct s7d_v4l2_codec_entry {
	struct s7d_codec_core *core;
	struct s7d_v4l2_backend_factory factory;
	struct s7d_v4l2_backend_ops ops;
	void *priv;
};

struct s7d_v4l2_client {
	struct list_head list;
	struct s7d_v4l2_context *context;
	struct v4l2_fh *fh;
	struct s7d_v4l2_codec_entry *entry;
	struct module *owner;
	void *backend;
};

/* One node for every codec, as stateful decoders usually expose them. */
struct s7d_v4l2_device {
	struct device *dev;
	struct s7d_v4l2_codec_entry entry[S7D_V4L2_MAX_CODECS];
	unsigned int entries;
	struct v4l2_device v4l2;
	struct video_device *video;
	struct v4l2_m2m_dev *m2m;
	struct mutex lock;
	struct completion video_released;
	struct list_head clients;
	struct list_head quarantine;
	bool registered;
	bool disconnected;
};

static bool s7d_factory_valid(const struct s7d_v4l2_backend_factory *factory)
{
	const struct s7d_v4l2_backend_ops *ops;

	if (!factory || !s7d_v4l2_codec_valid(&factory->codec) ||
	    !factory->ready || !factory->create || !factory->destroy)
		return false;
	ops = factory->ops;
	return ops && ops->prepare && ops->submit && ops->sync_initial &&
		ops->capture_start && ops->capture_stop &&
		ops->capture_queue && ops->drain && ops->resume && ops->detach &&
		ops->reset;
}

static int s7d_backend_ready(struct s7d_v4l2_codec_entry *entry)
{
	int ret;

	if (!try_module_get(entry->factory.owner))
		return -ENODEV;
	ret = entry->factory.ready(entry->priv);
	module_put(entry->factory.owner);
	return ret > 0 ? -EIO : ret;
}

static struct s7d_v4l2_context *s7d_file_context(void *priv)
{
	struct v4l2_fh *fh = priv;

	return fh->m2m_ctx->priv;
}

static struct s7d_v4l2_client *s7d_file_client(struct s7d_v4l2_device *device, void *priv)
{
	struct s7d_v4l2_client *client;

	lockdep_assert_held(&device->lock);
	list_for_each_entry(client, &device->clients, list)
		if (client->fh == priv)
			return client;
	return NULL;
}

static struct s7d_v4l2_codec_entry *
s7d_output_entry(struct s7d_v4l2_device *device, u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < device->entries; i++)
		if (device->entry[i].factory.codec.output_format == fourcc)
			return &device->entry[i];
	return NULL;
}

static int s7d_querycap(struct file *file, void *priv,
			   struct v4l2_capability *capability)
{
	strscpy(capability->driver, "s7d-codec", sizeof(capability->driver));
	strscpy(capability->card, "S7D Video Decoder", sizeof(capability->card));
	return 0;
}

static int s7d_enum_format(struct file *file, void *priv,
			   struct v4l2_fmtdesc *format)
{
	struct s7d_v4l2_device *device = video_drvdata(file);
	struct s7d_v4l2_codec_entry *entry;

	if (format->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return s7d_v4l2_enum_format(s7d_file_context(priv), format);
	if (format->index >= device->entries)
		return -EINVAL;
	entry = &device->entry[format->index];
	s7d_v4l2_codec_output_desc(&entry->factory.codec, &entry->ops, format);
	return 0;
}

static int s7d_try_format(struct file *file, void *priv, struct v4l2_format *format)
{
	struct s7d_v4l2_device *device = video_drvdata(file);
	struct s7d_v4l2_codec_entry *entry = NULL;

	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		entry = s7d_output_entry(device, format->fmt.pix_mp.pixelformat);
	if (entry) {
		s7d_v4l2_codec_try_output(&entry->factory.codec, &format->fmt.pix_mp);
		return 0;
	}
	return s7d_v4l2_try_format(s7d_file_context(priv), format);
}

/* An OUTPUT format of another codec moves the idle context to that backend. */
static int s7d_switch_codec(struct s7d_v4l2_client *client,
			    struct s7d_v4l2_codec_entry *entry)
{
	struct s7d_v4l2_codec_entry *old = client->entry;
	struct module *owner = client->owner;
	void *backend;
	int ret;

	if (!try_module_get(entry->factory.owner))
		return -ENODEV;
	ret = entry->factory.ready(entry->priv);
	if (ret) {
		ret = ret > 0 ? -EIO : ret;
		goto put;
	}
	backend = entry->factory.create(entry->priv);
	if (IS_ERR_OR_NULL(backend)) {
		ret = backend ? PTR_ERR(backend) : -EIO;
		goto put;
	}
	ret = s7d_v4l2_context_rebind(client->context, entry->core, &entry->factory.codec,
				      &entry->ops, backend);
	if (ret) {
		entry->factory.destroy(entry->priv, backend);
		goto put;
	}
	old->factory.destroy(old->priv, client->backend);
	client->entry = entry;
	client->owner = entry->factory.owner;
	client->backend = backend;
	module_put(owner);
	return 0;
put:
	module_put(entry->factory.owner);
	return ret;
}

static int s7d_set_format(struct file *file, void *priv, struct v4l2_format *format)
{
	struct s7d_v4l2_device *device = video_drvdata(file);
	struct s7d_v4l2_codec_entry *entry;
	struct s7d_v4l2_client *client;
	int ret;

	if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		entry = s7d_output_entry(device, format->fmt.pix_mp.pixelformat);
		client = s7d_file_client(device, priv);
		if (!client)
			return -EINVAL;
		if (entry && entry != client->entry) {
			ret = s7d_switch_codec(client, entry);
			if (ret)
				return ret;
		}
	}
	return s7d_v4l2_set_format(s7d_file_context(priv), format);
}

static int s7d_get_format(struct file *file, void *priv, struct v4l2_format *format)
{
	return s7d_v4l2_get_format(s7d_file_context(priv), format);
}

static int s7d_get_selection(struct file *file, void *priv,
			     struct v4l2_selection *selection)
{
	return s7d_v4l2_get_selection(s7d_file_context(priv), selection);
}

static int s7d_decoder_cmd(struct file *file, void *priv,
			   struct v4l2_decoder_cmd *command)
{
	return s7d_v4l2_decoder_cmd(s7d_file_context(priv), command);
}

static int s7d_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *subscription)
{
	return s7d_v4l2_subscribe_event(s7d_file_context(fh), subscription);
}

static int s7d_streamon(struct file *file, void *priv, enum v4l2_buf_type type)
{
	return s7d_v4l2_streamon(s7d_file_context(priv), file, type);
}

static int s7d_streamoff(struct file *file, void *priv, enum v4l2_buf_type type)
{
	return s7d_v4l2_streamoff(s7d_file_context(priv), file, type);
}

static int s7d_reqbufs(struct file *file, void *priv,
		       struct v4l2_requestbuffers *request)
{
	return s7d_v4l2_reqbufs(s7d_file_context(priv), file, request);
}

static int s7d_dqbuf(struct file *file, void *priv, struct v4l2_buffer *buffer)
{
	return s7d_v4l2_dqbuf(s7d_file_context(priv), file, buffer);
}

static const struct v4l2_ioctl_ops s7d_ioctl_ops = {
	.vidioc_querycap = s7d_querycap,
	.vidioc_enum_fmt_vid_out = s7d_enum_format,
	.vidioc_enum_fmt_vid_cap = s7d_enum_format,
	.vidioc_try_fmt_vid_out_mplane = s7d_try_format,
	.vidioc_try_fmt_vid_cap_mplane = s7d_try_format,
	.vidioc_s_fmt_vid_out_mplane = s7d_set_format,
	.vidioc_s_fmt_vid_cap_mplane = s7d_set_format,
	.vidioc_g_fmt_vid_out_mplane = s7d_get_format,
	.vidioc_g_fmt_vid_cap_mplane = s7d_get_format,
	.vidioc_g_selection = s7d_get_selection,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_try_decoder_cmd,
	.vidioc_decoder_cmd = s7d_decoder_cmd,
	.vidioc_subscribe_event = s7d_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_reqbufs = s7d_reqbufs,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = s7d_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = s7d_streamon,
	.vidioc_streamoff = s7d_streamoff,
};

static int s7d_open(struct file *file)
{
	struct s7d_v4l2_device *device = video_drvdata(file);
	struct s7d_v4l2_client *client;
	struct s7d_v4l2_context *context;
	int ret;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	mutex_lock(&device->lock);
	if (device->disconnected || !device->registered) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!list_empty(&device->quarantine)) {
		ret = -EIO;
		goto unlock;
	}
	/* The first codec is the default until S_FMT selects another. */
	client->entry = &device->entry[0];
	client->owner = client->entry->factory.owner;
	if (!try_module_get(client->owner)) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = client->entry->factory.ready(client->entry->priv);
	if (ret) {
		if (ret > 0)
			ret = -EIO;
		goto put_owner;
	}
	client->backend = client->entry->factory.create(client->entry->priv);
	if (IS_ERR_OR_NULL(client->backend)) {
		ret = client->backend ? PTR_ERR(client->backend) : -EIO;
		goto put_owner;
	}
	context = s7d_v4l2_context_create(device->dev, device->video, device->m2m,
					client->entry->core, &client->entry->factory.codec,
					&client->entry->ops, client->backend);
	if (IS_ERR(context)) {
		ret = PTR_ERR(context);
		client->entry->factory.destroy(client->entry->priv, client->backend);
		goto put_owner;
	}
	client->context = context;
	client->fh = s7d_v4l2_fh(context);
	list_add_tail(&client->list, &device->clients);
	file->private_data = client->fh;
	mutex_unlock(&device->lock);
	return 0;
put_owner:
	module_put(client->owner);
unlock:
	mutex_unlock(&device->lock);
	kfree(client);
	return ret;
}

static int s7d_release(struct file *file)
{
	struct s7d_v4l2_device *device = video_drvdata(file);
	struct s7d_v4l2_context *context;
	struct s7d_v4l2_client *client;
	int ret;

	mutex_lock(&device->lock);
	list_for_each_entry(client, &device->clients, list) {
		if (client->fh == file->private_data)
			goto found;
	}
	mutex_unlock(&device->lock);
	return -EINVAL;
found:
	context = client->context;
	client->context = NULL;
	client->fh = NULL;
	file->private_data = NULL;

	ret = s7d_v4l2_context_destroy(context);
	if (!ret)
		client->entry->factory.destroy(client->entry->priv, client->backend);
	if (ret) {
		list_move_tail(&client->list, &device->quarantine);
		dev_err(device->dev, "decoder stop failed: %d; backend retained\n", ret);
	} else {
		list_del(&client->list);
	}
	mutex_unlock(&device->lock);
	if (!ret) {
		module_put(client->owner);
		kfree(client);
	}
	return ret;
}

static __poll_t s7d_poll(struct file *file, struct poll_table_struct *wait)
{
	return s7d_v4l2_poll(s7d_file_context(file->private_data), file, wait);
}

static const struct v4l2_file_operations s7d_file_ops = {
	.owner = THIS_MODULE,
	.open = s7d_open,
	.release = s7d_release,
	.poll = s7d_poll,
	.mmap = v4l2_m2m_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = v4l2_compat_ioctl32,
#endif
};

static void s7d_video_release(struct video_device *video)
{
	struct s7d_v4l2_device *device = video_get_drvdata(video);

	mutex_lock(&device->lock);
	device->video = NULL;
	video_device_release(video);
	complete_all(&device->video_released);
	mutex_unlock(&device->lock);
}

struct s7d_v4l2_device *s7d_v4l2_device_create(struct device *dev)
{
	struct s7d_v4l2_device *device;
	int ret;

	if (!dev)
		return ERR_PTR(-EINVAL);
	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return ERR_PTR(-ENOMEM);
	device->dev = get_device(dev);
	mutex_init(&device->lock);
	init_completion(&device->video_released);
	complete_all(&device->video_released);
	INIT_LIST_HEAD(&device->clients);
	INIT_LIST_HEAD(&device->quarantine);
	strscpy(device->v4l2.name, "s7d-codec", sizeof(device->v4l2.name));
	ret = v4l2_device_register(dev, &device->v4l2);
	if (ret)
		goto free;
	device->m2m = v4l2_m2m_init(&s7d_v4l2_m2m_ops);
	if (IS_ERR(device->m2m)) {
		ret = PTR_ERR(device->m2m);
		v4l2_device_unregister(&device->v4l2);
		goto free;
	}
	return device;
free:
	put_device(device->dev);
	kfree(device);
	return ERR_PTR(ret);
}

int s7d_v4l2_device_add_codec(struct s7d_v4l2_device *device,
			      struct s7d_codec_core *core,
			      const struct s7d_v4l2_backend_factory *factory, void *priv)
{
	struct s7d_v4l2_codec_entry *entry;
	int ret;

	if (!core)
		return -EINVAL;
	if (!s7d_factory_valid(factory))
		return -EOPNOTSUPP;
	mutex_lock(&device->lock);
	if (device->registered || device->video) {
		ret = -EBUSY;
		goto unlock;
	}
	if (device->entries == ARRAY_SIZE(device->entry) ||
	    s7d_output_entry(device, factory->codec.output_format)) {
		ret = -EINVAL;
		goto unlock;
	}
	entry = &device->entry[device->entries];
	entry->core = core;
	entry->factory = *factory;
	entry->ops = *factory->ops;
	entry->factory.ops = &entry->ops;
	entry->priv = priv;
	ret = s7d_backend_ready(entry);
	if (!ret)
		device->entries++;
unlock:
	mutex_unlock(&device->lock);
	return ret;
}

int s7d_v4l2_device_register(struct s7d_v4l2_device *device)
{
	struct video_device *video;
	bool free_video = false;
	int ret;

	mutex_lock(&device->lock);
	if (device->registered) {
		ret = -EALREADY;
		goto unlock;
	}
	if (device->disconnected || device->video || !device->entries) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = s7d_backend_ready(&device->entry[0]);
	if (ret)
		goto unlock;
	video = video_device_alloc();
	if (!video) {
		ret = -ENOMEM;
		goto unlock;
	}
	strscpy(video->name, "S7D Video Decoder", sizeof(video->name));
	video->fops = &s7d_file_ops;
	video->ioctl_ops = &s7d_ioctl_ops;
	video->release = s7d_video_release;
	video->lock = &device->lock;
	video->v4l2_dev = &device->v4l2;
	video->vfl_dir = VFL_DIR_M2M;
	video->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(video, device);
	reinit_completion(&device->video_released);
	device->video = video;
	mutex_unlock(&device->lock);

	ret = video_register_device(video, VFL_TYPE_VIDEO, -1);
	mutex_lock(&device->lock);
	if (ret) {
		device->disconnected = true;
		/* device_register failure may already have called s7d_video_release. */
		if (device->video == video && !video->dev.release)
			free_video = true;
	} else {
		device->registered = true;
	}
	mutex_unlock(&device->lock);
	if (free_video)
		s7d_video_release(video);
	return ret;
unlock:
	mutex_unlock(&device->lock);
	return ret;
}

void s7d_v4l2_device_unregister(struct s7d_v4l2_device *device)
{
	struct video_device *video = NULL;

	mutex_lock(&device->lock);
	device->disconnected = true;
	if (device->registered) {
		device->registered = false;
		video = device->video;
	}
	mutex_unlock(&device->lock);
	if (video)
		video_unregister_device(video);
}

int s7d_v4l2_device_shutdown(struct s7d_v4l2_device *device)
{
	struct s7d_v4l2_client *client;
	int ret = 0, stopped;

	s7d_v4l2_device_unregister(device);
	mutex_lock(&device->lock);
	list_for_each_entry(client, &device->clients, list) {
		stopped = s7d_v4l2_context_shutdown(client->context);
		if (stopped && !ret)
			ret = stopped;
	}
	mutex_unlock(&device->lock);
	return ret;
}

int s7d_v4l2_device_destroy(struct s7d_v4l2_device *device)
{
	int ret = 0;

	s7d_v4l2_device_unregister(device);
	mutex_lock(&device->lock);
	if (!list_empty(&device->clients) || !list_empty(&device->quarantine))
		ret = -EBUSY;
	mutex_unlock(&device->lock);
	if (ret)
		return ret;
	wait_for_completion(&device->video_released);
	/* Join the release callback's final critical section. */
	mutex_lock(&device->lock);
	mutex_unlock(&device->lock);
	v4l2_m2m_release(device->m2m);
	v4l2_device_unregister(&device->v4l2);
	put_device(device->dev);
	kfree(device);
	return 0;
}
