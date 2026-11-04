/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_V4L2_H
#define S7D_V4L2_H

#include <linux/align.h>
#include <linux/math.h>
#include <linux/poll.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include "s7d-codec-session.h"
#include "s7d-codec-layout.h"

#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '0', '1')
#endif

/*
 * Driver-private CAPTURE formats of the AV1, HEVC and VP9 decoders: each frame in Amlogic FBC
 * (scatter layout) followed by its linear 8-bit NV12 copy, in a single plane.
 * AM08C carries an 8-bit stream (mem-saving body), AM10C a 10-bit one (full
 * body); a stream is only offered the one of its own depth. w and h are the
 * CAPTURE format's width and height (G_FMT: coded width, height rounded up to
 * 64); G_SELECTION gives the visible part.
 *
 *   offset 0:           FBC header, 32 bytes per 32x32 block, hdr =
 *                       ALIGN(DIV_ROUND_UP(w, 32) * DIV_ROUND_UP(h, 32) * 32,
 *                       4096) bytes reserved. Each little-endian u32 word,
 *                       shifted left by 2, is the physical address of its
 *                       compressed data in the body.
 *   offset hdr:         FBC body, body = ALIGN(DIV_ROUND_UP(w, 64) *
 *                       DIV_ROUND_UP(h, 32) * blk, 4096) bytes, 4 KiB aligned,
 *                       with blk = 3200 (AM08C) or 4096 (AM10C).
 *   offset nv12 = hdr + body:
 *                       NV12 Y, pitch = bytesperline = ALIGN(w, 64), h rows;
 *   offset nv12 + pitch * h:
 *                       NV12 interleaved CbCr, same pitch, h / 2 rows.
 *
 * sizeimage = nv12 + pitch * h * 3 / 2. The header holds absolute addresses,
 * so the buffer must stay where the driver wrote it (export it, never copy).
 * The decoder writes each picture straight into a buffer and keeps reading
 * it as a reference while the stream refers to it; such a buffer is not
 * written again until the reference is gone, even when it is requeued, so
 * V4L2_CID_MIN_BUFFERS_FOR_CAPTURE covers the reference frames.
 * The NV12 copy of an AM10C frame is the decoder's 8-bit double write output.
 * DRM import, offsets[0] = 0, pitches[0] = w:
 *   AM08C: DRM_FORMAT_YUV420_8BIT with modifier
 *          DRM_FORMAT_MOD_AMLOGIC_FBC(AMLOGIC_FBC_LAYOUT_SCATTER,
 *          AMLOGIC_FBC_OPTION_MEM_SAVING);
 *   AM10C: DRM_FORMAT_YUV420_10BIT with modifier
 *          DRM_FORMAT_MOD_AMLOGIC_FBC(AMLOGIC_FBC_LAYOUT_SCATTER, 0);
 * or, for either, plain DRM_FORMAT_NV12 at offsets nv12 and nv12 + pitch * h,
 * pitches = pitch.
 */
#define V4L2_PIX_FMT_AM08C v4l2_fourcc('A', '0', '8', 'C') /* Amlogic 8-bit FBC, scatter, mem-saving, plus NV12 */
#define V4L2_PIX_FMT_AM10C v4l2_fourcc('A', '1', '0', 'C') /* Amlogic 10-bit FBC, scatter, + NV12 */

/*
 * AM08F/AM10F: AM08C/AM10C without the NV12 copy. The single plane holds the
 * FBC header at offset 0 and the FBC body at offset hdr, with the offsets,
 * sizes and alignment documented above, and nothing after the body:
 * sizeimage = hdr + body. bytesperline stays ALIGN(w, 64) as for
 * AM08C/AM10C; no row of the buffer has that pitch. The decoder writes only
 * its compressed output (no double write). AM08F carries an 8-bit stream
 * (mem-saving body), AM10F a 10-bit one (full body); a stream is only offered
 * the one of its own depth, and a client that set the other one gets NV12, as
 * with AM08C/AM10C. References, V4L2_CID_MIN_BUFFERS_FOR_CAPTURE and the
 * absolute header addresses work as for AM08C/AM10C. DRM import is the FBC
 * one above (offsets[0] = 0, pitches[0] = w, the same format and modifier);
 * there is no NV12 alternative.
 */
#define V4L2_PIX_FMT_AM08F v4l2_fourcc('A', '0', '8', 'F') /* Amlogic 8-bit FBC, scatter, mem-saving */
#define V4L2_PIX_FMT_AM10F v4l2_fourcc('A', '1', '0', 'F') /* Amlogic 10-bit FBC, scatter */

/* Header bytes the scatter layout uses for a picture; the same for both depths. */
static inline u32 s7d_amfbc_header_used(u32 width, u32 height)
{
	return DIV_ROUND_UP(width, 32) * DIV_ROUND_UP(height, 32) * 32;
}

static inline u32 s7d_amfbc_header_size(u32 width, u32 height)
{
	return ALIGN(s7d_amfbc_header_used(width, height), SZ_4K);
}

/* Body bytes per 64x32 block: 3200 with 8-bit mem saving, 4096 for 10-bit. */
static inline u32 s7d_amfbc_body_size(u32 width, u32 height, u32 bit_depth)
{
	return ALIGN(DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 32) *
		     (bit_depth == 10 ? 4096 : 3200), SZ_4K);
}

/* The NV12 copy follows the reserved FBC header and body. */
static inline u32 s7d_amfbc_nv12_offset(u32 width, u32 height, u32 bit_depth)
{
	return s7d_amfbc_header_size(width, height) +
		s7d_amfbc_body_size(width, height, bit_depth);
}

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
	u32 min_input_size;
	u32 max_capture_buffers; /* Zero keeps the common limit. */
	u32 staging_headroom;
	int (*default_layout)(u32 width, u32 height, u32 fourcc,
			      struct s7d_codec_layout *layout);
	bool nv21;
	/* CAPTURE may also offer AM08C/AM10C and AM08F/AM10F (AV1, HEVC and VP9). */
	bool fbc;
	/*
	 * AM08C/AM10C/AM08F/AM10F CAPTURE minimum, when the decoder keeps its references in
	 * the CAPTURE buffers themselves; zero keeps the layout's minimum.
	 */
	u32 fbc_min_capture_buffers;
};

int s7d_v4l2_default_nv12_layout(u32 width, u32 height, u32 fourcc,
				struct s7d_codec_layout *layout);

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

/* Caller validates layout/count and preserves the serialized surface pool. */
int s7d_v4l2_validate_capture_pool(const struct s7d_codec_layout *layout,
				   u64 generation,
				   const struct s7d_v4l2_capture_buffer *buffers,
				   unsigned int count);

/*
 * Where an AM08C/AM10C/AM08F/AM10F CAPTURE buffer holds a picture, as the
 * format documents.
 */
struct s7d_amfbc_buffer {
	u32 header;
	u32 header_bytes;
	u32 body;
	u32 body_bytes;
	/* NV12 luma; chroma follows at nv12 + nv12_luma_bytes. Zero for AM08F/AM10F. */
	u32 nv12;
	u32 nv12_luma_bytes;
};

/* AM08F/AM10F: the buffer has no NV12 copy; the decoder writes FBC only. */
static inline bool s7d_amfbc_fbc_only(const struct s7d_amfbc_buffer *buffer)
{
	return !buffer->nv12_luma_bytes;
}

/*
 * A pool the decoder writes straight into: every buffer must carry the FBC
 * of @layout's AM08C/AM10C format (header, body 4 KiB aligned, NV12 right
 * behind) or every buffer that of its AM08F/AM10F format (header and body
 * only), all below 4 GiB. *fbc reads false, with no regions filled, when no
 * buffer carries FBC; a mixed pool is -EINVAL.
 */
int s7d_v4l2_fbc_capture_pool(const struct s7d_codec_layout *layout,
			      const struct s7d_v4l2_capture_buffer *buffers,
			      unsigned int count, struct s7d_amfbc_buffer *regions,
			      bool *fbc);
/*
 * What a DQBUF of @region's buffer reports: AM08C/AM10C the whole buffer (FBC
 * as the prefix, then the NV12 planes of @layout), AM08F/AM10F header and body.
 */
void s7d_v4l2_fbc_frame_bytes(const struct s7d_codec_layout *layout,
			      const struct s7d_amfbc_buffer *region,
			      struct s7d_codec_frame *frame);

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
/* Switch an idle context (no buffers, no session) to another codec backend. */
int s7d_v4l2_context_rebind(struct s7d_v4l2_context *context, struct s7d_codec_core *core,
			    const struct s7d_v4l2_codec *codec,
			    const struct s7d_v4l2_backend_ops *ops, void *backend);
int s7d_v4l2_context_shutdown(struct s7d_v4l2_context *context);
/* Consumes context even on stop failure; the caller may not retry destroy. */
int s7d_v4l2_context_destroy(struct s7d_v4l2_context *context);
struct v4l2_fh *s7d_v4l2_fh(struct s7d_v4l2_context *context);
/* All contexts permanently bypass the generic M2M scheduler. */
extern const struct v4l2_m2m_ops s7d_v4l2_m2m_ops;

bool s7d_v4l2_codec_valid(const struct s7d_v4l2_codec *codec);
void s7d_v4l2_codec_output_desc(const struct s7d_v4l2_codec *codec,
				 const struct s7d_v4l2_backend_ops *ops,
				 struct v4l2_fmtdesc *format);
void s7d_v4l2_codec_try_output(const struct s7d_v4l2_codec *codec,
			       struct v4l2_pix_format_mplane *format);
int s7d_v4l2_enum_format(struct s7d_v4l2_context *context,
			 struct v4l2_fmtdesc *format);
int s7d_v4l2_try_format(struct s7d_v4l2_context *context,
			struct v4l2_format *format);
struct s7d_codec_session *s7d_v4l2_session(struct s7d_v4l2_context *ctx);
/* Callers hold the queue lock or are serialized with s7d_v4l2_context_rebind(). */
enum s7d_codec_engine s7d_v4l2_engine(struct s7d_v4l2_context *ctx);
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
