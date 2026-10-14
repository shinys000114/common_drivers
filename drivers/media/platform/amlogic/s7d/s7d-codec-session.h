/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_SESSION_H
#define S7D_CODEC_SESSION_H

#include <linux/types.h>
#include <linux/videodev2.h>
#include "s7d-codec-types.h"

#define S7D_CODEC_MAX_PLANES 3

struct device;
struct dma_buf;
struct module;
struct s7d_codec_core;
struct s7d_codec_session;
struct s7d_codec_surface;
struct s7d_codec_dma;

struct s7d_codec_resource_ops {
	/* Failed hold must unwind; none of these callbacks may re-enter the core. */
	int (*hold)(void *priv, enum s7d_codec_engine engine);
	/* Zero requires DMA idle and synchronized IRQ/work, not just reset. */
	int (*quiesce)(void *priv, enum s7d_codec_engine engine);
	void (*release)(void *priv, enum s7d_codec_engine engine);
};

struct s7d_codec_surface_request {
	u64 surface_id;
	u64 generation;
	u32 num_planes;
	u32 dma_alignment;
	size_t min_size[S7D_CODEC_MAX_PLANES];
};

/* Retained by value with each picture when its OUTPUT buffer is recycled. */
struct s7d_codec_timestamp_metadata {
	u32 flags;
	struct v4l2_timecode timecode;
};

struct s7d_codec_frame {
	u64 surface_id;
	u64 generation;
	u64 write_token;
	u64 timestamp_ns;
	struct s7d_codec_timestamp_metadata timestamp_meta;
	u32 sequence;
	u32 flags;
	size_t bytesused[S7D_CODEC_MAX_PLANES];
};

struct s7d_codec_callbacks {
	/* Process context; must not re-enter this core or take the queue mutex. */
	void (*frame_ready)(void *priv, const struct s7d_codec_frame *frame);
};

/* The owner serializes create/destroy; module builds pass their THIS_MODULE. */
struct s7d_codec_core *
s7d_codec_core_create(struct device *dev, struct module *owner,
		     const struct s7d_codec_resource_ops *ops, void *priv,
		     unsigned int dma_bits);
int s7d_codec_core_destroy(struct s7d_codec_core *core);
int s7d_codec_session_open(struct s7d_codec_core *core,
			   enum s7d_codec_engine engine,
			   const struct s7d_codec_callbacks *callbacks, void *priv,
			   struct s7d_codec_session **out);
void s7d_codec_session_get(struct s7d_codec_session *session);
/* Each caller holds a live session reference and its execution/module context. */
void s7d_codec_session_put(struct s7d_codec_session *session);
/*
 * Caller joins DMA/IRQ/work and DPB users, and excludes all raw surface users.
 * Success invalidates that generation's surfaces; the session stays open.
 * Retire older live generations first.
 */
int s7d_codec_session_retire_generation(struct s7d_codec_session *session,
				       u64 generation);
/* Keep the caller reference through queued-buffer return after successful stop. */
int s7d_codec_session_stop(struct s7d_codec_session *session);

struct s7d_codec_dma *
s7d_codec_dma_alloc(struct s7d_codec_session *session, size_t bytes);
void *s7d_codec_dma_cpu(struct s7d_codec_dma *mem);
dma_addr_t s7d_codec_dma_addr(struct s7d_codec_dma *mem);
size_t s7d_codec_dma_size(struct s7d_codec_dma *mem);

int s7d_codec_surface_pin(struct s7d_codec_session *session,
			  struct dma_buf *const dmabuf[],
			  const struct s7d_codec_surface_request *request,
			  struct s7d_codec_surface **out);
int s7d_codec_surface_dma(struct s7d_codec_surface *surface, unsigned int plane,
			  dma_addr_t *addr, size_t *size);
int s7d_codec_surface_queue(struct s7d_codec_surface *surface);
/* The backend has stopped acquiring this queued, non-DPB surface. */
int s7d_codec_surface_return_queued(struct s7d_codec_surface *surface);
int s7d_codec_surface_acquire(struct s7d_codec_surface *surface, u64 *token);
int s7d_codec_surface_written(struct s7d_codec_surface *surface, u64 token);
/* Physically idle, unpublished picture; consumes its sole DPB reference. */
int s7d_codec_surface_discard(struct s7d_codec_surface *surface, u64 token);
int s7d_codec_surface_ref_get(struct s7d_codec_surface *surface);
int s7d_codec_surface_ref_put(struct s7d_codec_surface *surface);
int s7d_codec_session_frame_ready(struct s7d_codec_session *session,
				  const struct s7d_codec_frame *frame);

#endif
