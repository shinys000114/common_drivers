// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <asm/barrier.h>
#include <asm/byteorder.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "s7d-av1-input.h"
#include "s7d-av1-runner.h"
#include "s7d-av1-v4l2.h"
#include "s7d-codec-dos.h"
#include "s7d-v4l2-device.h"

/* A UHD key TU can exceed 6 MiB. */
#define AV1_INPUT_BYTES SZ_8M
/* Each staged OBU gains a 20-byte record header: room for 52k OBUs per TU. */
#define AV1_STAGING_BYTES (AV1_INPUT_BYTES + SZ_1M)

/* The S7D HEVC core decodes AV1 up to UHD, as the vendor driver allows. */
#define AV1_MAX_WIDTH		4096
#define AV1_MAX_HEIGHT		2304

/*
 * The eight reference frames and the picture being decoded live in CAPTURE
 * buffers, plus one the client keeps on display, as for VP9.
 */
#define AV1_CAPTURE_CURRENT	2U
#define AV1_CAPTURE_MINIMUM	(S7D_AV1_DPB_REFERENCES + AV1_CAPTURE_CURRENT)

/* What the decoder does with a CAPTURE buffer. */
enum av1_bind_state {
	AV1_BIND_FREE,		/* not the decoder's: queued, or the client's */
	AV1_BIND_PENDING,	/* acquired for the picture about to be decoded */
	AV1_BIND_HIDDEN,	/* decoded and held by the DPB, never given out */
	AV1_BIND_SHOWN,		/* held by the DPB and given out at least once */
};

struct av1_bind {
	enum av1_bind_state state;
	/* Surface write token of the picture it holds. */
	u64 token;
};

struct av1_backend {
	struct s7d_av1_v4l2 *adapter;
	struct list_head node;
	struct mutex lock;
	struct mutex notifications;
	struct mutex stop;
	struct work_struct work;
	struct s7d_codec_session *session;
	struct s7d_v4l2_context *frontend;
	struct s7d_v4l2_input input;
	struct s7d_av1_tu tu;
	struct s7d_v4l2_drain drain;
	struct s7d_v4l2_capture_buffer capture[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	/*
	 * Each buffer is a DPB picture's memory (cookie = index + 1): the NV12
	 * double write, with AM08C/AM10C also its FBC header and body at the
	 * exported offsets. A DPB-held buffer keeps one surface DPB reference, so
	 * it is not written again even when the client requeues it.
	 */
	struct av1_bind bind[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_av1_slot_memory slot_memory[S7D_CODEC_MAX_CAPTURE_BUFFERS];
	struct s7d_codec_layout layout;
	struct s7d_av1_sequence sequence;
	u8 sequence_payload[S7D_AV1_SEQUENCE_MAX_BYTES];
	u32 sequence_bytes;
	struct s7d_av1_decode_result result;
	u64 lease_epoch;
	u64 generation;
	u64 initial_token;
	u64 last_submitted;
	u64 last_completed;
	u64 picture_token;
	u32 next_picture;
	u32 captures;
	/* Index + 1 of the buffer acquired for the next picture. */
	u32 pending;
	u32 frame_sequence;
	int failure;
	int stop_error;
	bool closing;
	bool detached;
	bool input_pending;
	bool preflight;
	bool initial_complete;
	bool sequence_valid;
	bool sequence_notified;
	bool output_pending;
	bool capture_blocked;
	bool execution_attempted;
	bool drain_pending;
	bool drain_notified;
	/* A key frame was taken since the session began: decoding has a start. */
	bool keyed;
	/* The session's first picture runs next: it needs the sequence header. */
	bool seed_sequence;
	/* The CAPTURE buffers are bound as DPB picture memory. */
	bool bound;
	/* AM08C/AM10C: they hold the FBC header and body too. */
	bool fbc;
};

struct s7d_av1_v4l2 {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_dos *dos;
	struct s7d_codec_core *core;
	struct s7d_av1_runner *runner;
	struct s7d_v4l2_backend_factory factory;
	struct mutex clients;
	struct mutex resources;
	struct list_head instances;
	struct av1_backend *active;
	u64 lease_epoch;
	int stop_error;
	int release_error;
	bool lease_active;
	bool runner_closed;
	bool quiesced;
	bool release_pin;
	bool closing;
};

static int av1_hold(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	mutex_lock(&adapter->resources);
	if (READ_ONCE(adapter->closing) || adapter->release_error)
		ret = -ESHUTDOWN;
	else if (adapter->lease_epoch == U64_MAX)
		ret = -EOVERFLOW;
	else
		ret = s7d_codec_dos_hold(adapter->dos, engine);
	if (!ret) {
		adapter->lease_epoch++;
		adapter->lease_active = true;
		adapter->runner_closed = true;
		adapter->quiesced = false;
		adapter->stop_error = 0;
	}
	mutex_unlock(&adapter->resources);
	return ret;
}

static int av1_quiesce(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	if (engine != S7D_CODEC_ENGINE_HEVC)
		return -EOPNOTSUPP;
	mutex_lock(&adapter->resources);
	ret = adapter->stop_error;
	if (!ret && !adapter->runner_closed)
		ret = -EBUSY;
	if (!ret)
		ret = s7d_codec_dos_prepare_access(adapter->dos, engine);
	if (!ret)
		ret = s7d_codec_dos_hevc_irq_mask(adapter->dos, false);
	if (!ret)
		ret = s7d_codec_dos_halt(adapter->dos, engine);
	if (!ret)
		adapter->quiesced = true;
	mutex_unlock(&adapter->resources);
	return ret;
}

static void av1_release(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	mutex_lock(&adapter->resources);
	ret = engine != S7D_CODEC_ENGINE_HEVC || !adapter->quiesced ? -EBUSY :
		s7d_codec_dos_release(adapter->dos, engine);
	if (!ret) {
		adapter->lease_active = false;
	} else {
		adapter->release_error = ret;
		if (!adapter->release_pin && try_module_get(adapter->owner))
			adapter->release_pin = true;
		dev_err(adapter->dev, "AV1 resource release failed: %d; owner retained\n", ret);
	}
	mutex_unlock(&adapter->resources);
}

static const struct s7d_codec_resource_ops av1_resource_ops = {
	.hold = av1_hold,
	.quiesce = av1_quiesce,
	.release = av1_release,
};

/*
 * A stream that starts, or resumes after a seek, at an inter frame: drop the
 * pictures up to the first key frame, the AV1 random access point, before the
 * firmware sees them. Pictures that follow it in the TU decode as usual.
 */
static void av1_skip_to_key(struct av1_backend *backend)
{
	struct s7d_av1_tu *tu = &backend->tu;
	u32 i;

	lockdep_assert_held(&backend->lock);
	if (backend->keyed)
		return;
	for (i = 0; i < tu->picture_count; i++)
		if (!tu->picture[i].show_existing &&
		    tu->picture[i].frame_type == S7D_AV1_KEY_FRAME)
			break;
	if (i == tu->picture_count) {
		tu->picture_count = 0;
		return;
	}
	backend->next_picture = i;
	backend->keyed = true;
	backend->seed_sequence = true;
}

/*
 * The first picture of a session brings the sequence header to the firmware.
 * A key frame without one in front (a stream that sent it once, at its
 * start, or a key frame that is not the first picture of its TU) gets the
 * stored one, after the temporal delimiter. *out is the caller's to free.
 */
static int av1_seed_sequence(struct av1_backend *backend, const u8 *data, u32 bytes,
			     u8 **out, u32 *total)
{
	u32 offset = 0, insert = 0, header, size;
	struct s7d_av1_obu obu;
	u8 *buffer;
	int ret;

	*out = NULL;
	while (offset < bytes) {
		ret = s7d_av1_obu_next(data, bytes, offset, &obu);
		if (ret)
			return ret;
		if (obu.type == S7D_AV1_OBU_SEQUENCE)
			return 0;
		if (obu.type == S7D_AV1_OBU_FRAME || obu.type == S7D_AV1_OBU_FRAME_HEADER)
			break;
		offset += obu.bytes;
		if (obu.type == S7D_AV1_OBU_TEMPORAL_DELIMITER)
			insert = offset;
	}
	/* OBU header with the size field, and the size in one or two leb128 bytes. */
	header = backend->sequence_bytes < 0x80 ? 2 : 3;
	if (!backend->sequence_bytes || backend->sequence_bytes >= 0x4000 ||
	    check_add_overflow(bytes, header + backend->sequence_bytes, &size))
		return -EINVAL;
	if (size > AV1_INPUT_BYTES)
		return -E2BIG;
	buffer = kvmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	memcpy(buffer, data, insert);
	buffer[insert] = S7D_AV1_OBU_SEQUENCE << 3 | 0x02;
	if (header == 2) {
		buffer[insert + 1] = backend->sequence_bytes;
	} else {
		buffer[insert + 1] = (backend->sequence_bytes & 0x7f) | 0x80;
		buffer[insert + 2] = backend->sequence_bytes >> 7;
	}
	memcpy(buffer + insert + header, backend->sequence_payload, backend->sequence_bytes);
	memcpy(buffer + insert + header + backend->sequence_bytes, data + insert,
	       bytes - insert);
	*out = buffer;
	*total = size;
	return 0;
}

static int av1_metadata(struct av1_backend *backend)
{
	const u8 *data = s7d_codec_dma_cpu(backend->input.memory);
	const struct s7d_av1_sequence *active;
	const u8 *payload = NULL;
	struct s7d_av1_sequence sequence;
	struct s7d_av1_obu obu;
	struct s7d_codec_colour colour = { };
	u32 offset = 0, payload_bytes = 0, start = 0, i;
	bool picture = false;
	int ret;

	lockdep_assert_held(&backend->lock);
	if (!data || backend->input.bytes > s7d_codec_dma_size(backend->input.memory))
		return -EINVAL;
	/*
	 * GStreamer queues the caps codec_data, an ISOBMFF av1C record, by
	 * itself. Its first byte has the OBU forbidden bit set; its config OBUs
	 * follow a four-byte header.
	 */
	if (backend->input.bytes >= 4 && data[0] == 0x81)
		offset = start = 4;
	/* A record without config OBUs: the sequence comes with the first TU. */
	if (offset && offset == backend->input.bytes) {
		memset(&backend->tu, 0, sizeof(backend->tu));
		return 1;
	}
	while (offset < backend->input.bytes) {
		ret = s7d_av1_obu_next(data, backend->input.bytes, offset, &obu);
		if (ret)
			return ret;
		offset += obu.bytes;
		picture |= obu.type == S7D_AV1_OBU_FRAME || obu.type == S7D_AV1_OBU_FRAME_HEADER;
		if (obu.type != S7D_AV1_OBU_SEQUENCE)
			continue;
		if (payload)
			return -EOPNOTSUPP;
		payload = data + obu.payload_offset;
		payload_bytes = obu.payload_bytes;
		ret = s7d_av1_parse_sequence(payload, payload_bytes, &sequence);
		if (!ret)
			ret = s7d_av1_admit_sequence(&sequence);
		if (ret)
			return ret;
	}
	/* Before any sequence header nothing decodes: skip the TU. */
	if (!payload && !backend->sequence_valid) {
		memset(&backend->tu, 0, sizeof(backend->tu));
		return 1;
	}
	active = payload ? &sequence : &backend->sequence;
	if (!picture) {
		/* Headers only: take the sequence, decode nothing. */
		if (!payload)
			return -EBADMSG;
		memset(&backend->tu, 0, sizeof(backend->tu));
	} else {
		ret = s7d_av1_split_tu(data + start, backend->input.bytes - start, active,
				       &backend->tu);
		if (ret)
			return ret;
		for (i = 0; i < backend->tu.picture_count; i++)
			backend->tu.picture[i].offset += start;
		av1_skip_to_key(backend);
	}
	if (!payload)
		return 0;
	if (sequence.max_frame_width < 64 || sequence.max_frame_height < 64 ||
	    sequence.max_frame_width > AV1_MAX_WIDTH ||
	    sequence.max_frame_height > AV1_MAX_HEIGHT ||
	    ((sequence.max_frame_width | sequence.max_frame_height) & 1))
		return -EOPNOTSUPP;
	if (backend->sequence_valid)
		return backend->sequence_bytes != payload_bytes ||
			memcmp(payload, backend->sequence_payload, payload_bytes) ? -EOPNOTSUPP : 0;
	ret = s7d_v4l2_default_nv12_layout(sequence.max_frame_width,
					sequence.max_frame_height,
					V4L2_PIX_FMT_NV12M, &backend->layout);
	if (ret)
		return ret;
	/* The CAPTURE NV12 double write stays 8-bit; the depth selects the FBC format. */
	backend->layout.bit_depth = sequence.bit_depth;
	/* Every format: the references and the decoded picture sit in CAPTURE buffers. */
	backend->layout.dpb_limit = S7D_AV1_DPB_REFERENCES;
	backend->layout.dpb_slots = S7D_AV1_DPB_REFERENCES;
	backend->layout.current_slots = AV1_CAPTURE_CURRENT;
	backend->layout.extra_surfaces = 0;
	backend->layout.min_capture_buffers = AV1_CAPTURE_MINIMUM;
	ret = s7d_codec_validate_layout(&backend->layout);
	if (ret)
		return ret;
	colour.flags = S7D_CODEC_COLOUR_SIGNAL_PRESENT;
	if (sequence.color_range)
		colour.flags |= S7D_CODEC_COLOUR_FULL_RANGE;
	if (sequence.color_description_present) {
		colour.flags |= S7D_CODEC_COLOUR_DESCRIPTION_PRESENT;
		colour.primaries = sequence.color_primaries;
		colour.transfer = sequence.transfer_characteristics;
		colour.matrix = sequence.matrix_coefficients;
	}
	ret = s7d_codec_resolve_colour(&colour, sequence.max_frame_width,
				     sequence.max_frame_height, &backend->layout.colourimetry);
	if (ret)
		return ret;
	backend->sequence = sequence;
	memcpy(backend->sequence_payload, payload, payload_bytes);
	backend->sequence_bytes = payload_bytes;
	backend->sequence_valid = true;
	return 0;
}

static int av1_notify_sequence(struct av1_backend *backend, bool *cancelled)
{
	int ret;

	*cancelled = false;
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		ret = s7d_v4l2_sequence_result(backend->frontend, &backend->layout, cancelled);
	else {
		*cancelled = true;
		ret = -ECANCELED;
	}
	mutex_unlock(&backend->notifications);
	return ret;
}

static void av1_notify_error(struct av1_backend *backend, int error)
{
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		s7d_v4l2_error(backend->frontend, error);
	mutex_unlock(&backend->notifications);
}

static int av1_complete_input(struct av1_backend *backend, u64 token, int status)
{
	bool cancelled = false;
	int ret;

	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing)) {
		if (status)
			s7d_v4l2_error(backend->frontend, status);
		ret = s7d_v4l2_input_done_result(backend->frontend, token, status, &cancelled);
	} else {
		cancelled = true;
		ret = -ECANCELED;
	}
	mutex_unlock(&backend->notifications);
	if (ret && !cancelled) {
		mutex_lock(&backend->lock);
		backend->failure = ret;
		mutex_unlock(&backend->lock);
		av1_notify_error(backend, ret);
	}
	return ret;
}

static bool av1_copy_cancelled(void *priv)
{
	struct av1_backend *backend = priv;

	return READ_ONCE(backend->closing);
}

/* A free CAPTURE buffer, write-pinned with one DPB reference. */
static int av1_acquire_free(struct av1_backend *backend, u32 *index, u64 *token)
{
	struct s7d_codec_surface *surface;
	u32 i, pass;
	int ret;

	/* First a buffer no importer still reads, then any. */
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < backend->captures; i++) {
			if (backend->bind[i].state != AV1_BIND_FREE)
				continue;
			surface = backend->capture[i].surface;
			if (!pass && s7d_codec_surface_reading(surface))
				continue;
			ret = s7d_codec_surface_acquire(surface, token);
			if (ret == -EAGAIN)
				continue;
			if (ret)
				return ret;
			*index = i;
			return s7d_codec_surface_ref_get(surface);
		}
	return -EAGAIN;
}

/* The next decoded picture goes straight into a free CAPTURE buffer. */
static int av1_acquire_target(struct av1_backend *backend)
{
	u32 index;
	u64 token;
	int ret;

	if (backend->pending)
		return 0;
	ret = av1_acquire_free(backend, &index, &token);
	if (ret)
		return ret;
	backend->bind[index] = (struct av1_bind) { .state = AV1_BIND_PENDING, .token = token };
	backend->pending = index + 1;
	return 0;
}

/* The DPB let go of this buffer; nothing reads or writes it for that slot again. */
static int av1_release_capture(struct av1_backend *backend, u32 index)
{
	struct s7d_codec_surface *surface = backend->capture[index].surface;
	struct av1_bind *bind = &backend->bind[index];
	int ret = 0;

	switch (bind->state) {
	case AV1_BIND_PENDING:
		/* Bound for a picture that turned out to be a show-existing frame. */
		ret = s7d_codec_surface_written(surface, bind->token);
		fallthrough;
	case AV1_BIND_HIDDEN:
		/* Never given out: straight back to the free pool. */
		if (!ret)
			ret = s7d_codec_surface_discard(surface, bind->token);
		break;
	case AV1_BIND_SHOWN:
		/* Free again once the client requeues it, if it has not already. */
		ret = s7d_codec_surface_ref_put(surface);
		break;
	default:
		return -EPROTO;
	}
	if (ret)
		return ret;
	if (backend->pending == index + 1)
		backend->pending = 0;
	memset(bind, 0, sizeof(*bind));
	return 0;
}

static int av1_unbind(struct av1_backend *backend)
{
	u64 cookie[S7D_AV1_DPB_MAX_SLOTS];
	u32 i, count;
	int ret;

	if (!backend->bound)
		return 0;
	ret = s7d_av1_runner_unbind(backend->adapter->runner, cookie, &count);
	for (i = 0; !ret && i < count; i++)
		ret = cookie[i] && cookie[i] <= backend->captures ?
			av1_release_capture(backend, cookie[i] - 1) : -EPROTO;
	return ret;
}

/* After a decode: the picture now sits in its buffer; drop what the DPB dropped. */
static int av1_decoded(struct av1_backend *backend)
{
	struct av1_bind *bind;
	int ret;

	if (!backend->bound)
		return 0;
	if (backend->pending && backend->result.picture_decoded) {
		bind = &backend->bind[backend->pending - 1];
		ret = s7d_codec_surface_written(backend->capture[backend->pending - 1].surface,
						bind->token);
		if (ret)
			return ret;
		bind->state = AV1_BIND_HIDDEN;
		backend->pending = 0;
	}
	/* A shown picture stays owned until published; release after that. */
	return backend->result.output_ready ? 0 : av1_unbind(backend);
}

/* Hand a written CAPTURE buffer holding the output picture to the client. */
static int av1_deliver(struct av1_backend *backend, u32 index, u64 token)
{
	const struct s7d_av1_reference_output *output = &backend->result.output;
	const struct s7d_av1_slot_memory *memory = &backend->slot_memory[index];
	struct s7d_codec_frame frame = { };
	dma_addr_t base;
	size_t prefix;
	int ret;

	ret = s7d_codec_surface_prefix(backend->capture[index].surface, &base, &prefix);
	if (ret)
		return ret;
	frame.surface_id = backend->capture[index].surface_id;
	frame.generation = backend->generation;
	frame.write_token = token;
	frame.timestamp_ns = output->output.metadata.timestamp_ns;
	frame.timestamp_meta = backend->input.timestamp_meta;
	frame.sequence = backend->frame_sequence;
	if (output->output.picture.frame_type == S7D_AV1_KEY_FRAME)
		frame.flags = S7D_CODEC_FRAME_KEY;
	if (backend->fbc && !memory->luma.bytes) {
		/* AM08F/AM10F report the FBC header and body. */
		frame.bytesused[0] = memory->header.bytes;
		frame.bytesused[1] = memory->body.bytes;
	} else if (prefix) {
		/* AM08C/AM10C report the whole buffer: FBC, then NV12 up to its padded height. */
		frame.prefix_bytesused = prefix;
		frame.bytesused[0] = backend->layout.size[0];
		frame.bytesused[1] = backend->layout.size[1];
	} else {
		frame.bytesused[0] = backend->layout.pitch[0] * backend->layout.coded_height;
		frame.bytesused[1] = frame.bytesused[0] / 2;
	}
	mutex_lock(&backend->notifications);
	if (backend->frontend && !READ_ONCE(backend->closing))
		ret = s7d_codec_session_frame_ready(backend->session, &frame);
	else
		ret = -ECANCELED;
	mutex_unlock(&backend->notifications);
	return ret;
}

/*
 * The header words of a copied AM08C/AM10C/AM08F/AM10F picture are physical
 * addresses >> 2 into the source body; point them into the copy's body instead.
 */
static int av1_fbc_rebase(struct av1_backend *backend, void *prefix,
			  const struct s7d_av1_slot_memory *source,
			  const struct s7d_av1_slot_memory *target)
{
	const struct s7d_codec_layout *layout = &backend->layout;
	u32 used = s7d_amfbc_header_used(layout->coded_width, layout->coded_height);
	u64 body = source->body.address, body_end = body + source->body.bytes;
	u64 first = source->header.address;
	/* The source buffer: header, body and, unless AM08F/AM10F, NV12. */
	u64 last = source->chroma.bytes ?
		(u64)source->chroma.address + source->chroma.bytes : body_end;
	__le32 *header = prefix;
	u32 i, word, stray = 0;
	u64 address;

	if (!used || used % 4 || used > source->header.bytes || used > target->header.bytes)
		return -ERANGE;
	for (i = 0; i < used / 4; i++) {
		word = le32_to_cpu(header[i]);
		address = (u64)word << 2;
		if (address >= body && address < body_end)
			header[i] = cpu_to_le32((u32)((address - body + target->body.address) >> 2));
		else if (address >= first && address < last)
			stray++;
	}
	if (stray) {
		dev_warn_ratelimited(backend->adapter->dev,
				     "AV1 FBC: %u header words point outside the %u-byte body\n",
				     stray, source->body.bytes);
		return -EIO;
	}
	/* Rebased header stores precede the publication that exports them. */
	dma_wmb();
	return 0;
}

/*
 * Fallback: the picture must be shown again while the client still holds its
 * buffer (show-existing of a frame already given out), so copy it into a free
 * one; an AM08C/AM10C copy takes the whole buffer and gets its header rebased,
 * an AM08F/AM10F one its header and body (its two planes), rebased the same.
 */
static int av1_publish_copy(struct av1_backend *backend, u32 source)
{
	const struct s7d_av1_slot_memory *memory = &backend->slot_memory[source];
	const bool fbc_only = backend->fbc && !memory->luma.bytes;
	const size_t bytes[2] = {
		fbc_only ? memory->header.bytes : backend->layout.size[0],
		fbc_only ? memory->body.bytes : backend->layout.size[1],
	};
	struct s7d_codec_surface *surface, *from = backend->capture[source].surface;
	u64 from_token = backend->bind[source].token;
	u32 index;
	u64 token;
	void *prefix;
	int ret, cleanup;

	ret = av1_acquire_free(backend, &index, &token);
	if (ret)
		return ret;
	surface = backend->capture[index].surface;
	if (!backend->fbc || fbc_only)
		ret = s7d_codec_surface_copy(surface, token, from, from_token, bytes,
					     av1_copy_cancelled, backend);
	else
		ret = s7d_codec_surface_copy_all(surface, token, from, from_token, bytes,
						 av1_copy_cancelled, backend);
	if (!ret && backend->fbc) {
		ret = s7d_codec_surface_prefix_cpu(surface, token, &prefix);
		if (!ret)
			ret = av1_fbc_rebase(backend, prefix, &backend->slot_memory[source],
					     &backend->slot_memory[index]);
	}
	cleanup = s7d_codec_surface_written(surface, token);
	if (!ret)
		ret = cleanup;
	if (!ret && av1_copy_cancelled(backend))
		ret = -ECANCELED;
	if (!ret)
		ret = av1_deliver(backend, index, token);
	if (ret) {
		if (!cleanup)
			s7d_codec_surface_discard(surface, token);
		return ret;
	}
	return s7d_codec_surface_ref_put(surface);
}

/*
 * The hardware wrote the picture into its CAPTURE buffer; give that buffer
 * out. A picture shown again (show-existing) goes out in place
 * when the client has requeued its buffer, and as a copy while it holds it.
 */
static int av1_publish_bound(struct av1_backend *backend)
{
	u64 cookie = backend->result.output.cookie;
	struct av1_bind *bind;
	u32 index;
	int ret;

	if (!cookie || cookie > backend->captures)
		return -EPROTO;
	index = cookie - 1;
	bind = &backend->bind[index];
	if (bind->state == AV1_BIND_SHOWN) {
		ret = s7d_codec_surface_reshow(backend->capture[index].surface, bind->token);
		if (ret == -EBUSY)
			return av1_publish_copy(backend, index);
		if (ret)
			return ret;
		bind->state = AV1_BIND_HIDDEN;
	} else if (bind->state != AV1_BIND_HIDDEN) {
		return -EPROTO;
	}
	ret = av1_deliver(backend, index, bind->token);
	if (ret)
		return ret;
	bind->state = AV1_BIND_SHOWN;
	return 0;
}

static int av1_publish(struct av1_backend *backend)
{
	const struct s7d_av1_reference_output *output = &backend->result.output;
	int ret;

	if (output->output.picture.width != backend->layout.coded_width ||
	    output->output.picture.height != backend->layout.coded_height ||
	    output->pitch != backend->layout.pitch[0])
		return -EOPNOTSUPP;
	ret = backend->bound ? av1_publish_bound(backend) : -EPROTO;
	if (!ret)
		ret = s7d_av1_runner_output_committed(backend->adapter->runner, output);
	if (ret)
		return ret;
	backend->frame_sequence++;
	return av1_unbind(backend);
}

static void av1_work(struct work_struct *work)
{
	struct av1_backend *backend = container_of(work, struct av1_backend, work);
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	u64 token = 0;
	bool cancelled;
	int ret;

	for (;;) {
		mutex_lock(&backend->lock);
		if (backend->closing || backend->failure || backend->capture_blocked)
			goto unlock;
		if (backend->input_pending && !backend->preflight) {
			token = backend->input.token;
			ret = av1_metadata(backend);
			if (ret < 0)
				goto failed;
			backend->preflight = true;
			if (ret) {
				/* Nothing to take: complete it, initialization goes on. */
				if (token == backend->initial_token)
					backend->initial_complete = true;
				mutex_unlock(&backend->lock);
				continue;
			}
			mutex_unlock(&backend->lock);
			cancelled = false;
			ret = backend->sequence_notified ? 0 :
				av1_notify_sequence(backend, &cancelled);
			mutex_lock(&backend->lock);
			if (ret) {
				if (cancelled && backend->closing)
					goto unlock;
				goto failed;
			}
			backend->sequence_notified = true;
			if (token == backend->initial_token)
				backend->initial_complete = true;
			mutex_unlock(&backend->lock);
			continue;
		}
		if (backend->input_pending && backend->preflight && !backend->tu.picture_count) {
			token = backend->input.token;
			backend->input_pending = false;
			backend->preflight = false;
			backend->last_completed = token;
			mutex_unlock(&backend->lock);
			if (av1_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->input_pending && backend->preflight && backend->captures) {
			token = backend->input.token;
			if (!backend->output_pending) {
				u32 offset = backend->tu.picture[backend->next_picture].offset;
				u32 bytes = backend->tu.picture[backend->next_picture].bytes;
				struct s7d_av1_decode_target target = {
					.fbc = backend->fbc,
				};
				u64 picture_token;
				const u8 *data;
				u8 *seeded = NULL;

				if (backend->picture_token == U64_MAX) {
					ret = -EOVERFLOW;
					goto failed;
				}
				/* Any buffer the decoder takes means a full stop to give it back. */
				backend->execution_attempted = true;
				if (!backend->tu.picture[backend->next_picture].show_existing) {
					mutex_unlock(&backend->lock);
					ret = av1_acquire_target(backend);
					mutex_lock(&backend->lock);
					if (ret == -EAGAIN || (!ret && backend->closing))
						goto unlock;
					if (ret)
						goto failed;
				}
				if (backend->pending)
					target.memory = &backend->slot_memory[backend->pending - 1];
				picture_token = ++backend->picture_token;
				data = s7d_codec_dma_cpu(backend->input.memory) + offset;
				if (backend->seed_sequence) {
					ret = av1_seed_sequence(backend, data, bytes, &seeded,
								&bytes);
					if (ret)
						goto failed;
					if (seeded)
						data = seeded;
					backend->seed_sequence = false;
				}
				mutex_lock(&adapter->resources);
				adapter->runner_closed = false;
				mutex_unlock(&adapter->resources);
				mutex_unlock(&backend->lock);
				memset(&backend->result, 0, sizeof(backend->result));
				ret = s7d_av1_runner_decode(adapter->runner, backend->session,
					backend->lease_epoch, picture_token, data, bytes,
					backend->input.timestamp_ns, &target, &backend->result);
				kvfree(seeded);
				if (!ret)
					ret = av1_decoded(backend);
				mutex_lock(&backend->lock);
				backend->output_pending = backend->result.output_ready;
				if (ret) {
					if (ret == -ECANCELED && backend->closing)
						goto unlock;
					goto failed;
				}
				if (backend->closing)
					goto unlock;
				if (!backend->result.input_consumed ||
				    (!backend->result.picture_decoded && !backend->result.show_existing)) {
					ret = -EIO;
					goto failed;
				}
			}
			if (backend->output_pending) {
				mutex_unlock(&backend->lock);
				ret = av1_publish(backend);
				mutex_lock(&backend->lock);
				if (ret == -EAGAIN)
					goto unlock;
				if (ret) {
					if (ret == -ECANCELED && backend->closing)
						goto unlock;
					goto failed;
				}
				backend->output_pending = false;
			}
			backend->next_picture++;
			if (backend->next_picture < backend->tu.picture_count) {
				mutex_unlock(&backend->lock);
				continue;
			}
			backend->input_pending = false;
			backend->preflight = false;
			backend->last_completed = token;
			mutex_unlock(&backend->lock);
			if (av1_complete_input(backend, token, 0))
				return;
			continue;
		}
		if (backend->drain_pending && !backend->drain_notified &&
		    !backend->input_pending && !backend->output_pending) {
			struct s7d_v4l2_drain drain = backend->drain;

			mutex_unlock(&backend->lock);
			mutex_lock(&backend->notifications);
			cancelled = false;
			if (backend->frontend && !READ_ONCE(backend->closing))
				ret = s7d_v4l2_drain_done_result(backend->frontend, drain.epoch,
								 drain.last_input_token, &cancelled);
			else {
				cancelled = true;
				ret = -ECANCELED;
			}
			mutex_unlock(&backend->notifications);
			mutex_lock(&backend->lock);
			if (cancelled && backend->closing)
				goto unlock;
			if (ret) {
				backend->failure = ret;
				mutex_unlock(&backend->lock);
				av1_notify_error(backend, ret);
				return;
			}
			backend->drain_notified = true;
		}
unlock:
		mutex_unlock(&backend->lock);
		return;
failed:
		backend->failure = ret;
		backend->input_pending = false;
		mutex_unlock(&backend->lock);
		av1_complete_input(backend, token, ret);
		return;
	}
}

static int av1_prepare(void *priv, struct s7d_codec_session *session,
		       struct s7d_v4l2_context *frontend)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret = 0;

	if (!session || !frontend)
		return -EINVAL;
	mutex_lock(&adapter->clients);
	mutex_lock(&backend->lock);
	if (adapter->closing || backend->closing || backend->session || adapter->active) {
		ret = -EBUSY;
	} else {
		s7d_codec_session_get(session);
		backend->session = session;
		backend->frontend = frontend;
		mutex_lock(&adapter->resources);
		backend->lease_epoch = adapter->lease_epoch;
		mutex_unlock(&adapter->resources);
		adapter->active = backend;
	}
	mutex_unlock(&backend->lock);
	mutex_unlock(&adapter->clients);
	return ret;
}

static int av1_submit(void *priv, const struct s7d_v4l2_input *input)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!input || !input->memory || !input->bytes || !input->token ||
		 input->bytes > AV1_INPUT_BYTES || input->bytes > s7d_codec_dma_size(input->memory))
		ret = -EINVAL;
	else if (backend->input_pending || backend->output_pending || backend->drain_pending ||
		 input->token <= backend->last_submitted)
		ret = -EBUSY;
	else {
		backend->input = *input;
		memset(&backend->tu, 0, sizeof(backend->tu));
		backend->next_picture = 0;
		backend->last_submitted = input->token;
		if (!backend->initial_token)
			backend->initial_token = input->token;
		backend->input_pending = true;
		backend->preflight = false;
		queue_work(system_unbound_wq, &backend->work);
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_sync_initial(void *priv, u64 token)
{
	struct av1_backend *backend = priv;
	int ret;

	flush_work(&backend->work);
	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!token || token != backend->initial_token || backend->captures)
		ret = -ESTALE;
	else
		ret = backend->initial_complete ? 0 : -EAGAIN;
	mutex_unlock(&backend->lock);
	return ret;
}

/*
 * NV12/NV12M buffers take the NV12 double write of a DPB picture, at the
 * planes they export, at the format's pitch.
 */
static int av1_nv12_prepare(struct av1_backend *backend,
			    const struct s7d_v4l2_capture_buffer *buffers, unsigned int count)
{
	const struct s7d_codec_layout *layout = &backend->layout;
	u32 luma = layout->pitch[0] * layout->storage_height;
	dma_addr_t plane[2];
	size_t bytes[2];
	unsigned int i, j;
	int ret;

	for (i = 0; i < count; i++) {
		for (j = 0; j < 2; j++) {
			ret = s7d_codec_surface_dma(buffers[i].surface, j, &plane[j], &bytes[j]);
			if (ret)
				return ret;
		}
		/* What the hardware writes: pitch * height rows of luma, half of it CbCr. */
		if (bytes[0] < luma || bytes[1] < luma / 2 ||
		    (u64)plane[0] + luma > (1ULL << 32) ||
		    (u64)plane[1] + luma / 2 > (1ULL << 32))
			return -ERANGE;
		backend->slot_memory[i] = (struct s7d_av1_slot_memory) {
			.cookie = i + 1,
			.luma = { .address = plane[0], .bytes = luma },
			.chroma = { .address = plane[1], .bytes = luma / 2 },
		};
	}
	return 0;
}

/*
 * The CAPTURE buffers become the DPB's picture memory. AM08C/AM10C ones take
 * it at the offsets the format exports: FBC header first, its body after it,
 * the NV12 double write behind both; AM08F/AM10F ones have no NV12, so the
 * picture is written compressed only. Anything else fails CAPTURE setup.
 */
static int av1_capture_prepare(struct av1_backend *backend,
			       const struct s7d_v4l2_capture_buffer *buffers,
			       unsigned int count)
{
	const struct s7d_codec_layout *layout = &backend->layout;
	u32 depth = layout->bit_depth == 10 ? 10 : 8;
	struct s7d_amfbc_buffer region[ARRAY_SIZE(backend->capture)];
	struct s7d_av1_reference_layout fbc;
	unsigned int i;
	bool prefixed;
	int ret;

	lockdep_assert_held(&backend->lock);
	backend->bound = false;
	backend->fbc = false;
	if (count > ARRAY_SIZE(region) || count < AV1_CAPTURE_MINIMUM)
		return -EINVAL;
	ret = s7d_v4l2_fbc_capture_pool(layout, buffers, count, region, &prefixed);
	if (ret)
		return ret;
	if (!prefixed) {
		ret = av1_nv12_prepare(backend, buffers, count);
		if (!ret)
			backend->bound = true;
		return ret;
	}
	/* What the hardware writes for a picture of this size must fit the format. */
	ret = s7d_av1_reference_layout(layout->coded_width, layout->coded_height, depth,
				       backend->sequence.use_128x128_superblock ? 128 : 64,
				       &fbc);
	if (ret)
		return ret;
	if (fbc.body_allocation > region[0].body_bytes ||
	    s7d_amfbc_header_used(layout->coded_width, layout->coded_height) >
	    region[0].header_bytes)
		return -ERANGE;
	for (i = 0; i < count; i++) {
		backend->slot_memory[i] = (struct s7d_av1_slot_memory) {
			.cookie = i + 1,
			.header = { .address = region[i].header,
				    .bytes = region[i].header_bytes },
			.body = { .address = region[i].body, .bytes = region[i].body_bytes },
		};
		if (s7d_amfbc_fbc_only(&region[i]))
			continue;
		backend->slot_memory[i].luma = (struct s7d_av1_region) {
			.address = region[i].nv12, .bytes = region[i].nv12_luma_bytes,
		};
		backend->slot_memory[i].chroma = (struct s7d_av1_region) {
			.address = region[i].nv12 + region[i].nv12_luma_bytes,
			.bytes = region[i].nv12_luma_bytes / 2,
		};
	}
	backend->bound = true;
	backend->fbc = true;
	return 0;
}

static int av1_capture_start(void *priv, const struct s7d_codec_layout *layout,
			     u64 generation,
			     const struct s7d_v4l2_capture_buffer *buffers, unsigned int count)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || !backend->session)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!layout || !buffers || !generation || !backend->sequence_valid ||
		 backend->captures || count > ARRAY_SIZE(backend->capture))
		ret = -EINVAL;
	if (ret)
		goto out;
	ret = s7d_codec_validate_layout(layout);
	if (!ret && (memcmp(&backend->layout, layout, sizeof(*layout)) ||
		     count < layout->min_capture_buffers))
		ret = -EINVAL;
	if (!ret)
		ret = s7d_v4l2_validate_capture_pool(layout, generation, buffers, count);
	if (!ret) {
		memset(backend->bind, 0, sizeof(backend->bind));
		memset(backend->slot_memory, 0, sizeof(backend->slot_memory));
		backend->pending = 0;
		ret = av1_capture_prepare(backend, buffers, count);
	}
	if (!ret) {
		memcpy(backend->capture, buffers, sizeof(*buffers) * count);
		backend->captures = count;
		backend->generation = generation;
		backend->capture_blocked = false;
		queue_work(system_unbound_wq, &backend->work);
	}
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_capture_stop_check(struct av1_backend *backend, u64 generation)
{
	if (backend->closing || backend->failure || !backend->session)
		return backend->failure ? backend->failure : -ESHUTDOWN;
	if (!generation || !backend->sequence_valid ||
	    (backend->captures && backend->generation != generation))
		return -ESTALE;
	return backend->execution_attempted || backend->drain_pending ? -EOPNOTSUPP : 0;
}

static int av1_capture_stop(void *priv, u64 generation)
{
	struct av1_backend *backend = priv;
	int ret;

	mutex_lock(&backend->lock);
	ret = av1_capture_stop_check(backend, generation);
	if (!ret)
		backend->capture_blocked = true;
	mutex_unlock(&backend->lock);
	if (ret)
		return ret;
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	mutex_unlock(&backend->notifications);
	mutex_lock(&backend->lock);
	ret = av1_capture_stop_check(backend, generation);
	if (!ret) {
		memset(backend->capture, 0, sizeof(backend->capture));
		memset(backend->bind, 0, sizeof(backend->bind));
		memset(backend->slot_memory, 0, sizeof(backend->slot_memory));
		backend->captures = 0;
		backend->pending = 0;
		backend->bound = false;
		backend->fbc = false;
		backend->generation = 0;
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_capture_queue(void *priv, struct s7d_codec_surface *surface,
			    u64 surface_id, u64 generation)
{
	struct av1_backend *backend = priv;
	u32 i;
	int ret = -ESTALE;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || backend->capture_blocked) {
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
		goto out;
	}
	for (i = 0; i < backend->captures; i++)
		if (backend->capture[i].surface == surface &&
		    backend->capture[i].surface_id == surface_id &&
		    backend->capture[i].generation == generation) {
			queue_work(system_unbound_wq, &backend->work);
			ret = 0;
			break;
		}
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_drain(void *priv, const struct s7d_v4l2_drain *request)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure)
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	else if (!request || !request->epoch || backend->input_pending ||
		 backend->output_pending || backend->drain_pending ||
		 request->last_input_token != backend->last_completed)
		ret = -EBUSY;
	else {
		backend->drain = *request;
		backend->drain_pending = true;
		queue_work(system_unbound_wq, &backend->work);
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static int av1_resume(void *priv)
{
	struct av1_backend *backend = priv;
	int ret = 0;

	flush_work(&backend->work);
	mutex_lock(&backend->lock);
	if (backend->closing || backend->failure || backend->input_pending ||
	    backend->output_pending || !backend->drain_notified)
		ret = backend->failure ? backend->failure : -EBUSY;
	else {
		memset(&backend->drain, 0, sizeof(backend->drain));
		backend->drain_pending = false;
		backend->drain_notified = false;
	}
	mutex_unlock(&backend->lock);
	return ret;
}

static void av1_detach(void *priv)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret = 0;

	mutex_lock(&backend->stop);
	if (backend->detached)
		goto unlock;
	mutex_lock(&backend->lock);
	backend->closing = true;
	if (backend->session)
		s7d_av1_runner_request_stop(adapter->runner);
	mutex_unlock(&backend->lock);
	cancel_work_sync(&backend->work);
	mutex_lock(&backend->notifications);
	backend->frontend = NULL;
	mutex_unlock(&backend->notifications);
	if (backend->session) {
		if (backend->output_pending) {
			ret = s7d_av1_runner_output_committed(adapter->runner, &backend->result.output);
			if (!ret)
				backend->output_pending = false;
		}
		if (!ret)
			ret = s7d_av1_runner_close(adapter->runner, backend->session, backend->lease_epoch);
		mutex_lock(&adapter->resources);
		adapter->stop_error = ret;
		adapter->runner_closed = !ret;
		mutex_unlock(&adapter->resources);
	}
	backend->stop_error = ret;
	backend->detached = true;
unlock:
	mutex_unlock(&backend->stop);
}

static int av1_release_epoch(struct av1_backend *backend)
{
	struct s7d_codec_session *session;
	int ret;

	lockdep_assert_held(&backend->adapter->clients);
	if (!backend->closing)
		return -EBUSY;
	if (backend->stop_error)
		return backend->stop_error;
	if (backend->session) {
		ret = s7d_codec_session_stop(backend->session);
		if (ret)
			return ret;
	}
	mutex_lock(&backend->lock);
	if (backend->adapter->active == backend)
		backend->adapter->active = NULL;
	session = backend->session;
	backend->session = NULL;
	mutex_unlock(&backend->lock);
	if (session)
		s7d_codec_session_put(session);
	return 0;
}

static int av1_reset(void *priv)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret;

	mutex_lock(&adapter->clients);
	ret = adapter->closing ? -ESHUTDOWN : av1_release_epoch(backend);
	if (!ret) {
		mutex_lock(&backend->lock);
		memset(&backend->input, 0, sizeof(backend->input));
		memset(&backend->tu, 0, sizeof(backend->tu));
		memset(&backend->drain, 0, sizeof(backend->drain));
		memset(&backend->result, 0, sizeof(backend->result));
		memset(backend->capture, 0, sizeof(backend->capture));
		/* The stopped session took every CAPTURE binding with it. */
		memset(backend->bind, 0, sizeof(backend->bind));
		memset(backend->slot_memory, 0, sizeof(backend->slot_memory));
		backend->pending = 0;
		backend->bound = false;
		backend->fbc = false;
		backend->lease_epoch = 0;
		backend->generation = 0;
		backend->initial_token = 0;
		backend->last_submitted = 0;
		backend->last_completed = 0;
		backend->picture_token = 0;
		backend->next_picture = 0;
		backend->captures = 0;
		backend->frame_sequence = 0;
		backend->failure = 0;
		backend->stop_error = 0;
		backend->input_pending = false;
		backend->preflight = false;
		backend->initial_complete = false;
		backend->output_pending = false;
		backend->capture_blocked = false;
		backend->execution_attempted = false;
		backend->drain_pending = false;
		backend->drain_notified = false;
		backend->keyed = false;
		backend->seed_sequence = false;
		backend->closing = false;
		backend->detached = false;
		mutex_unlock(&backend->lock);
	}
	mutex_unlock(&adapter->clients);
	return ret;
}

/* An OUTPUT-only seek keeps the sequence header; a new stream forgets it. */
static int av1_clear_stream(void *priv)
{
	struct av1_backend *backend = priv;
	struct s7d_av1_v4l2 *adapter = backend->adapter;
	int ret = 0;

	mutex_lock(&adapter->clients);
	mutex_lock(&backend->lock);
	if (adapter->closing || backend->closing || backend->failure) {
		ret = backend->failure ? backend->failure : -ESHUTDOWN;
	} else if (backend->session || backend->frontend || adapter->active == backend ||
		   backend->generation || backend->captures || backend->input_pending ||
		   backend->execution_attempted || backend->drain_pending ||
		   work_busy(&backend->work)) {
		ret = -EBUSY;
	} else {
		memset(&backend->layout, 0, sizeof(backend->layout));
		memset(&backend->sequence, 0, sizeof(backend->sequence));
		memset(backend->sequence_payload, 0, sizeof(backend->sequence_payload));
		backend->sequence_bytes = 0;
		backend->sequence_valid = false;
		backend->sequence_notified = false;
	}
	mutex_unlock(&backend->lock);
	mutex_unlock(&adapter->clients);
	return ret;
}

static const struct s7d_v4l2_backend_ops av1_ops = {
	.prepare = av1_prepare,
	.submit = av1_submit,
	.sync_initial = av1_sync_initial,
	.capture_start = av1_capture_start,
	.capture_stop = av1_capture_stop,
	.capture_queue = av1_capture_queue,
	.drain = av1_drain,
	.resume = av1_resume,
	.detach = av1_detach,
	.reset = av1_reset,
	.clear_stream = av1_clear_stream,
};

static int av1_factory_ready(void *priv)
{
	struct s7d_av1_v4l2 *adapter = priv;
	int ret;

	mutex_lock(&adapter->resources);
	ret = READ_ONCE(adapter->closing) || adapter->release_error ? -ESHUTDOWN : 0;
	mutex_unlock(&adapter->resources);
	return ret;
}

static void *av1_factory_create(void *priv)
{
	struct s7d_av1_v4l2 *adapter = priv;
	struct av1_backend *backend;

	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&adapter->clients);
	if (adapter->closing || !try_module_get(adapter->owner)) {
		mutex_unlock(&adapter->clients);
		kfree(backend);
		return ERR_PTR(-ESHUTDOWN);
	}
	backend->adapter = adapter;
	mutex_init(&backend->lock);
	mutex_init(&backend->notifications);
	mutex_init(&backend->stop);
	INIT_WORK(&backend->work, av1_work);
	list_add_tail(&backend->node, &adapter->instances);
	mutex_unlock(&adapter->clients);
	return backend;
}

static void av1_factory_destroy(void *priv, void *instance)
{
	struct s7d_av1_v4l2 *adapter = priv;
	struct av1_backend *backend = instance;
	int ret;

	mutex_lock(&adapter->clients);
	av1_detach(backend);
	ret = av1_release_epoch(backend);
	if (ret) {
		dev_err(adapter->dev, "AV1 instance stop failed: %d; owner retained\n", ret);
		mutex_unlock(&adapter->clients);
		return;
	}
	list_del(&backend->node);
	mutex_unlock(&adapter->clients);
	module_put(adapter->owner);
	kfree(backend);
}

struct s7d_av1_v4l2 *
s7d_av1_v4l2_create(struct device *dev, struct s7d_codec_dos *dos,
		      struct s7d_codec_firmware_owner *firmware,
		      struct s7d_hevc_engine *engine, struct module *owner)
{
	struct s7d_av1_v4l2 *adapter;
	int ret;

	if (!dev || !dos || !firmware || !engine)
		return ERR_PTR(-EINVAL);
	adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return ERR_PTR(-ENOMEM);
	adapter->dev = get_device(dev);
	adapter->owner = owner;
	adapter->dos = dos;
	mutex_init(&adapter->clients);
	mutex_init(&adapter->resources);
	INIT_LIST_HEAD(&adapter->instances);
	adapter->runner = s7d_av1_runner_create(dev, dos, firmware, engine, AV1_STAGING_BYTES);
	if (IS_ERR(adapter->runner)) {
		ret = PTR_ERR(adapter->runner);
		goto free_adapter;
	}
	adapter->core = s7d_codec_core_create(dev, owner, &av1_resource_ops, adapter, 32);
	if (IS_ERR(adapter->core)) {
		ret = PTR_ERR(adapter->core);
		s7d_av1_runner_destroy(adapter->runner);
		goto free_adapter;
	}
	adapter->factory = (struct s7d_v4l2_backend_factory) {
		.owner = owner,
		.codec = {
			.engine = S7D_CODEC_ENGINE_HEVC,
			.output_format = V4L2_PIX_FMT_AV1,
			.max_width = AV1_MAX_WIDTH, .max_height = AV1_MAX_HEIGHT,
			.max_input_size = AV1_INPUT_BYTES,
			.min_input_size = AV1_INPUT_BYTES,
			.default_layout = s7d_v4l2_default_nv12_layout,
			.fbc = true,
			.fbc_min_capture_buffers = AV1_CAPTURE_MINIMUM,
		},
		.ops = &av1_ops, .ready = av1_factory_ready,
		.create = av1_factory_create, .destroy = av1_factory_destroy,
	};
	return adapter;

free_adapter:
	put_device(adapter->dev);
	kfree(adapter);
	return ERR_PTR(ret);
}

struct s7d_codec_core *s7d_av1_v4l2_core(struct s7d_av1_v4l2 *adapter)
{
	return adapter->core;
}

const struct s7d_v4l2_backend_factory *
s7d_av1_v4l2_factory(struct s7d_av1_v4l2 *adapter)
{
	return &adapter->factory;
}

int s7d_av1_v4l2_suspend_check(struct s7d_av1_v4l2 *adapter)
{
	int ret;

	mutex_lock(&adapter->clients);
	mutex_lock(&adapter->resources);
	ret = adapter->active || adapter->lease_active || adapter->release_error ? -EBUSY : 0;
	mutex_unlock(&adapter->resources);
	if (!ret)
		ret = s7d_codec_dos_suspend_check(adapter->dos);
	mutex_unlock(&adapter->clients);
	return ret;
}

int s7d_av1_v4l2_shutdown(struct s7d_av1_v4l2 *adapter)
{
	struct av1_backend *backend;
	int ret = 0;

	mutex_lock(&adapter->clients);
	adapter->closing = true;
	list_for_each_entry(backend, &adapter->instances, node) {
		av1_detach(backend);
		if (!ret)
			ret = backend->stop_error;
	}
	mutex_unlock(&adapter->clients);
	return ret;
}

int s7d_av1_v4l2_destroy(struct s7d_av1_v4l2 *adapter)
{
	int ret;

	if (!adapter)
		return -EINVAL;
	mutex_lock(&adapter->clients);
	adapter->closing = true;
	if (!list_empty(&adapter->instances)) {
		ret = -EBUSY;
		goto unlock;
	}
	mutex_lock(&adapter->resources);
	ret = adapter->lease_active || adapter->release_error ? -EBUSY : 0;
	mutex_unlock(&adapter->resources);
	if (ret)
		goto unlock;
	if (adapter->core) {
		ret = s7d_codec_core_destroy(adapter->core);
		if (ret)
			goto unlock;
		adapter->core = NULL;
	}
	ret = s7d_av1_runner_destroy(adapter->runner);
unlock:
	mutex_unlock(&adapter->clients);
	if (ret)
		return ret;
	put_device(adapter->dev);
	kfree(adapter);
	return 0;
}
