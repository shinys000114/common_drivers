// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "s7d-afbc-engine.h"

#define AFBC_RAW			0x3a01
#define AFBC_CLEAR		0x3a02
#define AFBC_MASK		0x3a03
#define AFBC_COMMAND		0x3a05
#define AFBC_STATUS		0x3a06
#define AFBC_SURFACES		0x3a07
#define AFBC_TOP			0x1a0f
#define AFBC_ENCP_INFO		0x271d
#define AFBC_READOUT		BIT(0)
#define AFBC_SWAPPED		BIT(1)
#define AFBC_RAW_ERROR		GENMASK(5, 2)
#define AFBC_ACTIVE		BIT(0)
#define AFBC_SWAPPING		BIT(1)
#define AFBC_ERROR_STATUS	BIT(2)
#define AFBC_TOP_ACTIVE		BIT(31)
#define AFBC_MANUAL_RESET		BIT(23)
#define AFBC_PAYLOAD_LIMIT	BIT(19)
#define ENCP_EN			0x1b80
#define READ0_STATUS		0x27ad
#define OSD_ACTUAL_ENABLE	BIT(21)
#define OSD_FIFO_BUSY		GENMASK(21, 20)
#define AFBC_DIRECT		BIT(0)

void s7d_afbc_engine_init(struct s7d_afbc_engine *e,
			  const struct s7d_afbc_engine_io *io, void *data)
{
	memset(e, 0, sizeof(*e));
	e->io = io;
	e->data = data;
	e->phase = S7D_AFBC_STOPPED;
}

int s7d_afbc_engine_fail(struct s7d_afbc_engine *e, int error, u32 reg)
{
	if (e->phase != S7D_AFBC_ERROR) {
		e->last_error = error < 0 ? error : -EIO;
		e->failed_reg = reg;
		e->failed_generation = e->phase == S7D_AFBC_RUNNING ||
			(e->phase == S7D_AFBC_STOPPING && !e->stop_unstarted) ?
			e->generation : e->pending_generation;
		e->failed_epoch = e->epoch;
		e->failed_phase = e->phase;
		e->failed_readback = false;
		e->failed_expected = 0;
		e->failed_observed = 0;
		e->phase = S7D_AFBC_ERROR;
	}
	return e->last_error;
}

static int engine_read(struct s7d_afbc_engine *e, u32 reg, u32 *value)
{
	int ret = e->io->read(e->data, reg, value);

	return ret ? s7d_afbc_engine_fail(e, ret, reg) : 0;
}

static int engine_write(struct s7d_afbc_engine *e, u32 reg, u32 value)
{
	int ret = e->io->write(e->data, reg, value);

	return ret ? s7d_afbc_engine_fail(e, ret, reg) : 0;
}

int s7d_afbc_engine_read(struct s7d_afbc_engine *e,
			  struct s7d_afbc_observation *out)
{
	int ret;

	if (e->phase == S7D_AFBC_ERROR)
		return e->last_error;
	ret = engine_read(e, AFBC_RAW, &out->raw);
	if (!ret)
		ret = engine_read(e, AFBC_STATUS, &out->status);
	if (!ret)
		ret = engine_read(e, AFBC_TOP, &out->top);
	if (!ret)
		ret = engine_read(e, AFBC_SURFACES, &out->surfaces);
	if (!ret)
		e->observed = *out;
	return ret;
}

static const struct s7d_afbc_surface *engine_surface(const struct s7d_afbc_engine *e)
{
	if (e->phase == S7D_AFBC_PREPARED ||
	    (e->phase == S7D_AFBC_STOPPING && e->stop_unstarted))
		return e->pending_valid ?
			s7d_afbc_surface_get(e->pending.surface_mask) : NULL;
	return e->bound_valid ? s7d_afbc_surface_get(e->bound.surface_mask) : NULL;
}

static int observation_check(struct s7d_afbc_engine *e,
			     const struct s7d_afbc_observation *o)
{
	const struct s7d_afbc_surface *s = engine_surface(e);

	e->observed = *o;
	if (o->raw & AFBC_RAW_ERROR)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_RAW);
	if (o->status & AFBC_ERROR_STATUS)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_STATUS);
	if (!(o->top & AFBC_MANUAL_RESET))
		return s7d_afbc_engine_fail(e, -EIO, AFBC_TOP);
	if (!s || o->surfaces != s->mask)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_SURFACES);
	return 0;
}

static bool inactive(const struct s7d_afbc_observation *o)
{
	return !(o->status & (AFBC_ACTIVE | AFBC_SWAPPING)) &&
	       !(o->top & AFBC_TOP_ACTIVE);
}

bool s7d_afbc_same_layout(const struct s7d_afbc_state *a,
			 const struct s7d_afbc_state *b)
{
	unsigned int i;

	if (memcmp(&a->osd, &b->osd, sizeof(a->osd)) ||
	    memcmp(&a->route, &b->route, sizeof(a->route)) ||
	    memcmp(&a->unpack, &b->unpack, sizeof(a->unpack)) ||
	    a->surface_mask != b->surface_mask ||
	    a->afbc_size != b->afbc_size ||
	    a->header_size != b->header_size || a->body_size != b->body_size ||
	    a->aligned_width != b->aligned_width ||
	    a->aligned_height != b->aligned_height ||
	    a->buffer_height != b->buffer_height)
		return false;
	for (i = 2; i < S7D_AFBC_SURFACE_REG_COUNT; i++)
		if (a->regs[i].reg != b->regs[i].reg ||
		    a->regs[i].value != b->regs[i].value)
			return false;
	return true;
}

static int program_surface(struct s7d_afbc_engine *e,
			   const struct s7d_afbc_state *p)
{
	unsigned int i;
	u32 surfaces, reg, value;
	int ret;

	for (i = 0; i < S7D_AFBC_SURFACE_REG_COUNT; i++) {
		reg = p->regs[i].reg;
		ret = engine_write(e, reg,
				   p->regs[i].value);
		if (!ret)
			ret = engine_read(e, reg, &value);
		if (ret)
			return ret;
		if (value != p->regs[i].value) {
			ret = s7d_afbc_engine_fail(e, -EIO, reg);
			e->failed_expected = p->regs[i].value;
			e->failed_observed = value;
			e->failed_readback = true;
			return ret;
		}
	}
	ret = engine_write(e, AFBC_SURFACES, p->surface_mask);
	if (!ret)
		ret = engine_read(e, AFBC_SURFACES, &surfaces);
	if (ret)
		return ret;
	if (surfaces != p->surface_mask)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_SURFACES);
	return 0;
}

int s7d_afbc_engine_prepare(struct s7d_afbc_engine *e,
			     const struct s7d_afbc_state *plan, u64 generation)
{
	struct s7d_afbc_observation o;
	struct s7d_afbc_state copy = *plan;
	u32 surfaces, format, top, unpack;
	int ret;

	if (e->phase != S7D_AFBC_STOPPED)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EBUSY;
	if (!generation || generation <= e->accepted_generation)
		return -EINVAL;
	ret = s7d_afbc_check_state(&copy);
	if (ret)
		return ret;
	e->pending = copy;
	e->pending_generation = generation;
	e->accepted_generation = generation;
	e->pending_valid = true;
	ret = s7d_afbc_engine_read(e, &o);
	if (ret)
		return ret;
	if (o.raw & AFBC_RAW_ERROR)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_RAW);
	if (o.status & AFBC_ERROR_STATUS)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_STATUS);
	if (!inactive(&o))
		return s7d_afbc_engine_fail(e, -EBUSY, AFBC_STATUS);
	ret = engine_read(e, AFBC_SURFACES, &surfaces);
	if (ret)
		return ret;
	if (surfaces)
		return s7d_afbc_engine_fail(e, -EBUSY, AFBC_SURFACES);
	ret = engine_read(e, copy.regs[2].reg, &format);
	if (ret)
		return ret;
	if (format & AFBC_PAYLOAD_LIMIT)
		return s7d_afbc_engine_fail(e, -EOPNOTSUPP, copy.regs[2].reg);
	ret = engine_read(e, copy.unpack.reg, &unpack);
	if (ret)
		return ret;
	if (unpack & (BIT(16) | BIT(28)))
		return s7d_afbc_engine_fail(e, -EOPNOTSUPP, copy.unpack.reg);
	ret = engine_write(e, AFBC_TOP, o.top | AFBC_MANUAL_RESET);
	if (!ret)
		ret = engine_read(e, AFBC_TOP, &top);
	if (ret)
		return ret;
	if ((top ^ o.top) & ~AFBC_MANUAL_RESET ||
	    !(top & AFBC_MANUAL_RESET))
		return s7d_afbc_engine_fail(e, -EIO, AFBC_TOP);
	ret = engine_write(e, AFBC_MASK, 0);
	if (!ret)
		ret = engine_read(e, AFBC_MASK, &surfaces);
	if (!ret && surfaces)
		ret = s7d_afbc_engine_fail(e, -EIO, AFBC_MASK);
	if (!ret)
		ret = program_surface(e, &e->pending);
	if (ret)
		return ret;
	e->stop_decode_done = false;
	e->stop_unstarted = false;
	e->phase = S7D_AFBC_PREPARED;
	return 0;
}

static int clear_previous(struct s7d_afbc_engine *e)
{
	u32 raw, normal;
	int ret;

	ret = engine_read(e, AFBC_RAW, &raw);
	if (ret)
		return ret;
	e->observed.raw = raw;
	if (raw & AFBC_RAW_ERROR)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_RAW);
	normal = raw & (AFBC_SWAPPED | AFBC_READOUT);
	if (normal)
		ret = engine_write(e, AFBC_CLEAR, normal);
	if (!ret)
		ret = engine_read(e, AFBC_RAW, &raw);
	if (ret)
		return ret;
	e->observed.raw = raw;
	if (raw & (AFBC_RAW_ERROR | AFBC_SWAPPED | AFBC_READOUT))
		return s7d_afbc_engine_fail(e, -EIO, AFBC_RAW);
	return 0;
}

static int issue(struct s7d_afbc_engine *e,
		 const struct s7d_afbc_frame *frame, bool take_pending)
{
	int ret;

	if (e->epoch == ~(u64)0)
		return s7d_afbc_engine_fail(e, -EOVERFLOW, AFBC_COMMAND);
	ret = clear_previous(e);
	if (!ret && frame) {
		ret = e->io->check_start(e->data, frame->sequence, frame->field);
		if (ret)
			return s7d_afbc_engine_fail(e, ret, AFBC_ENCP_INFO);
	}
	if (!ret)
		ret = engine_write(e, AFBC_COMMAND, AFBC_DIRECT);
	if (ret)
		return ret;
	if (take_pending) {
		e->bound = e->pending;
		e->generation = e->pending_generation;
		e->bound_valid = true;
		e->pending_valid = false;
		e->pending_ready = false;
		e->started_valid = frame != NULL;
		if (frame) {
			e->started_sequence = frame->sequence;
			e->started_field = frame->field;
		}
	}
	e->epoch++;
	e->sampled_valid = false;
	e->swapped = false;
	e->readout = false;
	if (frame) {
		e->last_start_sequence = frame->sequence;
		e->last_start_valid = true;
	}
	e->phase = S7D_AFBC_RUNNING;
	return 0;
}

int s7d_afbc_engine_start_initial(struct s7d_afbc_engine *e)
{
	struct s7d_afbc_observation o;
	const struct s7d_afbc_surface *s;
	u32 value;
	int ret;

	if (e->phase != S7D_AFBC_PREPARED)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	ret = s7d_afbc_engine_read(e, &o);
	if (!ret)
		ret = observation_check(e, &o);
	if (ret)
		return ret;
	if (!inactive(&o))
		return s7d_afbc_engine_fail(e, -EBUSY, AFBC_STATUS);
	s = engine_surface(e);
	ret = engine_read(e, ENCP_EN, &value);
	if (ret)
		return ret;
	if (!(value & BIT(0)))
		return s7d_afbc_engine_fail(e, -EHOSTDOWN, ENCP_EN);
	ret = engine_read(e, s->ctrl, &value);
	if (ret)
		return ret;
	if (value & (BIT(0) | OSD_ACTUAL_ENABLE))
		return s7d_afbc_engine_fail(e, -EBUSY, s->ctrl);
	return issue(e, NULL, true);
}

int s7d_afbc_engine_stage(struct s7d_afbc_engine *e,
			   const struct s7d_afbc_state *plan, u64 generation)
{
	struct s7d_afbc_state copy = *plan;
	int ret;

	if (e->phase != S7D_AFBC_RUNNING || e->pending_valid)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EBUSY;
	if (!generation || generation <= e->accepted_generation)
		return -EINVAL;
	ret = s7d_afbc_check_state(&copy);
	if (ret)
		return ret;
	if (!s7d_afbc_same_layout(&copy, &e->bound))
		return -EOPNOTSUPP;
	e->pending = copy;
	e->pending_generation = generation;
	e->accepted_generation = generation;
	e->pending_valid = true;
	e->pending_ready = false;
	return 0;
}

int s7d_afbc_engine_rdma_drained(struct s7d_afbc_engine *e, u64 generation)
{
	if (e->phase != S7D_AFBC_RUNNING)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	if (!e->pending_valid || generation != e->pending_generation)
		return -EINVAL;
	e->pending_ready = true;
	return 0;
}

int s7d_afbc_engine_cancel_stage(struct s7d_afbc_engine *e, u64 generation)
{
	if (e->phase != S7D_AFBC_RUNNING)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	if (!e->pending_valid || e->pending_ready ||
	    generation != e->pending_generation)
		return -EINVAL;
	e->pending_valid = false;
	return 0;
}

int s7d_afbc_engine_sample(struct s7d_afbc_engine *e,
			    const struct s7d_afbc_frame *frame,
			    const struct s7d_afbc_observation *observation)
{
	int ret;

	if (e->phase != S7D_AFBC_RUNNING)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	ret = observation_check(e, observation);
	if (ret)
		return ret;
	if ((e->started_valid && frame->sequence < e->started_sequence) ||
	    (e->last_start_valid && frame->sequence < e->last_start_sequence) ||
	    (e->sampled_valid && frame->sequence < e->sampled.sequence))
		return s7d_afbc_engine_fail(e, -ERANGE, 0);
	e->observed = *observation;
	e->sampled = *frame;
	e->sampled_epoch = e->epoch;
	e->sampled_valid = true;
	e->swapped |= !!(observation->raw & AFBC_SWAPPED);
	e->readout |= !!(observation->raw & AFBC_READOUT);
	if (e->swapped && e->readout && inactive(observation) &&
	    frame->idle && frame->vblank)
		e->completed_epoch = e->epoch;
	if (!e->started_valid) {
		if (frame->vblank) {
			e->started_sequence = frame->sequence;
			e->started_field = frame->field;
			e->started_valid = true;
		}
		return 0;
	}
	if (!e->swapped || !e->readout || !inactive(observation) ||
	    !frame->idle || !frame->vblank ||
	    frame->sequence == e->started_sequence ||
	    frame->field == e->started_field)
		return 0;
	if (e->generation == e->completed_generation)
		return 0;
	e->completed_generation = e->generation;
	e->completed_generation_epoch = e->epoch;
	return 1;
}

int s7d_afbc_engine_restart(struct s7d_afbc_engine *e,
			     const struct s7d_afbc_frame *frame)
{
	struct s7d_afbc_observation o;
	unsigned int i;
	u32 value;
	bool take_pending;
	int ret;

	if (e->phase != S7D_AFBC_RUNNING)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	if (e->last_start_valid && frame->sequence == e->last_start_sequence)
		return -EALREADY;
	if (e->completed_generation != e->generation ||
	    !frame->vblank || !frame->idle || !frame->window ||
	    !e->sampled_valid || e->sampled_epoch != e->epoch ||
	    e->sampled.sequence != frame->sequence ||
	    e->sampled.field != frame->field ||
	    e->sampled.vblank != frame->vblank ||
	    e->sampled.idle != frame->idle ||
	    e->sampled.window != frame->window ||
	    e->completed_epoch != e->epoch || !inactive(&e->observed))
		return -EAGAIN;
	if (e->epoch == ~(u64)0)
		return s7d_afbc_engine_fail(e, -EOVERFLOW, AFBC_COMMAND);
	if (!e->io->check_start)
		return -EOPNOTSUPP;
	ret = s7d_afbc_engine_read(e, &o);
	if (!ret)
		ret = observation_check(e, &o);
	if (ret)
		return ret;
	if (!inactive(&o) || (o.raw & (AFBC_READOUT | AFBC_SWAPPED)) !=
	    (AFBC_READOUT | AFBC_SWAPPED))
		return s7d_afbc_engine_fail(e, -EIO, AFBC_STATUS);
	take_pending = e->pending_valid && e->pending_ready;
	if (take_pending) {
		for (i = 0; i < 2; i++) {
			ret = engine_write(e, e->pending.regs[i].reg,
					   e->pending.regs[i].value);
			if (!ret)
				ret = engine_read(e, e->pending.regs[i].reg, &value);
			if (ret)
				return ret;
			if (value != e->pending.regs[i].value)
				return s7d_afbc_engine_fail(e, -EIO, e->pending.regs[i].reg);
		}
	}
	return issue(e, frame, take_pending);
}

int s7d_afbc_engine_stop_begin(struct s7d_afbc_engine *e)
{
	if (e->phase == S7D_AFBC_STOPPED || e->phase == S7D_AFBC_STOPPING)
		return 0;
	if (e->phase == S7D_AFBC_ERROR)
		return e->last_error;
	e->stop_unstarted = e->phase == S7D_AFBC_PREPARED;
	e->stop_decode_done = false;
	e->phase = S7D_AFBC_STOPPING;
	return 0;
}

int s7d_afbc_engine_stop_sample(struct s7d_afbc_engine *e,
				 const struct s7d_afbc_observation *o)
{
	int ret;

	if (e->phase == S7D_AFBC_STOPPED)
		return 1;
	if (e->phase != S7D_AFBC_STOPPING)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	ret = observation_check(e, o);
	if (ret)
		return ret;
	if (!inactive(o))
		return 0;
	if (!e->stop_unstarted &&
	    (o->raw & (AFBC_SWAPPED | AFBC_READOUT)) != (AFBC_SWAPPED | AFBC_READOUT))
		return 0;
	e->stop_decode_done = true;
	return 1;
}

int s7d_afbc_engine_stop_finish(struct s7d_afbc_engine *e)
{
	struct s7d_afbc_observation o;
	const struct s7d_afbc_surface *s;
	u32 value;
	int ret;

	if (e->phase == S7D_AFBC_STOPPED)
		return 0;
	if (e->phase != S7D_AFBC_STOPPING)
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	if (!e->stop_decode_done)
		return -EAGAIN;
	ret = s7d_afbc_engine_read(e, &o);
	if (!ret)
		ret = s7d_afbc_engine_stop_sample(e, &o);
	if (ret <= 0)
		return ret ? ret : -EAGAIN;
	s = engine_surface(e);
	ret = engine_read(e, s->ctrl, &value);
	if (ret)
		return ret;
	if (value & (BIT(0) | OSD_ACTUAL_ENABLE))
		return -EAGAIN;
	ret = engine_read(e, s->fifo, &value);
	if (ret)
		return ret;
	if (value & OSD_FIFO_BUSY)
		return -EAGAIN;
	ret = engine_read(e, READ0_STATUS, &value);
	if (ret)
		return ret;
	if (!(value & BIT(0)))
		return -EAGAIN;
	ret = engine_write(e, AFBC_SURFACES, 0);
	if (!ret)
		ret = engine_read(e, AFBC_SURFACES, &value);
	if (ret)
		return ret;
	if (value)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_SURFACES);
	e->phase = S7D_AFBC_STOPPED;
	return 0;
}

bool s7d_afbc_engine_can_abort_prepare(const struct s7d_afbc_engine *e)
{
	unsigned int i;

	if (e->phase != S7D_AFBC_ERROR ||
	    e->failed_phase != S7D_AFBC_STOPPED || e->bound_valid ||
	    !e->failed_readback || e->epoch || e->failed_epoch || !e->pending_valid ||
	    !e->pending_generation ||
	    e->failed_generation != e->pending_generation ||
	    e->accepted_generation != e->pending_generation ||
	    s7d_afbc_check_state(&e->pending))
		return false;
	for (i = 0; i < S7D_AFBC_SURFACE_REG_COUNT; i++)
		if (e->failed_reg == e->pending.regs[i].reg)
			return true;
	return false;
}

int s7d_afbc_engine_abort_prepare(struct s7d_afbc_engine *e, u32 *failed_reg)
{
	struct s7d_afbc_observation o;
	const struct s7d_afbc_surface *s;
	u32 value;
	int ret;

	if (!s7d_afbc_engine_can_abort_prepare(e))
		return e->phase == S7D_AFBC_ERROR ? e->last_error : -EINVAL;
	s = s7d_afbc_surface_get(e->pending.surface_mask);
	*failed_reg = AFBC_RAW;
	ret = e->io->read(e->data, AFBC_RAW, &o.raw);
	if (ret)
		return ret;
	if (o.raw & AFBC_RAW_ERROR)
		return -EIO;
	*failed_reg = AFBC_STATUS;
	ret = e->io->read(e->data, AFBC_STATUS, &o.status);
	if (ret)
		return ret;
	if (o.status & AFBC_ERROR_STATUS)
		return -EIO;
	if (o.status & (AFBC_ACTIVE | AFBC_SWAPPING))
		return -EBUSY;
	*failed_reg = AFBC_TOP;
	ret = e->io->read(e->data, AFBC_TOP, &o.top);
	if (ret)
		return ret;
	if ((o.top & AFBC_TOP_ACTIVE) || !(o.top & AFBC_MANUAL_RESET))
		return -EBUSY;
	*failed_reg = AFBC_SURFACES;
	ret = e->io->read(e->data, AFBC_SURFACES, &o.surfaces);
	if (ret)
		return ret;
	if (o.surfaces)
		return -EBUSY;
	*failed_reg = ENCP_EN;
	ret = e->io->read(e->data, ENCP_EN, &value);
	if (ret)
		return ret;
	if (value & BIT(0))
		return -EBUSY;
	*failed_reg = s->ctrl;
	ret = e->io->read(e->data, s->ctrl, &value);
	if (ret)
		return ret;
	if (value & (BIT(0) | OSD_ACTUAL_ENABLE))
		return -EBUSY;
	*failed_reg = s->fifo;
	ret = e->io->read(e->data, s->fifo, &value);
	if (ret)
		return ret;
	if (value & OSD_FIFO_BUSY)
		return -EBUSY;
	*failed_reg = READ0_STATUS;
	ret = e->io->read(e->data, READ0_STATUS, &value);
	if (ret)
		return ret;
	if (!(value & BIT(0)))
		return -EBUSY;
	*failed_reg = 0;
	e->pending_valid = false;
	e->pending_ready = false;
	e->phase = S7D_AFBC_STOPPED;
	return 0;
}
