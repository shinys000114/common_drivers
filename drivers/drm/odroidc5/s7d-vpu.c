// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/amlogic/clk/s7d-hdmi-pll-rate.h>
#include <linux/amlogic/s7d-vpu-reset.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_vblank.h>

#include <dt-bindings/reset/amlogic,s7d-vpu-reset.h>

#include "s7d-vpu.h"
#include "s7d-vpp.h"
#include "s7d-plane.h"

#define CREATE_TRACE_POINTS
#include "s7d-trace.h"

#define ENCI_EN			0x1b57
#define ENCP_EN			0x1b80
#define ENCL_EN			0x1ca0
#define OSD1_CTRL		0x1a10
#define OSD2_CTRL		0x1a30
#define OSD_FIFO_OFFSET		0x1b
#define OSD_FIFO_STATE		GENMASK(21, 20)
#define OSD_PATH		0x1a0e
#define OSD_SCALE_CTRL		0x1dff
#define VD1_BLEND		0x1dfb
#define VD2_BLEND		0x1dfc
#define VD1_PATH		0x1a0a
#define VD1_GEN			0x4800
#define VD2_GEN			0x4880
#define VD2_PATH		0x1a0b
#define VD1_BUSY		BIT(17)
#define VD1_FREE_CLK		BIT(31)
#define VENC_MUX		0x271a
#define ENCP_INFO_READ		0x271d
#define HDMI_SETTING		0x271b
#define HDMI_FMT		0x2743
#define VENC_CLK		0x2785
#define ASYNC_STAT		0x27ad
#define RDARB_MODE_L2		0x279d
#define HDMI_DITH		0x27fc
/* S7D ARB_RD01_WR01: VPP_ARB0/1 and display RDMA use READ0; READ1 is DI. */
#define ASYNC_IDLE		BIT(0)
#define OSD_ACTUAL_EN		BIT(21)
#define OSD_CFG_SYNC		BIT(31)
#define OSD_FREE_CLK		BIT(30)
/* ENCP is clocks[3]; the other clocks retain the VPU and its APB/IRQ paths. */
#define PIXEL_CLK		3
#define PIXEL_PLL		0
#define PIXEL_SOURCE		1
#define PIXEL_MUX		2
#define PIXEL_PARENT		3
#define CLOCK_ERROR_PPM		1000
#define AFBC_TOP			0x1a0f
#define AFBC_RAW			0x3a01
#define AFBC_COMMAND		0x3a05
#define AFBC_STATUS		0x3a06
#define AFBC_SURFACES		0x3a07

static u32 vpu_read(struct s7d_vpu *v, u32 reg)
{
	return readl(v->regs + reg * 4);
}

static int vpu_update(struct s7d_vpu *v, u32 reg, u32 mask, u32 value)
{
	writel((vpu_read(v, reg) & ~mask) | value, v->regs + reg * 4);
	if ((vpu_read(v, reg) & mask) != value) {
		v->failed_reg = reg;
		return -EIO;
	}
	return 0;
}

static int s7d_vpu_reset_access(void *data)
{
	struct s7d_vpu *v = data;

	return v->acquired ? 0 : -EHOSTDOWN;
}

static int s7d_vpu_afbc_read(void *data, u32 reg, u32 *value)
{
	struct s7d_vpu *v = data;
	int ret = s7d_vpu_reset_access(v);

	return ret ? ret : regmap_read(v->regmap, reg * 4, value);
}

static int s7d_vpu_afbc_write(void *data, u32 reg, u32 value)
{
	struct s7d_vpu *v = data;
	int ret = s7d_vpu_reset_access(v);

	if (ret)
		return ret;
	if (reg == AFBC_COMMAND)
		v->afbc_command_counter = vpu_read(v, ENCP_INFO_READ);
	return regmap_write(v->regmap, reg * 4, value);
}

static bool s7d_vpu_afbc_window(u32 before, u32 after, u32 vd1, u32 vd2,
			       u16 start, u16 end)
{
	if (end <= start || (before >> 29) != (after >> 29) ||
	    ((before >> 16) & 0x1fff) < start ||
	    ((before >> 16) & 0x1fff) > ((after >> 16) & 0x1fff) ||
	    ((after >> 16) & 0x1fff) >= end)
		return false;
	if ((vd1 & BIT(0)) && (!(vd1 & BIT(29)) ||
	    ((vd1 >> 19) & 0x3f) < end - start + 2U))
		return false;
	return !(vd2 & BIT(0)) || ((vd2 & BIT(29)) &&
		((vd2 >> 19) & 0x3f) >= end - start + 2U);
}

static int s7d_vpu_afbc_check_start(void *data, u64 sequence, u8 field)
{
	struct s7d_vpu *v = data;
	u32 before, after, fifo, fifo2, vd1, vd2, arbiter;

	lockdep_assert_held(&v->frame_lock);
	if (!v->acquired || !(vpu_read(v, ENCP_EN) & BIT(0)) ||
	    sequence != READ_ONCE(v->scanout.vblank_seq))
		return -EHOSTDOWN;
	before = vpu_read(v, ENCP_INFO_READ);
	fifo = vpu_read(v, OSD1_CTRL + OSD_FIFO_OFFSET);
	fifo2 = vpu_read(v, OSD2_CTRL + OSD_FIFO_OFFSET);
	vd1 = vpu_read(v, VD1_GEN);
	vd2 = vpu_read(v, VD2_GEN);
	arbiter = vpu_read(v, ASYNC_STAT);
	after = vpu_read(v, ENCP_INFO_READ);
	if ((before >> 29) != field || (after >> 29) != field ||
	    ((fifo | fifo2) & OSD_FIFO_STATE) || ((vd1 | vd2) & VD1_BUSY) ||
	    !(arbiter & ASYNC_IDLE) ||
	    !s7d_vpu_afbc_window(before, after, vd1, vd2, v->flip_start, v->flip_end))
		return -ETIMEDOUT;
	return 0;
}

static int s7d_vpu_afbc_reset_idle(void *data);

static unsigned long s7d_vpu_afbc_replay_timeout(struct s7d_vpu *v)
{
	return max_t(unsigned long, msecs_to_jiffies(500),
		     nsecs_to_jiffies64(v->afbc_replay_frame_ns * 4));
}

static const struct s7d_afbc_engine_io s7d_vpu_afbc_io = {
	.read = s7d_vpu_afbc_read,
	.write = s7d_vpu_afbc_write,
	.check_start = s7d_vpu_afbc_check_start,
	.reset_idle = s7d_vpu_afbc_reset_idle,
};

static void s7d_vpu_afbc_timeout(struct work_struct *work)
{
	struct s7d_vpu *v = container_of(to_delayed_work(work),
				       struct s7d_vpu, afbc_timeout);
	unsigned long flags, now;
	int ret = 0;

	spin_lock_irqsave(&v->frame_lock, flags);
	now = jiffies;
	if (v->afbc_enabled && v->afbc.phase == S7D_AFBC_RUNNING) {
		if (time_before(now, v->afbc_deadline))
			mod_delayed_work(system_wq, &v->afbc_timeout,
					 v->afbc_deadline - now);
		else if (v->afbc_replay)
			ret = -ETIMEDOUT;
		else
			ret = s7d_afbc_engine_fail(&v->afbc, -ETIMEDOUT,
				v->afbc.completed_epoch == v->afbc.epoch ?
				ENCP_INFO_READ : AFBC_STATUS);
	}
	if (ret)
		s7d_crtc_link_error(v->crtc, ret);
	spin_unlock_irqrestore(&v->frame_lock, flags);
}

static void s7d_vpu_afbc_arm_timeout(struct s7d_vpu *v)
{
	lockdep_assert_held(&v->frame_lock);
	v->afbc_deadline = jiffies + msecs_to_jiffies(500);
	mod_delayed_work(system_wq, &v->afbc_timeout, msecs_to_jiffies(500));
}

static int s7d_vpu_afbc_resets(struct s7d_vpu *v)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(v->local_resets); i++) {
		ret = reset_control_status(v->local_resets[i].rstc);
		if (ret)
			return ret < 0 ? ret : -EHOSTDOWN;
	}
	return 0;
}

static bool s7d_vpu_afbc_fault_owned(struct s7d_vpu *v)
{
	struct s7d_scanout *s = &v->scanout;
	unsigned long flags;
	unsigned int i, slot = v->afbc.bound.surface_mask == BIT(0) ?
		S7D_PLANE_PRIMARY : S7D_PLANE_RGB;
	bool owned;

	spin_lock_irqsave(&s->lock, flags);
	owned = s->fault && s->phase == S7D_SCANOUT_IDLE &&
		v->afbc_surface_mask == v->afbc.bound.surface_mask &&
		v->afbc_unpack_mask == (v->afbc.bound.unpack.mask & ~GENMASK(15, 0)) &&
		s->active.afbc.fb && s->active.fb[slot] == s->active.afbc.fb &&
		s->active.afbc.generation == v->afbc.generation &&
		s->active.generation >= s->active.afbc.generation &&
		s->generation >= s->active.generation &&
		!s->pending_afbc_epoch &&
		!s7d_afbc_check_state(&s->active.afbc.plan) &&
		s7d_afbc_same_layout(&s->active.afbc.plan, &v->afbc.bound) &&
		s->active.afbc.plan.header_addr == v->afbc.bound.header_addr;
	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (s->pending.fb[i] || s->retired.fb[i])
			owned = false;
	spin_unlock_irqrestore(&s->lock, flags);
	return owned;
}

static int s7d_vpu_afbc_reset_check(struct s7d_vpu *v, u32 reg, u32 mask, u32 expected)
{
	u32 value;
	int ret;

	v->failed_reg = reg;
	ret = s7d_vpu_afbc_read(v, reg, &value);
	return ret ? ret : (value & mask) == expected ? 0 : -EBUSY;
}

static int s7d_vpu_afbc_reset_idle(void *data)
{
	struct s7d_vpu *v = data;
	struct reset_control *reset = v->local_resets[S7D_VPU_RESET_AFBCD_LOGIC].rstc;
	static const u32 osds[] = { OSD1_CTRL, OSD2_CTRL };
	static const u32 videos[] = { VD1_GEN, VD2_GEN };
	unsigned int i;
	int ret;

	if (!v->acquired || !s7d_vpu_afbc_fault_owned(v))
		return -EHOSTDOWN;
	v->failed_reg = 0;
	ret = reset_control_status(v->rdma.reset);
	if (ret != 1)
		return ret < 0 ? ret : -EBUSY;
	v->failed_reg = 0x1a01;
	ret = s7d_vpu_afbc_resets(v);
	if (ret)
		return ret;
	ret = s7d_vpu_afbc_reset_check(v, RDARB_MODE_L2, GENMASK(18, 16), 0);
	for (i = 0; !ret && i < ARRAY_SIZE(videos); i++)
		ret = s7d_vpu_afbc_reset_check(v, videos[i], VD1_BUSY | BIT(0), 0);
	for (i = 0; !ret && i < ARRAY_SIZE(osds); i++) {
		ret = s7d_vpu_afbc_reset_check(v, osds[i], OSD_ACTUAL_EN | BIT(0), 0);
		if (!ret)
			ret = s7d_vpu_afbc_reset_check(v, osds[i] + OSD_FIFO_OFFSET,
						      OSD_FIFO_STATE, 0);
	}
	if (!ret)
		ret = s7d_vpu_afbc_reset_check(v, ASYNC_STAT, ASYNC_IDLE, ASYNC_IDLE);
	if (!ret)
		ret = s7d_vpu_afbc_reset_check(v, AFBC_STATUS, U32_MAX, BIT(2));
	if (!ret)
		ret = s7d_vpu_afbc_reset_check(v, AFBC_TOP, BIT(31) | BIT(23), BIT(23));
	if (ret)
		return ret;
	v->failed_reg = 0x1a01;
	ret = reset_control_assert(reset);
	if (ret)
		return ret;
	ret = reset_control_status(reset);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;
	ret = reset_control_status(reset);
	return ret < 0 ? ret : ret ? -EIO : 0;
}

static void s7d_vpu_afbc_admission(struct s7d_vpu *v)
{
	struct s7d_afbc_observation o;
	u32 format, unpack;
	unsigned int i;
	int ret;

	if (!v->afbc_available)
		return;
	ret = s7d_vpu_afbc_resets(v);
	if (!ret)
		ret = s7d_afbc_engine_read(&v->afbc, &o);
	if (!ret && ((o.raw & GENMASK(5, 2)) || (o.status & BIT(2))))
		ret = -EIO;
	if (!ret && ((o.status & GENMASK(1, 0)) || (o.top & BIT(31)) || o.surfaces))
		ret = -EBUSY;
	v->afbc_ready_mask = 0;
	for (i = 0; i < ARRAY_SIZE(v->afbc_admission_error); i++) {
		const struct s7d_afbc_surface *s = s7d_afbc_surface_get(BIT(i));
		int err = ret;

		if (!err)
			err = s7d_vpu_afbc_read(v, s->bank + 2, &format);
		if (!err && (format & BIT(19)))
			err = -EOPNOTSUPP;
		if (!err)
			err = s7d_vpu_afbc_read(v, s->unpack, &unpack);
		if (!err && (unpack & (BIT(16) | BIT(28))))
			err = -EOPNOTSUPP;
		v->afbc_admission_error[i] = err;
		if (!err)
			v->afbc_ready_mask |= s->mask;
	}
}

static int s7d_vpu_afbc_stop_sample(struct s7d_vpu *v)
{
	struct s7d_afbc_observation o;
	int ret = s7d_afbc_engine_read(&v->afbc, &o);

	return ret ? ret : s7d_afbc_engine_stop_sample(&v->afbc, &o);
}

static int s7d_vpu_afbc_stop_decode(struct s7d_vpu *v)
{
	int sample, ret;

	ret = read_poll_timeout(s7d_vpu_afbc_stop_sample, sample, sample != 0,
			       10, 50000, false, v);
	return ret ? s7d_afbc_engine_fail(&v->afbc, ret, AFBC_STATUS) :
		     (sample < 0 ? sample : 0);
}

struct s7d_vpu_submission {
	struct s7d_vpu *vpu;
	const struct s7d_crtc_state *state;
};

static int s7d_vpu_afbc_replay_barrier(void *data)
{
	struct s7d_vpu *v = data;

	lockdep_assert_held(&v->frame_lock);
	return s7d_afbc_engine_replay_barrier(&v->afbc);
}

static int s7d_vpu_afbc_replay_stage(struct s7d_vpu_submission *submission,
				    const struct s7d_scanout_buffers *active,
				    const struct s7d_scanout_buffers *candidate)
{
	struct s7d_vpu *v = submission->vpu;
	const struct s7d_crtc_state *state = submission->state;
	int ret;

	if (candidate->afbc.generation != active->afbc.generation) {
		ret = s7d_afbc_engine_stage_decode(&v->afbc, &candidate->afbc.plan,
						 candidate->afbc.generation);
		if (ret)
			return ret;
	}
	ret = s7d_rdma_loop_stage(&v->rdma, state->update, state->update_count, 0,
		candidate->afbc.plan.header_addr, S7D_RDMA_AFBC_DIRECT,
		candidate->generation);
	if (!ret)
		v->afbc_replay_pending_cookie = candidate->generation;
	return ret;
}

static int s7d_vpu_afbc_stage(void *data,
			     const struct s7d_scanout_buffers *active,
			     const struct s7d_scanout_buffers *candidate,
			     u64 generation,
			     enum s7d_scanout_transaction transaction)
{
	struct s7d_vpu_submission *submission = data;
	struct s7d_vpu *v = submission->vpu;
	struct s7d_afbc_engine *e = &v->afbc;

	lockdep_assert_held(&v->frame_lock);
	if (!candidate->afbc.fb && !active->afbc.fb)
		return v->afbc_enabled ? -EIO : 0;
	if (!v->afbc_enabled || !candidate->afbc.fb || !active->afbc.fb ||
	    !e->bound_valid || e->generation != active->afbc.generation ||
	    !s7d_afbc_same_layout(&e->bound, &active->afbc.plan) ||
	    e->bound.header_addr != active->afbc.plan.header_addr)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_SURFACES);
	if (candidate->afbc.generation == active->afbc.generation) {
		if (e->phase != S7D_AFBC_RUNNING || e->pending_valid ||
		    !s7d_scanout_afbc_equal(active, candidate))
			return s7d_afbc_engine_fail(e, -EIO, AFBC_SURFACES);
		if (transaction == S7D_SCANOUT_TRANSACTION_REPLAY)
			return s7d_vpu_afbc_replay_stage(submission, active, candidate);
		return 0;
	}
	if (candidate->afbc.generation != generation)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_SURFACES);
	if (transaction == S7D_SCANOUT_TRANSACTION_REPLAY)
		return s7d_vpu_afbc_replay_stage(submission, active, candidate);
	if (transaction == S7D_SCANOUT_TRANSACTION_DECODE_ONLY)
		return s7d_afbc_engine_stage_decode(e, &candidate->afbc.plan, generation);
	return s7d_afbc_engine_stage(e, &candidate->afbc.plan, generation);
}

static int s7d_vpu_afbc_cancel(void *data, u64 generation)
{
	struct s7d_vpu_submission *submission = data;
	struct s7d_vpu *v = submission->vpu;

	lockdep_assert_held(&v->frame_lock);
	if (v->afbc_replay && v->afbc_replay_pending_cookie == generation) {
		int ret = s7d_rdma_loop_cancel(&v->rdma);

		if (ret)
			return ret;
		v->afbc_replay_pending_cookie = 0;
	}
	if (v->afbc_replay && !v->afbc.pending_valid)
		return v->afbc.phase == S7D_AFBC_RUNNING ? 0 : v->afbc.last_error ?: -EIO;
	return v->afbc_enabled ?
		s7d_afbc_engine_cancel_stage(&v->afbc, generation) : 0;
}

static int s7d_vpu_submit(void *data, const struct s7d_crtc_state *state)
{
	struct s7d_vpu *v = data;
	struct s7d_vpu_submission submission = { .vpu = v, .state = state };
	enum s7d_scanout_transaction transaction;
	unsigned long flags;
	int ret;

	s7d_scanout_flush_retired(&v->scanout);
	spin_lock_irqsave(&v->frame_lock, flags);
	transaction = state->afbc_decode_only ? S7D_SCANOUT_TRANSACTION_DECODE_ONLY :
		S7D_SCANOUT_TRANSACTION_RDMA;
	if (v->afbc_replay) {
		if (!state->afbc_replay_compatible || v->afbc_replay_pending_cookie ||
		    v->afbc_replay_apply_pending) {
			ret = -EBUSY;
			goto out;
		}
		transaction = S7D_SCANOUT_TRANSACTION_REPLAY;
	}
	if (state->buffers.afbc.fb && state->buffers.video.fb &&
	    state->video_submitted && !state->afbc_unchanged) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = s7d_scanout_submit_staged(&v->scanout, &state->buffers, state->update,
		state->update_count, state->video_unchanged, state->video_address_only,
		state->afbc_unchanged,
		transaction,
		state->afbc_decode_only || state->afbc_replay_compatible ?
			&state->previous_afbc : NULL,
		s7d_vpu_afbc_stage,
		s7d_vpu_afbc_cancel, &submission);
	if (!ret && transaction == S7D_SCANOUT_TRANSACTION_REPLAY) {
		ret = s7d_crtc_expect_latch(v->crtc,
					 s7d_scanout_pending_generation(&v->scanout));
		if (ret)
			s7d_scanout_fail(&v->scanout);
	}
out:
	spin_unlock_irqrestore(&v->frame_lock, flags);
	return ret;
}

static bool rate_matches(unsigned long requested, unsigned long actual)
{
	u64 delta = requested > actual ? requested - actual : actual - requested;

	return actual && delta * 1000000 <= (u64)requested * CLOCK_ERROR_PPM;
}

static int s7d_vpu_check(void *data, const struct s7d_crtc_state *state)
{
	struct s7d_vpu *v = data;
	struct s7d_hdmi_pll_rate plan;
	int ret;

	if (state->buffers.video2.fb && state->buffers.afbc.fb)
		return -EINVAL;
	if (state->buffers.afbc.fb) {
		u32 mask = state->buffers.afbc.plan.surface_mask;

		ret = s7d_afbc_check_state(&state->buffers.afbc.plan);
		if (ret)
			return ret;
		if (!v->afbc_available || !(v->afbc_ready_mask & mask))
			return v->afbc_admission_error[mask == BIT(0) ? 0 : 1] ?: -EOPNOTSUPP;
	}
	/* One unscaled RGB pixel per VPU cycle; protect this rate at prepare. */
	if (state->pixel_rate < 25175000 || state->pixel_rate > 594000000 ||
	    state->base.adjusted_mode.hdisplay > 4096 ||
	    state->base.adjusted_mode.vdisplay > 2160 ||
	    clk_get_rate(v->clocks[0].clk) < state->pixel_rate)
		return -ERANGE;
	if (state->buffers.afbc.fb && state->buffers.video.fb &&
	    clk_get_rate(v->clocks[0].clk) < 2 * state->pixel_rate)
		return -ERANGE;
	ret = s7d_osd_scaler_check_mode(state->osd.scalers,
		state->base.adjusted_mode.htotal, state->pixel_rate,
		clk_get_rate(v->clocks[0].clk));
	if (ret)
		return ret;
	ret = s7d_video_scaler_check_mode(&state->video.scaler,
		state->base.adjusted_mode.hdisplay, state->base.adjusted_mode.vdisplay,
		state->pixel_rate, clk_get_rate(v->clocks[0].clk));
	if (ret)
		return ret;
	ret = s7d_video_osd_scaler_check_mode(&state->video.scaler, state->osd.scalers,
		state->base.adjusted_mode.htotal, state->pixel_rate,
		clk_get_rate(v->clocks[0].clk));
	if (ret)
		return ret;
	/*
	 * clk_round_rate() is constrained by the live PHY's exclusive vote.
	 * Validate capability with the same pure planner as the CCF provider;
	 * leave protection and hardware untouched until the actual modeset.
	 */
	ret = s7d_hdmi_pll_calculate(state->pixel_rate, clk_get_rate(v->xtal), &plan);
	if (ret)
		return ret;
	return rate_matches(state->pixel_rate, plan.rate) ? 0 : -ERANGE;
}

static int check_handoff(struct s7d_vpu *v);

static void s7d_vpu_disable_pixel(struct s7d_vpu *v)
{
	if (!v->pixel_enabled)
		return;
	clk_disable_unprepare(v->clocks[PIXEL_CLK].clk);
	v->pixel_enabled = false;
}

static int s7d_vpu_acquire(void *data)
{
	struct s7d_vpu *v = data;
	int ret;

	if (v->boot_held) {
		v->boot_held = false;
		return 0;
	}
	if (v->acquired)
		return -EBUSY;
	ret = pm_runtime_resume_and_get(v->dev);
	if (ret < 0)
		return ret;
	ret = clk_bulk_prepare_enable(PIXEL_CLK, v->clocks);
	if (ret) {
		pm_runtime_put(v->dev);
		return ret;
	}
	v->acquired = true;
	ret = check_handoff(v);
	if (ret)
		goto disable_bus;
	/* Keep a live firmware output until prepare has verified its stop. */
	if (clk_get_rate(v->clocks[PIXEL_CLK].clk) &&
	    (vpu_read(v, ENCP_EN) & BIT(0))) {
		ret = clk_prepare_enable(v->clocks[PIXEL_CLK].clk);
		if (ret)
			goto disable_bus;
		v->pixel_enabled = true;
	}
	s7d_vpu_afbc_admission(v);
	return 0;
disable_bus:
	v->acquired = false;
	clk_bulk_disable_unprepare(PIXEL_CLK, v->clocks);
	pm_runtime_put(v->dev);
	return ret;
}

int s7d_vpu_hold_boot(struct s7d_vpu *v)
{
	int ret;

	if (v->acquired || v->touched)
		return -EBUSY;
	ret = s7d_vpu_acquire(v);
	if (!ret)
		v->boot_held = true;
	return ret;
}

static void s7d_vpu_disable_vblank(void *data)
{
	struct s7d_vpu *v = data;

	if (atomic_xchg(&v->vsync_enabled, 0))
		disable_irq_nosync(v->vsync_irq);
}

static int s7d_vpu_enable_vblank(void *data)
{
	struct s7d_vpu *v = data;

	if (!v->acquired || !v->touched)
		return -EIO;
	/* This is a direct GIC SPI, with no sleeping irqchip bus operations. */
	if (!atomic_xchg(&v->vsync_enabled, 1))
		enable_irq(v->vsync_irq);
	return 0;
}

static void mask_irqs(struct s7d_vpu *v)
{
	s7d_vpu_disable_vblank(v);
	if (v->rdma_enabled) {
		disable_irq_nosync(v->rdma_irq);
		v->rdma_enabled = false;
	}
	synchronize_irq(v->vsync_irq);
	synchronize_irq(v->rdma_irq);
}

static int stop_encoders(struct s7d_vpu *v)
{
	static const u32 enables[] = { ENCP_EN, ENCI_EN, ENCL_EN };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(enables); i++) {
		ret = vpu_update(v, enables[i], BIT(0), 0);
		if (ret)
			return ret;
	}
	return 0;
}

static int s7d_vpu_stop_hw(void *data)
{
	struct s7d_vpu *v = data;
	static const u32 osds[] = { OSD1_CTRL, OSD2_CTRL };
	static const u32 videos[] = { VD1_GEN, VD2_GEN };
	u32 value;
	unsigned int i;
	const struct s7d_afbc_surface *surface = NULL;
	bool abort_prepare = false;
	bool recover_detiling = false;
	int ret;

	mask_irqs(v);
	cancel_delayed_work_sync(&v->afbc_timeout);
	if (!v->touched)
		return 0;
	if (!v->acquired)
		return -EIO;
	/* Also used by CRTC error work, which bypasses DRM bridge disable. */
	ret = v->link.quiesce(v->link.data);
	if (ret)
		return ret;
	/*
	 * Reset old RDMA writers before disabling fetch. Keep the current VENC
	 * running until OSD disable and outstanding reads have drained: stopping
	 * its field/line strobes first leaves OSD_ENABLE latched on S7D.
	 */
	ret = s7d_rdma_quiesce(&v->rdma);
	if (ret)
		return ret;
	if (v->afbc_enabled) {
		surface = s7d_afbc_surface_get(v->afbc_surface_mask);
		if (!surface || (v->normal_unpack_owned & surface->mask))
			return -EIO;
		abort_prepare = s7d_afbc_engine_can_abort_prepare(&v->afbc);
		recover_detiling = s7d_afbc_engine_can_recover_detiling(&v->afbc) &&
			s7d_vpu_afbc_fault_owned(v);
		if (!abort_prepare && !recover_detiling) {
			ret = s7d_afbc_engine_stop_begin(&v->afbc);
			if (ret)
				return ret;
		}
	}
	if (v->afbc_enabled && !abort_prepare && !recover_detiling) {
		ret = s7d_vpu_afbc_stop_decode(v);
		if (ret)
			return ret;
	}
	if (!v->osd_draining) {
		for (i = 0; i < ARRAY_SIZE(osds); i++)
			v->osd_free_clk[i] = vpu_read(v, osds[i]) & OSD_FREE_CLK;
		v->osd_draining = true;
	}
	for (i = 0; i < ARRAY_SIZE(videos); i++) {
		if (!v->video_draining[i]) {
			v->video_free_clk[i] = vpu_read(v, videos[i]) & VD1_FREE_CLK;
			v->video_draining[i] = true;
		}
		ret = vpu_update(v, videos[i], VD1_FREE_CLK | BIT(30) | BIT(7) | BIT(0),
				 VD1_FREE_CLK);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(videos); i++) {
		ret = readl_poll_timeout(v->regs + videos[i] * 4, value,
					!(value & (VD1_BUSY | BIT(0))), 10, 50000);
		if (ret) {
			v->failed_reg = videos[i];
			return ret;
		}
	}
	for (i = 0; i < ARRAY_SIZE(osds); i++) {
		/*
		 * Keep the local OSD clock running until disable reaches OSD_ENABLE and DMA
		 * drains (S905X5M, VIU_OSD1_CTRL_STAT ENABLE_FREE_CLK).
		 */
		ret = vpu_update(v, osds[i], OSD_CFG_SYNC | OSD_FREE_CLK | BIT(0),
				 OSD_FREE_CLK);
		if (ret)
			return ret;
		ret = readl_poll_timeout(v->regs + osds[i] * 4, value,
					!(value & OSD_ACTUAL_EN), 10, 50000);
		if (ret) {
			v->failed_reg = osds[i];
			return ret;
		}
		ret = readl_poll_timeout(v->regs + (osds[i] + OSD_FIFO_OFFSET) * 4,
					value, !(value & OSD_FIFO_STATE), 10, 50000);
		if (ret) {
			v->failed_reg = osds[i] + OSD_FIFO_OFFSET;
			return ret;
		}
	}
	/* No fixed sleep authorizes buffer release. Fail and retain on timeout. */
	ret = readl_poll_timeout(v->regs + ASYNC_STAT * 4, value,
				(value & ASYNC_IDLE) == ASYNC_IDLE, 10, 50000);
	if (ret) {
		v->failed_reg = ASYNC_STAT;
		return ret;
	}
	if (v->afbc_enabled) {
		if (abort_prepare) {
			dev_err(v->dev, "AFBC unstarted prepare: %d reg %#x gen %llu readback %d expected %#x observed %#x\n",
				v->afbc.last_error, v->afbc.failed_reg,
				v->afbc.failed_generation, v->afbc.failed_readback,
				v->afbc.failed_expected, v->afbc.failed_observed);
			ret = s7d_afbc_engine_abort_prepare(&v->afbc, &v->failed_reg);
			if (ret)
				dev_err(v->dev, "AFBC prepare abort failed: %d reg %#x; retaining scanout resources\n",
					ret, v->failed_reg);
		} else if (recover_detiling) {
			u32 fault_reg = v->afbc.failed_reg;

			dev_err(v->dev, "AFBC detiling fault: gen %llu epoch %llu raw %#x status %#x; resetting drained decoder\n",
				v->afbc.failed_generation, v->afbc.failed_epoch,
				v->afbc.observed.raw, v->afbc.observed.status);
			ret = s7d_afbc_engine_recover_detiling(&v->afbc, &v->failed_reg);
			if (!ret)
				v->failed_reg = fault_reg;
		} else {
			ret = s7d_afbc_engine_stop_finish(&v->afbc);
		}
		if (ret)
			return ret;
		ret = vpu_update(v, OSD_PATH, surface->route_mask, 0);
		if (!ret)
			ret = vpu_update(v, surface->unpack, v->afbc_unpack_mask, 0);
		if (!ret)
			ret = vpu_update(v, surface->alpha, BIT(1), 0);
		if (ret)
			return ret;
		v->afbc_enabled = false;
		v->afbc_surface_mask = 0;
		v->afbc_unpack_mask = 0;
	}
	for (i = 0; i < ARRAY_SIZE(osds); i++) {
		u32 reg = i ? 0x1abd : 0x1a2f;

		if (!(v->normal_unpack_owned & BIT(i)))
			continue;
		ret = vpu_update(v, reg, s7d_osd_unpack_mask(reg), 0);
		if (ret)
			return ret;
	}
	ret = stop_encoders(v);
	if (ret)
		return ret;
	if (v->osd_scaler_route_owned) {
		/* Fetch, FIFO, READ0 and VENC are idle; clock references still held. */
		ret = vpu_update(v, OSD_SCALE_CTRL, GENMASK(2, 0),
				 v->osd_scaler_route_saved);
		if (ret)
			return ret;
		v->osd_scaler_route_owned = false;
		v->osd_scaler_route_saved = 0;
	}
	for (i = 0; i < ARRAY_SIZE(videos); i++) {
		ret = vpu_update(v, videos[i], VD1_FREE_CLK, v->video_free_clk[i]);
		if (ret)
			return ret;
		v->video_draining[i] = false;
	}
	for (i = 0; i < ARRAY_SIZE(osds); i++) {
		ret = vpu_update(v, osds[i], OSD_FREE_CLK, v->osd_free_clk[i]);
		if (ret)
			return ret;
	}
	v->osd_draining = false;
	v->normal_unpack_owned = 0;
	v->afbc_replay = false;
	v->afbc_replay_cookie = 0;
	v->afbc_replay_pending_cookie = 0;
	v->afbc_replay_latch_pending = false;
	v->afbc_replay_apply_pending = false;
	v->afbc_replay_afbc_changed = false;
	if (v->pixel_protected) {
		clk_rate_exclusive_put(v->clocks[PIXEL_CLK].clk);
		v->pixel_protected = false;
	}
	if (v->core_protected) {
		clk_rate_exclusive_put(v->clocks[0].clk);
		v->core_protected = false;
	}
	s7d_vpu_disable_pixel(v);
	return 0;
}

static int s7d_vpu_stop(void *data)
{
	struct s7d_vpu *v = data;
	int ret = s7d_vpu_stop_hw(v);

	if (ret && v->afbc_enabled)
		s7d_afbc_engine_fail(&v->afbc, ret, v->failed_reg);
	return ret;
}

static int check_handoff(struct s7d_vpu *v)
{
	unsigned int i;
	int ret;

	/* Cold/reset domains need a separate initialization sequence. */
	for (i = 0; i < ARRAY_SIZE(v->resets); i++) {
		ret = reset_control_status(v->resets[i].rstc);
		if (ret)
			return ret < 0 ? ret : -EHOSTDOWN;
	}
	/*
	 * Initial takeover supports firmware's linear OSD path. Refuse video,
	 * GFCD/AFBC routes rather than guessing their stop/idle/reset protocols.
	 * The provider profile excludes Linux video/capture and legacy writers.
	 */
	if ((vpu_read(v, OSD_PATH) & (BIT(14) | GENMASK(12, 9) | GENMASK(7, 0))) ||
	    (vpu_read(v, VD1_PATH) & 0x007dff00) ||
	    (vpu_read(v, VD2_PATH) & 0x007dfe00) ||
	    (vpu_read(v, 0x1d26) & BIT(27)) ||
	    (!v->touched &&
	     ((vpu_read(v, VD1_BLEND) | vpu_read(v, VD2_BLEND)) & 0x0f0f)))
		return -EOPNOTSUPP;
	/* ENCP FIFO must use cts_vpu_clk, not an unowned vpu_clkc route. */
	if (vpu_read(v, VENC_CLK) & BIT(0))
		return -EOPNOTSUPP;
	/* Both OSD arbiter branches and RDMA must use the READ0 idle we check. */
	if (vpu_read(v, RDARB_MODE_L2) & GENMASK(18, 16))
		return -EOPNOTSUPP;
	return 0;
}

static int program_osd_scaler(struct s7d_vpu *v,
			      const struct s7d_osd_scaler_state states[2])
{
	if (v->osd_scaler_route_owned)
		return -EIO;
	if (!states[0].enabled && !states[1].enabled)
		return 0;
	/* prepare() has verified stop and holds the VPU/clock references. */
	v->osd_scaler_route_saved = vpu_read(v, OSD_SCALE_CTRL) & GENMASK(2, 0);
	/* Own it before setup: a partial write failure must also be restored. */
	v->osd_scaler_route_owned = true;
	return s7d_osd_scaler_setup(v->regs, states, &v->failed_reg);
}

static int program_video_scaler(struct s7d_vpu *v,
				const struct s7d_video_scaler_state *state)
{
	u32 bank, phase, pair, word, component;
	unsigned int i;
	int ret;

	if (!state->enabled)
		return 0;
	if (state->setup_count > S7D_VIDEO_SCALER_SETUP_COUNT)
		return -EINVAL;
	/* Clear the documented preblend-to-VADJ1 bypass when scaling. */
	ret = vpu_update(v, 0x1d93, BIT(0), 0);
	if (ret)
		return ret;
	/* The selected S7D path scales before VE, whose input is the output size. */
	ret = vpu_update(v, 0x1d26, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = vpu_update(v, 0x1da4, 0x1fff1fff,
			 (state->dst_width << 16) | state->dst_height);
	if (ret)
		return ret;
	ret = vpu_update(v, state->pre_scale.reg, state->pre_scale.mask,
			 state->pre_scale.value);
	if (ret)
		return ret;
	for (i = 0; i < state->setup_count; i++) {
		const struct s7d_video_scaler_reg *s = &state->setup[i];

		/* Match the selected S7D size write; stopped readback is unqualified. */
		if (s->reg == 0x1da6) {
			writel(s->value, v->regs + s->reg * 4);
			continue;
		}
		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	/* Match the selected input-size writes before enabling VSR. */
	writel(state->src_width, v->regs + 0x1d01 * 4);
	writel(state->src_height, v->regs + 0x1d02 * 4);
	writel((state->src_height << 16) | state->src_width,
	       v->regs + 0x1d20 * 4);
	/* Indirect auto-increment ports are write-only during stopped preparation. */
	for (component = 0; component < 2; component++) {
		u32 index = component ? S7D_VIDEO_SCALER_COEF_IDX_CHROMA :
			S7D_VIDEO_SCALER_COEF_IDX_LUMA;
		u32 data = component ? S7D_VIDEO_SCALER_COEF_DATA_CHROMA :
			S7D_VIDEO_SCALER_COEF_DATA_LUMA;

		for (bank = 0; bank < S7D_VIDEO_SCALER_COEF_BANKS; bank++) {
			writel(bank << 7, v->regs + index * 4);
			for (phase = 0; phase < S7D_VIDEO_SCALER_COEF_PHASES; phase++) {
				for (pair = 0; pair < S7D_VIDEO_SCALER_COEF_PAIRS; pair++) {
					ret = s7d_video_scaler_coefficient(bank, phase,
								   pair, &word);
					if (ret)
						return ret;
					writel(word, v->regs + data * 4);
				}
			}
		}
	}
	return vpu_update(v, state->control.reg, state->control.mask, state->control.value);
}

static int program_video2_pps(struct s7d_vpu *v,
			      const struct s7d_video_pipeline_state *video)
{
	struct s7d_video2_pps_state pps;
	unsigned int i;
	int ret;

	if (!video->update_count)
		return 0;
	ret = s7d_video_build_vd2_pps(video->scaler.src_width,
				    video->scaler.src_height, &pps);
	if (ret)
		return ret;
	/* Fetch and VENC are stopped; close the PPS before changing geometry. */
	ret = vpu_update(v, pps.control.reg, pps.control.mask, 0);
	if (ret)
		return ret;
	for (i = 0; i < S7D_VIDEO2_PPS_SETUP_REG_COUNT; i++) {
		const struct s7d_video_reg_setting *s = &pps.setup[i];

		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	return vpu_update(v, pps.control.reg, pps.control.mask, pps.control.value);
}

static int program_output(struct s7d_vpu *v, const struct s7d_crtc_state *state)
{
	/* S7D VENC supplies BRG; rotate to RGB before the HDMI formatter. */
	u32 setting = BIT(16) | BIT(1) | (state->encp.hsync_positive ? BIT(2) : 0) |
				(state->encp.vsync_positive ? BIT(3) : 0);
	unsigned int i;
	int ret;

	ret = s7d_vpp_setup(v->regs, state->base.adjusted_mode.hdisplay,
			    state->base.adjusted_mode.vdisplay, &v->failed_reg);
	if (ret)
		return ret;
	for (i = 0; i < S7D_VIDEO_SETUP_REG_COUNT; i++) {
		const struct s7d_video_reg_setting *s = &state->video.setup[i];

		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	for (i = 0; i < S7D_VIDEO_SETUP_REG_COUNT; i++) {
		const struct s7d_video_reg_setting *s = &state->video2.setup[i];

		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	ret = program_video_scaler(v, &state->video.scaler);
	if (ret)
		return ret;
	ret = program_video2_pps(v, &state->video2);
	if (ret)
		return ret;
	for (i = 0; i < S7D_CSC_MATRIX_REG_COUNT; i++) {
		const struct s7d_csc_reg *s = &state->csc.matrix[i];

		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	for (i = 0; i < S7D_CSC_CONTROL_REG_COUNT; i++) {
		const struct s7d_csc_reg *s = &state->csc.control[i];

		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	for (i = 0; i < S7D_CSC_MATRIX_REG_COUNT; i++) {
		const struct s7d_csc_reg *s = &state->csc2.matrix[i];

		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	for (i = 0; i < S7D_CSC_CONTROL_REG_COUNT; i++) {
		const struct s7d_csc_reg *s = &state->csc2.control[i];

		ret = vpu_update(v, s->reg, s->mask, s->value);
		if (ret)
			return ret;
	}
	ret = vpu_update(v, 0x1d26, BIT(27) | GENMASK(26, 18), 256 << 18);
	if (ret)
		return ret;
	ret = vpu_update(v, 0x58b0, GENMASK(28, 0), BIT(0));
	if (ret)
		return ret;
	ret = vpu_update(v, 0x1d76, GENMASK(8, 0), 256);
	if (ret)
		return ret;
	ret = vpu_update(v, 0x3968, GENMASK(23, 0), 0);
	if (ret)
		return ret;
	ret = vpu_update(v, 0x3969, 0x1ffff9ff, 0);
	if (ret)
		return ret;
	for (i = 0; i < S7D_ENCP_REG_COUNT; i++) {
		ret = vpu_update(v, state->encp.regs[i].reg, U32_MAX,
				 state->encp.regs[i].value);
		if (ret)
			return ret;
	}
	/* VIU1 from ENCP, coupled sync selection; preserve VIU2 selection. */
	ret = vpu_update(v, VENC_MUX, BIT(20) | GENMASK(1, 0), 2);
	if (ret)
		return ret;
	/* S7D selects ENCP here; it has no T7-style per-VENC selector at 0x1cef. */
	/* RGB straight through: S7D's optional YUV->RGB matrix stays disabled. */
	ret = vpu_update(v, HDMI_FMT, U32_MAX, (2 << 22) | BIT(10) | (2 << 2));
	if (ret)
		return ret;
	ret = vpu_update(v, HDMI_SETTING, U32_MAX, setting);
	if (ret)
		return ret;
	ret = vpu_update(v, HDMI_DITH, U32_MAX, BIT(10));
	if (ret)
		return ret;
	ret = program_osd_scaler(v, state->osd.scalers);
	if (ret)
		return ret;
	for (i = 0; i < S7D_OSD_SETUP_REG_COUNT; i++) {
		u32 reg = le32_to_cpu(state->osd.setup[i].reg);
		u32 expected = le32_to_cpu(state->osd.setup[i].value);
		u32 mask = s7d_osd_unpack_mask(reg);

		if (mask) {
			if (state->buffers.afbc.fb &&
			    reg == state->buffers.afbc.plan.unpack.reg)
				continue;
			v->normal_unpack_owned |= reg == 0x1a2f ? BIT(0) : BIT(1);
			ret = vpu_update(v, reg, mask, expected);
			if (ret)
				return ret;
			continue;
		}
		mask = U32_MAX;
		if (reg == OSD1_CTRL + OSD_FIFO_OFFSET ||
		    reg == OSD2_CTRL + OSD_FIFO_OFFSET)
			expected = (expected & ~GENMASK(9, 5)) |
				   (state->encp.fifo_hold_lines << 5);
		if (reg == OSD2_CTRL)
			v->osd2_enable = expected & BIT(0);
		if (reg == OSD1_CTRL || reg == OSD2_CTRL)
			expected &= ~BIT(0);
		writel(expected, v->regs + reg * 4);
		/* Mixed status/config and Rev.B's documented alpha encoding. */
		if (reg == OSD1_CTRL || reg == OSD2_CTRL)
			mask = OSD_CFG_SYNC | GENMASK(20, 12) | GENMASK(3, 0);
		else if (reg == 0x1a2d || reg == 0x1a4d)
			mask = GENMASK(15, 0);
		else if (reg == 0x39ba)
			expected >>= 2;
		if ((vpu_read(v, reg) & mask) != (expected & mask)) {
			v->failed_reg = reg;
			return -EIO;
		}
	}
	for (i = 0; i < state->video.update_count; i++) {
		ret = vpu_update(v, le32_to_cpu(state->video.update[i].reg), U32_MAX,
				 le32_to_cpu(state->video.update[i].value));
		if (ret)
			return ret;
	}
	for (i = 0; i < state->video2.update_count; i++) {
		ret = vpu_update(v, le32_to_cpu(state->video2.update[i].reg), U32_MAX,
				 le32_to_cpu(state->video2.update[i].value));
		if (ret)
			return ret;
	}
	for (i = 0; i < S7D_POSTBLEND_REG_COUNT; i++) {
		ret = vpu_update(v, le32_to_cpu(state->postblend.regs[i].reg), U32_MAX,
				 le32_to_cpu(state->postblend.regs[i].value));
		if (ret)
			return ret;
	}
	v->video_enable[0] = state->video.control.value & BIT(0);
	v->video_enable[1] = state->video2.control.value & BIT(0);
	ret = vpu_update(v, state->video.control.reg,
			 state->video.control.mask | BIT(30),
			 state->video.control.value & ~BIT(0));
	if (ret)
		return ret;
	return vpu_update(v, state->video2.control.reg,
			  state->video2.control.mask | BIT(30),
			  state->video2.control.value & ~BIT(0));
}

static int s7d_vpu_prepare_pixel(struct s7d_vpu *v, unsigned long rate)
{
	int ret;

	if (v->pixel_enabled)
		return -EBUSY;
	ret = clk_set_parent(v->pixel_route[PIXEL_SOURCE].clk,
			     v->pixel_route[PIXEL_PLL].clk);
	if (ret)
		return ret;
	ret = clk_set_parent(v->pixel_route[PIXEL_MUX].clk,
			     v->pixel_route[PIXEL_PARENT].clk);
	if (ret)
		return ret;
	/* A cold PLL has no saved configuration for its prepare callback. */
	ret = clk_set_rate(v->pixel_route[PIXEL_PLL].clk, rate);
	if (ret)
		return ret;
	if (!rate_matches(rate, clk_get_rate(v->pixel_route[PIXEL_PLL].clk)))
		return -ERANGE;
	ret = clk_set_rate(v->clocks[PIXEL_CLK].clk, rate);
	if (ret)
		return ret;
	if (!rate_matches(rate, clk_get_rate(v->clocks[PIXEL_CLK].clk)))
		return -ERANGE;
	ret = clk_prepare_enable(v->clocks[PIXEL_CLK].clk);
	if (!ret)
		v->pixel_enabled = true;
	return ret;
}

static int s7d_vpu_prepare(void *data, const struct s7d_crtc_state *state)
{
	struct s7d_vpu *v = data;
	int ret;

	v->failed_reg = 0;
	if (!v->acquired)
		return -EIO;
	/* From here even a partial failure needs a verified stop before release. */
	v->touched = true;
	ret = s7d_vpu_stop(v);
	if (ret)
		return ret;
	ret = clk_rate_exclusive_get(v->clocks[0].clk);
	if (ret)
		return ret;
	v->core_protected = true;
	if (clk_get_rate(v->clocks[0].clk) < state->pixel_rate)
		return -ERANGE;
	if (state->buffers.afbc.fb && state->buffers.video.fb &&
	    clk_get_rate(v->clocks[0].clk) < 2 * state->pixel_rate)
		return -ERANGE;
	ret = s7d_osd_scaler_check_mode(state->osd.scalers,
		state->base.adjusted_mode.htotal, state->pixel_rate,
		clk_get_rate(v->clocks[0].clk));
	if (ret)
		return ret;
	ret = s7d_video_scaler_check_mode(&state->video.scaler,
		state->base.adjusted_mode.hdisplay, state->base.adjusted_mode.vdisplay,
		state->pixel_rate, clk_get_rate(v->clocks[0].clk));
	if (ret)
		return ret;
	ret = s7d_video_osd_scaler_check_mode(&state->video.scaler, state->osd.scalers,
		state->base.adjusted_mode.htotal, state->pixel_rate,
		clk_get_rate(v->clocks[0].clk));
	if (ret)
		return ret;
	ret = s7d_vpu_prepare_pixel(v, state->pixel_rate);
	if (ret)
		return ret;
	ret = clk_rate_exclusive_get(v->clocks[PIXEL_CLK].clk);
	if (ret)
		return ret;
	v->pixel_protected = true;
	ret = v->link.prepare(v->link.data, clk_get_rate(v->clocks[PIXEL_CLK].clk));
	if (ret)
		return ret;
	ret = s7d_rdma_prepare(&v->rdma);
	if (ret)
		return ret;
	ret = program_output(v, state);
	if (ret)
		return ret;
	if (state->buffers.afbc.fb) {
		const struct s7d_afbc_state *p = &state->buffers.afbc.plan;
		u64 generation = s7d_scanout_pending_afbc_generation(&v->scanout);

		ret = s7d_vpu_afbc_resets(v);
		if (ret)
			return ret;
		v->afbc_surface_mask = p->surface_mask;
		v->afbc_unpack_mask = p->unpack.mask & ~GENMASK(15, 0);
		v->afbc_enabled = true;
		ret = s7d_afbc_engine_prepare(&v->afbc, p, generation);
		if (!ret)
			ret = vpu_update(v, p->route.reg, p->route.mask, p->route.value);
		if (!ret)
			ret = vpu_update(v, p->unpack.reg, p->unpack.mask, p->unpack.value);
		if (ret)
			return s7d_afbc_engine_fail(&v->afbc, ret, v->failed_reg);
	}
	v->flip_start = state->encp.flip_start;
	v->flip_end = state->encp.flip_end;
	v->afbc_replay_frame_ns = DIV_ROUND_UP_ULL(
		(u64)state->base.adjusted_mode.htotal *
		state->base.adjusted_mode.vtotal * NSEC_PER_SEC, state->pixel_rate);
	/* RDMA done was acknowledged by prepare; clear stale GIC edge state. */
	ret = irq_set_irqchip_state(v->rdma_irq, IRQCHIP_STATE_PENDING, false);
	if (ret)
		return ret;
	return irq_set_irqchip_state(v->vsync_irq, IRQCHIP_STATE_PENDING, false);
}

static int s7d_vpu_start(void *data)
{
	struct s7d_vpu *v = data;
	unsigned long flags;
	int ret;

	if (!v->acquired || !v->touched)
		return -EIO;
	spin_lock_irqsave(&v->frame_lock, flags);
	enable_irq(v->rdma_irq);
	v->rdma_enabled = true;
	ret = vpu_update(v, ENCP_EN, BIT(0), BIT(0));
	if (!ret && v->afbc_enabled) {
		ret = s7d_afbc_engine_start_initial(&v->afbc);
		if (!ret) {
			s7d_vpu_afbc_arm_timeout(v);
			ret = s7d_scanout_afbc_started(&v->scanout,
				v->afbc.generation, v->afbc.epoch, 0);
		}
	}
	if (!ret)
		ret = vpu_update(v, VD1_GEN, BIT(0), v->video_enable[0] ? BIT(0) : 0);
	if (!ret)
		ret = vpu_update(v, VD2_GEN, BIT(0), v->video_enable[1] ? BIT(0) : 0);
	if (!ret)
		ret = vpu_update(v, OSD2_CTRL, BIT(0), v->osd2_enable ? BIT(0) : 0);
	if (!ret)
		ret = vpu_update(v, OSD1_CTRL, BIT(0), BIT(0));
	if (ret && v->afbc_enabled)
		ret = s7d_afbc_engine_fail(&v->afbc, ret, v->failed_reg);
	spin_unlock_irqrestore(&v->frame_lock, flags);
	return ret;
}

static void s7d_vpu_release(void *data)
{
	struct s7d_vpu *v = data;

	if (!v->acquired)
		return;
	s7d_vpu_disable_pixel(v);
	clk_bulk_disable_unprepare(PIXEL_CLK, v->clocks);
	pm_runtime_put(v->dev);
	v->acquired = false;
}

static void s7d_vpu_report_error(void *data, int error)
{
	struct s7d_vpu *v = data;

	if (!s7d_crtc_last_error(v->crtc))
		return;
	if (v->afbc.phase == S7D_AFBC_ERROR)
		dev_err(v->dev, "AFBC failed: %d reg %#x phase %u gen %llu epoch %llu ns %llu caller %pS command %#x last raw %#x status %#x top %#x surfaces %#x readback %d expected %#x observed %#x\n",
			v->afbc.last_error, v->afbc.failed_reg, v->afbc.failed_phase,
			v->afbc.failed_generation, v->afbc.failed_epoch,
			v->afbc.failed_ns, (void *)v->afbc.failed_caller,
			v->afbc_command_counter, v->afbc.observed.raw,
			v->afbc.observed.status, v->afbc.observed.top,
			v->afbc.observed.surfaces, v->afbc.failed_readback,
			v->afbc.failed_expected, v->afbc.failed_observed);
	if (v->acquired)
		dev_err(v->dev, "display stop incomplete: %d, VCBUS word %#x; scanout resources retained\n",
			error, v->failed_reg);
	else
		dev_err(v->dev, "display stopped: %d, VCBUS word %#x; full modeset required\n",
			error, v->failed_reg);
}

const struct s7d_crtc_ops s7d_vpu_crtc_ops = {
	.wait_for_link = true,
	.check = s7d_vpu_check,
	.acquire = s7d_vpu_acquire,
	.prepare = s7d_vpu_prepare,
	.start = s7d_vpu_start,
	.submit = s7d_vpu_submit,
	.stop = s7d_vpu_stop,
	.release = s7d_vpu_release,
	.enable_vblank = s7d_vpu_enable_vblank,
	.disable_vblank = s7d_vpu_disable_vblank,
	.report_error = s7d_vpu_report_error,
};

/* No extra MMIO in normal operation; opt-in tracefs event only. */
static void s7d_vpu_trace_frame(struct s7d_vpu *v, unsigned int kind, int result)
{
	if (trace_s7d_frame_enabled())
		trace_s7d_frame(kind, vpu_read(v, ENCP_INFO_READ),
			       drm_crtc_vblank_count(v->crtc), result);
}

static int s7d_vpu_afbc_sample(struct s7d_vpu *v,
			       enum s7d_rdma_result result,
			       struct s7d_frame_state *frame,
			       const struct s7d_afbc_frame *afbc_frame, u64 cookie)
{
	struct s7d_afbc_observation o;
	int ret;

	frame->afbc_gate = v->afbc_enabled;
	if (!v->afbc_enabled || v->afbc.phase == S7D_AFBC_PREPARED)
		return 0;
	if (result == S7D_RDMA_FAULT)
		return s7d_afbc_engine_fail(&v->afbc, -EIO, 0);
	if (v->afbc_replay) {
		if (result == S7D_RDMA_APPLIED && v->afbc_replay_apply_pending) {
			u64 now = ktime_get_mono_fast_ns();

			if (cookie != v->afbc_replay_cookie ||
			    afbc_frame->sequence != v->afbc_replay_expected_sequence ||
			    now < v->afbc_replay_armed_ns ||
			    now - v->afbc_replay_armed_ns >
				v->afbc_replay_frame_ns + v->afbc_replay_frame_ns / 4)
				return s7d_afbc_engine_fail(&v->afbc, -ESTALE, AFBC_COMMAND);
			if (v->afbc_replay_afbc_changed) {
				u64 generation =
					s7d_scanout_pending_afbc_generation(&v->scanout);

				ret = s7d_afbc_engine_replay_applied(&v->afbc,
					generation, afbc_frame);
				if (ret != 1)
					return s7d_afbc_engine_fail(&v->afbc,
						ret < 0 ? ret : -ESTALE, AFBC_COMMAND);
			}
			ret = s7d_scanout_replay_applied(&v->scanout, cookie,
				v->afbc.generation, v->afbc.epoch, frame->field);
			if (ret)
				return s7d_afbc_engine_fail(&v->afbc, ret, 0);
			v->afbc_replay_apply_pending = false;
			v->afbc_replay_applied_field = frame->field;
			v->afbc_replay_latch_pending = true;
		}
		ret = s7d_afbc_engine_read(&v->afbc, &o);
		if (!ret)
			ret = s7d_afbc_engine_replay_sample(&v->afbc, afbc_frame, &o);
		if (ret >= 0 && cookie == v->afbc_replay_cookie &&
		    (result == S7D_RDMA_APPLIED || result == S7D_RDMA_REPLAY))
			v->afbc_deadline = jiffies + s7d_vpu_afbc_replay_timeout(v);
		trace_s7d_afbc_replay(cookie, v->afbc.generation,
			v->afbc.replay_handover_generation, v->afbc.epoch,
			v->afbc.observed.raw, v->afbc.observed.status,
			frame->field, afbc_frame->vblank, ret);
		frame->afbc_handover_generation = v->afbc.replay_handover_generation;
		frame->afbc_handover_epoch = v->afbc.replay_handover_epoch;
		frame->early = false;
		return ret < 0 ? ret : 0;
	}
	ret = s7d_afbc_engine_read(&v->afbc, &o);
	if (!ret)
		ret = s7d_afbc_engine_sample(&v->afbc, afbc_frame, &o);
	if (ret < 0)
		return ret;
	if (result == S7D_RDMA_COMPLETE && !s7d_scanout_afbc_unchanged(&v->scanout)) {
		ret = s7d_afbc_engine_rdma_drained(&v->afbc,
				 s7d_scanout_pending_afbc_generation(&v->scanout));
		if (ret)
			return s7d_afbc_engine_fail(&v->afbc, ret, 0);
	}
	frame->afbc_completed_generation = v->afbc.completed_generation;
	frame->afbc_completed_epoch = v->afbc.completed_generation_epoch;
	if (!s7d_scanout_afbc_unchanged(&v->scanout))
		frame->early = false;
	return 0;
}

static int s7d_vpu_afbc_restart(struct s7d_vpu *v,
				const struct s7d_afbc_frame *frame)
{
	u64 generation, previous_generation = v->afbc.generation;
	int ret;

	if (v->afbc_replay)
		return 0;
	if (!v->afbc_enabled || !frame->vblank ||
	    v->afbc.phase != S7D_AFBC_RUNNING || s7d_crtc_last_error(v->crtc))
		return 0;
	ret = s7d_afbc_engine_restart(&v->afbc, frame);
	if (ret == -EAGAIN || ret == -EALREADY)
		return 0;
	if (ret)
		return s7d_afbc_engine_fail(&v->afbc, ret, v->failed_reg);
	s7d_vpu_afbc_arm_timeout(v);
	if (v->afbc.generation != previous_generation) {
		generation = s7d_scanout_pending_afbc_generation(&v->scanout);
		if (!generation || generation != v->afbc.generation)
			return s7d_afbc_engine_fail(&v->afbc, -EIO, 0);
		ret = s7d_scanout_afbc_started(&v->scanout, generation, v->afbc.epoch,
					       frame->field);
		if (ret)
			return s7d_afbc_engine_fail(&v->afbc, ret, 0);
	}
	return 0;
}

static int s7d_vpu_afbc_replay_begin(struct s7d_vpu *v,
				    const struct s7d_afbc_frame *frame)
{
	struct s7d_scanout *s = &v->scanout;
	struct s7d_afbc_engine *e = &v->afbc;
	unsigned long flags;
	unsigned int i;
	u64 cookie;
	bool owned;
	int ret;

	if (IS_ENABLED(CONFIG_PREEMPT_RT) || v->afbc_replay ||
	    !v->afbc_enabled || !frame->vblank || !e->last_start_valid ||
	    e->last_start_sequence != frame->sequence ||
	    e->bound.surface_mask != BIT(0) || s7d_crtc_last_error(v->crtc))
		return 0;
	spin_lock_irqsave(&s->lock, flags);
	owned = !s->fault && s->phase == S7D_SCANOUT_IDLE &&
		!s->pending_afbc_epoch &&
		s7d_scanout_replay_compatible(&s->active, &s->active) &&
		s->active.afbc.generation == e->generation &&
		s->active.afbc.plan.header_addr == e->bound.header_addr &&
		s7d_afbc_same_layout(&s->active.afbc.plan, &e->bound);
	for (i = 0; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (s->pending.fb[i] || s->retired.fb[i])
			owned = false;
	cookie = s->active.generation;
	spin_unlock_irqrestore(&s->lock, flags);
	if (!owned || s7d_rdma_check_idle(&v->rdma))
		return 0;
	ret = s7d_afbc_engine_replay_begin(e);
	if (ret)
		return ret;
	v->afbc_replay = true;
	v->afbc_replay_cookie = cookie;
	ret = s7d_rdma_loop_stage(&v->rdma, NULL, 0, 0, e->bound.header_addr,
				  S7D_RDMA_AFBC_DIRECT, v->afbc_replay_cookie);
	if (!ret)
		ret = s7d_rdma_loop_replace(&v->rdma, s7d_vpu_afbc_replay_barrier, v);
	if (ret)
		return s7d_afbc_engine_fail(e, ret, AFBC_COMMAND);
	v->afbc_deadline = jiffies + s7d_vpu_afbc_replay_timeout(v);
	mod_delayed_work(system_wq, &v->afbc_timeout,
			 s7d_vpu_afbc_replay_timeout(v));
	return 0;
}

static int s7d_vpu_afbc_replay_replace(struct s7d_vpu *v,
				      const struct s7d_afbc_frame *frame,
				      enum s7d_rdma_result result, u64 cookie)
{
	u64 sequence;
	u32 before, after;
	int ret;

	if (!v->afbc_replay || !v->afbc_replay_pending_cookie ||
	    s7d_crtc_last_error(v->crtc) || !frame->window ||
	    (result != S7D_RDMA_APPLIED && result != S7D_RDMA_REPLAY) ||
	    cookie != v->afbc_replay_cookie)
		return 0;
	before = vpu_read(v, ENCP_INFO_READ);
	if ((before >> 29) != frame->field ||
	    ((before >> 16) & 0x1fff) >= v->flip_end)
		return 0;
	sequence = READ_ONCE(v->scanout.vblank_seq);
	if (READ_ONCE(v->scanout.vblank_field) != frame->field)
		sequence++;
	if (sequence == ~0ULL)
		return s7d_afbc_engine_fail(&v->afbc, -EOVERFLOW, ENCP_INFO_READ);
	ret = s7d_rdma_loop_replace(&v->rdma, s7d_vpu_afbc_replay_barrier, v);
	if (ret)
		return s7d_afbc_engine_fail(&v->afbc, ret, AFBC_COMMAND);
	v->afbc_replay_armed_ns = ktime_get_mono_fast_ns();
	after = vpu_read(v, ENCP_INFO_READ);
	if ((before >> 29) != (after >> 29) ||
	    ((after >> 16) & 0x1fff) < ((before >> 16) & 0x1fff))
		return s7d_afbc_engine_fail(&v->afbc, -ETIMEDOUT, ENCP_INFO_READ);
	v->afbc_replay_afbc_changed = v->afbc.pending_valid;
	if (v->afbc_replay_afbc_changed) {
		ret = s7d_afbc_engine_replay_published(&v->afbc);
		if (ret)
			return s7d_afbc_engine_fail(&v->afbc, ret, AFBC_COMMAND);
	}
	v->afbc_replay_cookie = v->afbc_replay_pending_cookie;
	v->afbc_replay_pending_cookie = 0;
	v->afbc_replay_expected_sequence = sequence + 1;
	v->afbc_replay_apply_pending = true;
	return 0;
}

static void s7d_vpu_afbc_report_first(struct s7d_vpu *v, bool restart,
		bool vblank, enum s7d_rdma_result result,
		u32 before, u32 after, u32 fifo, u32 fifo2, u32 vd1, u32 arbiter)
{
	struct s7d_scanout *s = &v->scanout;
	unsigned long flags;
	u64 active_generation, pending_generation, active_header, pending_header;
	u64 active_afbc_generation, pending_afbc_generation;
	u32 active_fb, pending_fb;
	unsigned int phase;

	lockdep_assert_held(&v->frame_lock);
	spin_lock_irqsave(&s->lock, flags);
	active_generation = s->active.generation;
	pending_generation = s->pending.generation;
	active_afbc_generation = s->active.afbc.generation;
	pending_afbc_generation = s->pending.afbc.generation;
	active_header = s->active.afbc.plan.header_addr;
	pending_header = s->pending.afbc.plan.header_addr;
	active_fb = s->active.afbc.fb ? s->active.afbc.fb->base.id : 0;
	pending_fb = s->pending.afbc.fb ? s->pending.afbc.fb->base.id : 0;
	phase = s->phase;
	spin_unlock_irqrestore(&s->lock, flags);
	dev_err(v->dev, "AFBC first frame fault: %s vblank %u rdma %u ns %llu caller %pS owner %u active fb %u gen %llu afbc gen %llu header %#llx pending fb %u gen %llu afbc gen %llu header %#llx bound %u gen %llu header %#llx staged %u gen %llu header %#llx native before %#x after %#x fifo %#x fifo2 %#x vd1 %#x read0 %#x\n",
		restart ? "restart" : "sample", vblank, result,
		v->afbc.failed_ns, (void *)v->afbc.failed_caller, phase,
		active_fb, active_generation, active_afbc_generation, active_header,
		pending_fb, pending_generation, pending_afbc_generation, pending_header,
		v->afbc.bound_valid, v->afbc.generation, v->afbc.bound.header_addr,
		v->afbc.pending_valid, v->afbc.pending_generation,
		v->afbc.pending.header_addr, before, after, fifo, fifo2, vd1, arbiter);
}

static void s7d_vpu_frame_irq(struct s7d_vpu *v, bool vblank,
			      enum s7d_rdma_result result, u64 cookie)
{
	struct s7d_frame_state frame = {0};
	struct s7d_afbc_frame afbc_frame;
	const struct s7d_flip_latch *latch_ptr = NULL;
	struct s7d_flip_latch latch;
	unsigned long flags;
	u32 before, after, fifo, fifo2, vd1, vd2, arbiter;
	bool afbc_error;
	int ret;

	/* An older IRQ sample must not overtake the threaded RDMA completion. */
	spin_lock_irqsave(&v->frame_lock, flags);
	if (v->afbc_replay &&
	    (result == S7D_RDMA_APPLIED || result == S7D_RDMA_REPLAY) &&
	    cookie != v->afbc_replay_cookie)
		result = S7D_RDMA_NO_IRQ;
	afbc_error = v->afbc.phase == S7D_AFBC_ERROR;
	before = vpu_read(v, ENCP_INFO_READ);
	fifo = vpu_read(v, OSD1_CTRL + OSD_FIFO_OFFSET);
	fifo2 = vpu_read(v, OSD2_CTRL + OSD_FIFO_OFFSET);
	vd1 = vpu_read(v, VD1_GEN);
	vd2 = vpu_read(v, VD2_GEN);
	arbiter = vpu_read(v, ASYNC_STAT);
	after = vpu_read(v, ENCP_INFO_READ);
	frame.field = after >> 29;
	frame.idle = (before >> 29) == frame.field &&
		     !((fifo | fifo2) & OSD_FIFO_STATE) && !((vd1 | vd2) & VD1_BUSY) &&
		     (arbiter & ASYNC_IDLE);
	/* Any active video MIF requires a later field before framebuffer retirement. */
	frame.early = frame.idle && !((vd1 | vd2) & BIT(0)) &&
		s7d_vpu_afbc_window(before, after, vd1, vd2, v->flip_start, v->flip_end);
	afbc_frame = (struct s7d_afbc_frame) {
		.sequence = READ_ONCE(v->scanout.vblank_seq) + (vblank ? 1 : 0),
		.field = frame.field, .vblank = vblank,
		.idle = frame.idle,
		.window = frame.idle && s7d_vpu_afbc_window(before, after, vd1, vd2,
							v->flip_start, v->flip_end),
	};
	if (v->afbc_replay && !vblank &&
	    READ_ONCE(v->scanout.vblank_field) != frame.field)
		afbc_frame.sequence++;
	ret = s7d_vpu_afbc_sample(v, result, &frame, &afbc_frame, cookie);
	if (ret) {
		if (!afbc_error && v->afbc.phase == S7D_AFBC_ERROR)
			s7d_vpu_afbc_report_first(v, false, vblank, result,
				before, after, fifo, fifo2, vd1, arbiter);
		v->failed_reg = v->afbc.failed_reg;
		result = S7D_RDMA_FAULT;
	}
	if (!ret && v->afbc_replay && v->afbc_replay_latch_pending &&
	    (!v->afbc_replay_afbc_changed || v->afbc.swapped)) {
		latch = (struct s7d_flip_latch) {
			.cookie = v->afbc_replay_cookie,
			.vblank_sequence = v->afbc_replay_expected_sequence,
			.field = v->afbc_replay_applied_field,
			.ambiguous = afbc_frame.sequence != v->afbc_replay_expected_sequence ||
				frame.field != v->afbc_replay_applied_field,
		};
		latch_ptr = &latch;
		v->afbc_replay_latch_pending = false;
	}
	trace_s7d_frame_idle(before, after, fifo, fifo2, vd1, vd2, arbiter, vblank);
	s7d_crtc_irq_latched(v->crtc, vblank, result, &frame, latch_ptr);
	ret = s7d_vpu_afbc_restart(v, &afbc_frame);
	if (!ret)
		ret = s7d_vpu_afbc_replay_begin(v, &afbc_frame);
	if (!ret)
		ret = s7d_vpu_afbc_replay_replace(v, &afbc_frame, result, cookie);
	if (ret) {
		s7d_vpu_afbc_report_first(v, true, vblank, result,
			before, after, fifo, fifo2, vd1, arbiter);
		v->failed_reg = v->afbc.failed_reg;
		s7d_crtc_link_error(v->crtc, ret);
	}
	spin_unlock_irqrestore(&v->frame_lock, flags);
}

static irqreturn_t s7d_vpu_vsync_irq(int irq, void *data)
{
	struct s7d_vpu *v = data;

	s7d_vpu_trace_frame(v, 0, 0);
	/* VIU1 VSYNC is a GIC edge, not the legacy VENC_INTFLAG interrupt. */
	s7d_vpu_frame_irq(v, true, S7D_RDMA_NO_IRQ, 0);
	return IRQ_HANDLED;
}

static irqreturn_t s7d_vpu_rdma_irq(int irq, void *data)
{
	struct s7d_vpu *v = data;
	u64 cookie = 0;
	enum s7d_rdma_result result;

	result = s7d_rdma_loop_irq(&v->rdma, &cookie);

	if (result == S7D_RDMA_NO_IRQ)
		return IRQ_NONE;
	if (result == S7D_RDMA_NEEDS_DRAIN) {
		s7d_vpu_trace_frame(v, 1, result);
		return IRQ_WAKE_THREAD;
	}
	s7d_vpu_frame_irq(v, false, result, cookie);
	return IRQ_HANDLED;
}

static irqreturn_t s7d_vpu_rdma_thread(int irq, void *data)
{
	struct s7d_vpu *v = data;
	int ret = s7d_rdma_finish(&v->rdma);

	s7d_vpu_trace_frame(v, 2, ret);
	/* No framebuffer retirement/event until reset has drained DMA reads. */
	s7d_vpu_frame_irq(v, false, ret ? S7D_RDMA_FAULT : S7D_RDMA_COMPLETE, 0);
	return IRQ_HANDLED;
}

static int s7d_vpu_local_resets_init(struct s7d_vpu *v)
{
	struct device_node *node;
	int ret;

	node = of_get_compatible_child(v->dev->of_node, "amlogic,s7d-vpu-reset");
	if (!node)
		return 0;
	if (!of_device_is_available(node)) {
		of_node_put(node);
		return 0;
	}
	if (!IS_REACHABLE(CONFIG_RESET_S7D_VPU)) {
		of_node_put(node);
		return -EOPNOTSUPP;
	}
	ret = devm_s7d_vpu_reset_register(v->dev, v->regmap, node,
				       s7d_vpu_reset_access, v);
	of_node_put(node);
	if (ret)
		return ret;
	v->local_resets[0].id = "afbc-regs";
	v->local_resets[1].id = "afbc-logic";
	v->local_resets[2].id = "afbc-arbiter-regs";
	v->local_resets[3].id = "afbc-arbiter-logic";
	ret = devm_reset_control_bulk_get_exclusive(v->dev,
		ARRAY_SIZE(v->local_resets), v->local_resets);
	if (!ret)
		v->afbc_available = true;
	return ret;
}

int s7d_vpu_init(struct platform_device *pdev, struct s7d_vpu *v,
		 const struct s7d_vpu_link *link)
{
	struct device *dev = &pdev->dev;
	static const struct regmap_config map_config = {
		.reg_bits = 32, .val_bits = 32, .reg_stride = 4,
		.max_register = SZ_256K - 4,
		.cache_type = REGCACHE_NONE, .fast_io = true,
	};
	struct reset_control *rdma_reset;
	struct resource *res;
	int ret;

	if (!link || !link->quiesce || !link->prepare)
		return -EINVAL;
	v->link = *link;
	v->dev = dev;
	spin_lock_init(&v->frame_lock);
	INIT_DELAYED_WORK(&v->afbc_timeout, s7d_vpu_afbc_timeout);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vcbus");
	if (!res || resource_size(res) != SZ_256K)
		return -EINVAL;
	v->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(v->regs))
		return PTR_ERR(v->regs);
	v->regmap = devm_regmap_init_mmio(dev, v->regs, &map_config);
	if (IS_ERR(v->regmap))
		return PTR_ERR(v->regmap);
	s7d_afbc_engine_init(&v->afbc, &s7d_vpu_afbc_io, v);
	ret = s7d_vpu_local_resets_init(v);
	if (ret)
		return ret;
	v->xtal = devm_clk_get(dev, "xtal");
	if (IS_ERR(v->xtal))
		return dev_err_probe(dev, PTR_ERR(v->xtal), "missing reference clock\n");
	v->clocks[0].id = "vpu";
	v->clocks[1].id = "vapb";
	v->clocks[2].id = "intr";
	v->clocks[3].id = "encp";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(v->clocks), v->clocks);
	if (ret)
		return ret;
	v->pixel_route[PIXEL_PLL].id = "pixel-pll";
	v->pixel_route[PIXEL_SOURCE].id = "pixel-source";
	v->pixel_route[PIXEL_MUX].id = "pixel-mux";
	v->pixel_route[PIXEL_PARENT].id = "pixel-parent";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(v->pixel_route), v->pixel_route);
	if (ret)
		return ret;
	v->resets[0].id = "viu";
	v->resets[1].id = "venc";
	v->resets[2].id = "vencp";
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(v->resets), v->resets);
	if (ret)
		return ret;
	rdma_reset = devm_reset_control_get_exclusive(dev, "rdma");
	if (IS_ERR(rdma_reset))
		return PTR_ERR(rdma_reset);
	v->vsync_irq = platform_get_irq_byname(pdev, "vsync");
	if (v->vsync_irq < 0)
		return v->vsync_irq;
	v->rdma_irq = platform_get_irq_byname(pdev, "rdma");
	if (v->rdma_irq < 0)
		return v->rdma_irq;
	if (irq_get_trigger_type(v->vsync_irq) != IRQ_TYPE_EDGE_RISING ||
	    irq_get_trigger_type(v->rdma_irq) != IRQ_TYPE_EDGE_RISING)
		return -EINVAL;
	atomic_set(&v->vsync_enabled, 0);
	ret = devm_request_irq(dev, v->vsync_irq, s7d_vpu_vsync_irq,
			       IRQF_NO_AUTOEN, "s7d-vsync", v);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, v->rdma_irq, s7d_vpu_rdma_irq,
					s7d_vpu_rdma_thread,
					IRQF_NO_AUTOEN | IRQF_ONESHOT, "s7d-rdma", v);
	if (ret)
		return ret;
	ret = s7d_rdma_init(&v->rdma, dev, v->regs, rdma_reset);
	if (ret)
		return ret;
	s7d_scanout_init(&v->scanout, &v->rdma);
	return 0;
}

int s7d_vpu_fini(struct s7d_vpu *v)
{
	int ret;

	/* Probe failure before any Linux commit: no firmware register writes. */
	if (v->boot_held) {
		v->boot_held = false;
		s7d_vpu_release(v);
	}
	if (v->acquired)
		return -EBUSY;
	cancel_delayed_work_sync(&v->afbc_timeout);
	mask_irqs(v);
	ret = s7d_scanout_fini(&v->scanout);
	return ret ? ret : s7d_rdma_fini(&v->rdma);
}
