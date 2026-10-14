/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_V4L2_H
#define S7D_V4L2_H

#include <linux/poll.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include "s7d-codec-session.h"
#include "s7d-codec-layout.h"

struct device;
struct file;
struct video_device;
struct v4l2_fh;
struct v4l2_m2m_dev;
struct v4l2_m2m_ops;
struct s7d_v4l2_context;

struct s7d_v4l2_codec {
	enum s7d_codec_engine engine;
	u32 output_format;
	u32 max_width;
	u32 max_height;
	u32 max_input_size;
	bool nv21;
};

struct s7d_v4l2_input {
	struct s7d_codec_dma *memory;
	size_t bytes;
	size_t padded_bytes;
	u64 token;
	u64 timestamp_ns;
	struct s7d_codec_timestamp_metadata timestamp_meta;
};

struct s7d_v4l2_capture_buffer {
	struct s7d_codec_surface *surface;
	u64 surface_id;
	u64 generation;
};

struct s7d_v4l2_drain {
	u64 epoch;
	u64 last_input_token;
};

struct s7d_v4l2_reconfigure {
	u64 epoch;
	u64 input_token;
	u64 generation;
};

struct s7d_v4l2_backend_ops {
	/* Other-worker notifications may precede serialized request returns. */
	/* Requests must not call frontend notifications inline. */
	int (*prepare)(void *priv, struct s7d_codec_session *session,
		       struct s7d_v4l2_context *context);
	int (*submit)(void *priv, const struct s7d_v4l2_input *input);
	/* Join the first token's CPU callbacks before capture or hardware entry. */
	/* Headers without SPS also succeed; frontend metadata is authoritative. */
	int (*sync_initial)(void *priv, u64 token);
	int (*capture_start)(void *priv, const struct s7d_codec_layout *layout,
			     u64 generation,
			     const struct s7d_v4l2_capture_buffer *buffers,
			     unsigned int count);
	/* Join and unbind only before hardware entry; otherwise use full stop. */
	int (*capture_stop)(void *priv, u64 generation);
	int (*capture_queue)(void *priv, struct s7d_codec_surface *surface,
			     u64 surface_id, u64 generation);
	int (*drain)(void *priv, const struct s7d_v4l2_drain *request);
	int (*resume)(void *priv);
	/* Detach all frontend notifications, including on failed hardware stop. */
	void (*detach)(void *priv);
	/* Joined work and a stopped session precede release of this stream epoch. */
	int (*reset)(void *priv);
	/* Optional native same-parameter seek; only with no live stream epoch. */
	int (*clear_stream)(void *priv);
	int (*reconfigure_stop)(void *priv, const struct s7d_v4l2_reconfigure *change);
	int (*reconfigure_restore)(void *priv, struct s7d_codec_session *session,
				   struct s7d_v4l2_context *context,
				   struct s7d_codec_dma *staging,
				   const struct s7d_v4l2_reconfigure *change,
				   u64 generation);
};

/* A private component: it neither registers a video node nor supplies a backend. */
struct s7d_v4l2_context *
s7d_v4l2_context_create(struct device *dev, struct video_device *video,
			struct v4l2_m2m_dev *m2m,
			struct s7d_codec_core *core,
			const struct s7d_v4l2_codec *codec,
			const struct s7d_v4l2_backend_ops *ops, void *backend);
int s7d_v4l2_context_shutdown(struct s7d_v4l2_context *context);
/* Consumes context even on stop failure; the caller may not retry destroy. */
int s7d_v4l2_context_destroy(struct s7d_v4l2_context *context);
struct v4l2_fh *s7d_v4l2_fh(struct s7d_v4l2_context *context);
/* All contexts permanently bypass the generic M2M scheduler. */
extern const struct v4l2_m2m_ops s7d_v4l2_m2m_ops;

bool s7d_v4l2_codec_valid(const struct s7d_v4l2_codec *codec);
int s7d_v4l2_enum_format(struct s7d_v4l2_context *context,
			 struct v4l2_fmtdesc *format);
int s7d_v4l2_try_format(struct s7d_v4l2_context *context,
			struct v4l2_format *format);
int s7d_v4l2_set_format(struct s7d_v4l2_context *context,
			struct v4l2_format *format);
int s7d_v4l2_get_format(struct s7d_v4l2_context *context,
			struct v4l2_format *format);
__poll_t s7d_v4l2_poll(struct s7d_v4l2_context *context, struct file *file,
		     struct poll_table_struct *wait);
int s7d_v4l2_get_selection(struct s7d_v4l2_context *context,
			   struct v4l2_selection *selection);
int s7d_v4l2_decoder_cmd(struct s7d_v4l2_context *context,
			 struct v4l2_decoder_cmd *command);
int s7d_v4l2_subscribe_event(struct s7d_v4l2_context *context,
			     const struct v4l2_event_subscription *subscription);
int s7d_v4l2_streamon(struct s7d_v4l2_context *context, struct file *file,
		    enum v4l2_buf_type type);
int s7d_v4l2_streamoff(struct s7d_v4l2_context *context, struct file *file,
		     enum v4l2_buf_type type);

int s7d_v4l2_reqbufs(struct s7d_v4l2_context *context, struct file *file,
		     struct v4l2_requestbuffers *request);
int s7d_v4l2_dqbuf(struct s7d_v4l2_context *context, struct file *file,
		   struct v4l2_buffer *buffer);

/* Process context; core frame notification alone is allowed under its mutex. */
int s7d_v4l2_sequence(struct s7d_v4l2_context *context,
		      const struct s7d_codec_layout *layout);
int s7d_v4l2_input_done(struct s7d_v4l2_context *context, u64 token, int status);
/* Notification only; retirement joins the backend before returning buffers. */
int s7d_v4l2_error(struct s7d_v4l2_context *context, int error);
int s7d_v4l2_drain_done(struct s7d_v4l2_context *context, u64 epoch, u64 token);
/* Explicit cancellation identity, distinct from a completed operation error. */
int s7d_v4l2_reconfigure_result(struct s7d_v4l2_context *context,
				const struct s7d_codec_layout *layout,
				const struct s7d_v4l2_reconfigure *change,
				bool *cancelled);
int s7d_v4l2_sequence_result(struct s7d_v4l2_context *context,
			     const struct s7d_codec_layout *layout, bool *cancelled);
int s7d_v4l2_input_done_result(struct s7d_v4l2_context *context,
			       u64 token, int status, bool *cancelled);
int s7d_v4l2_drain_done_result(struct s7d_v4l2_context *context,
			       u64 epoch, u64 token, bool *cancelled);

#endif
