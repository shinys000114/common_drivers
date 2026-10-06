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
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "s7d-vpu.h"
#include "s7d-vpp.h"

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
#define VD1_BLEND		0x1dfb
#define VD2_BLEND		0x1dfc
#define VD1_PATH		0x1a0a
#define VD1_GEN			0x4800
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
#define CLOCK_ERROR_PPM		1000
#define AFBC_TOP			0x1a0f
#define AFBC_RAW			0x3a01
#define AFBC_COMMAND		0x3a05
#define AFBC_STATUS		0x3a06
#define AFBC_SURFACES		0x3a07
#define AFBC_FORMAT1		0x3a32
#define AFBC_UNPACK2		0x1abd

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

static int s7d_vpu_afbc_check_start(void *data, u64 sequence, u8 field)
{
	struct s7d_vpu *v = data;
	u32 before, after, fifo, fifo2, vd1, arbiter;

	lockdep_assert_held(&v->frame_lock);
	if (!v->acquired || !(vpu_read(v, ENCP_EN) & BIT(0)) ||
	    sequence != READ_ONCE(v->scanout.vblank_seq))
		return -EHOSTDOWN;
	before = vpu_read(v, ENCP_INFO_READ);
	fifo = vpu_read(v, OSD1_CTRL + OSD_FIFO_OFFSET);
	fifo2 = vpu_read(v, OSD2_CTRL + OSD_FIFO_OFFSET);
	vd1 = vpu_read(v, VD1_GEN);
	arbiter = vpu_read(v, ASYNC_STAT);
	after = vpu_read(v, ENCP_INFO_READ);
	if ((before >> 29) != field || (after >> 29) != field ||
	    ((fifo | fifo2) & OSD_FIFO_STATE) || (vd1 & (VD1_BUSY | BIT(0))) ||
	    !(arbiter & ASYNC_IDLE) ||
	    ((before >> 16) & 0x1fff) < v->flip_start ||
	    ((before >> 16) & 0x1fff) > ((after >> 16) & 0x1fff) ||
	    ((after >> 16) & 0x1fff) >= v->flip_end)
		return -ETIMEDOUT;
	return 0;
}

static const struct s7d_afbc_engine_io s7d_vpu_afbc_io = {
	.read = s7d_vpu_afbc_read,
	.write = s7d_vpu_afbc_write,
	.check_start = s7d_vpu_afbc_check_start,
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

static void s7d_vpu_afbc_admission(struct s7d_vpu *v)
{
	struct s7d_afbc_observation o;
	u32 format, unpack;
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
	if (!ret)
		ret = s7d_vpu_afbc_read(v, AFBC_FORMAT1, &format);
	if (!ret && (format & BIT(19)))
		ret = -EOPNOTSUPP;
	if (!ret)
		ret = s7d_vpu_afbc_read(v, AFBC_UNPACK2, &unpack);
	if (!ret && (unpack & (BIT(16) | BIT(28))))
		ret = -EOPNOTSUPP;
	v->afbc_admission_error = ret;
	v->afbc_ready = !ret;
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

static int s7d_vpu_afbc_stage(void *data,
			     const struct s7d_scanout_buffers *active,
			     const struct s7d_scanout_buffers *candidate,
			     u64 generation)
{
	struct s7d_vpu *v = data;
	struct s7d_afbc_engine *e = &v->afbc;

	lockdep_assert_held(&v->frame_lock);
	if (!candidate->afbc.fb && !active->afbc.fb)
		return v->afbc_enabled ? -EIO : 0;
	if (!v->afbc_enabled || !candidate->afbc.fb || !active->afbc.fb ||
	    !e->bound_valid || e->generation != active->generation ||
	    !s7d_afbc_same_layout(&e->bound, &active->afbc.plan) ||
	    e->bound.header_addr != active->afbc.plan.header_addr)
		return s7d_afbc_engine_fail(e, -EIO, AFBC_SURFACES);
	return s7d_afbc_engine_stage(e, &candidate->afbc.plan, generation);
}

static int s7d_vpu_afbc_cancel(void *data, u64 generation)
{
	struct s7d_vpu *v = data;

	lockdep_assert_held(&v->frame_lock);
	return v->afbc_enabled ?
		s7d_afbc_engine_cancel_stage(&v->afbc, generation) : 0;
}

static int s7d_vpu_submit(void *data, const struct s7d_crtc_state *state)
{
	struct s7d_vpu *v = data;
	unsigned long flags;
	int ret;

	s7d_scanout_flush_retired(&v->scanout);
	spin_lock_irqsave(&v->frame_lock, flags);
	ret = s7d_scanout_submit_staged(&v->scanout, &state->buffers, state->update,
		state->update_count, state->video_unchanged, s7d_vpu_afbc_stage,
		s7d_vpu_afbc_cancel, v);
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

	if (state->buffers.afbc.fb) {
		if (!v->afbc_available || !v->afbc_ready)
			return v->afbc_admission_error ?: -EOPNOTSUPP;
		ret = s7d_afbc_check_state(&state->buffers.afbc.plan);
		if (ret)
			return ret;
	}
	/* One unscaled RGB pixel per VPU cycle; protect this rate at prepare. */
	if (state->pixel_rate < 25175000 || state->pixel_rate > 594000000 ||
	    state->base.adjusted_mode.hdisplay > 4096 ||
	    state->base.adjusted_mode.vdisplay > 2160 ||
	    clk_get_rate(v->clocks[0].clk) < state->pixel_rate)
		return -ERANGE;
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
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(v->clocks), v->clocks);
	if (ret) {
		pm_runtime_put(v->dev);
		return ret;
	}
	v->acquired = true;
	ret = check_handoff(v);
	if (ret) {
		v->acquired = false;
		clk_bulk_disable_unprepare(ARRAY_SIZE(v->clocks), v->clocks);
		pm_runtime_put(v->dev);
		return ret;
	}
	s7d_vpu_afbc_admission(v);
	return 0;
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
	u32 value;
	unsigned int i;
	bool abort_prepare = false;
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
	if (v->afbc_enabled) {
		abort_prepare = s7d_afbc_engine_can_abort_prepare(&v->afbc);
		if (!abort_prepare) {
			ret = s7d_afbc_engine_stop_begin(&v->afbc);
			if (ret)
				return ret;
		}
	}
	ret = s7d_rdma_quiesce(&v->rdma);
	if (ret)
		return ret;
	if (v->afbc_enabled && !abort_prepare) {
		ret = s7d_vpu_afbc_stop_decode(v);
		if (ret)
			return ret;
	}
	if (!v->osd_draining) {
		for (i = 0; i < ARRAY_SIZE(osds); i++)
			v->osd_free_clk[i] = vpu_read(v, osds[i]) & OSD_FREE_CLK;
		v->osd_draining = true;
	}
	if (!v->vd1_draining) {
		v->vd1_free_clk = vpu_read(v, VD1_GEN) & VD1_FREE_CLK;
		v->vd1_draining = true;
	}
	/* Keep VD1 running until its state machines and READ0 have drained. */
	ret = vpu_update(v, VD1_GEN, VD1_FREE_CLK | BIT(30) | BIT(7) | BIT(0), VD1_FREE_CLK);
	if (ret)
		return ret;
	ret = readl_poll_timeout(v->regs + VD1_GEN * 4, value,
				!(value & VD1_BUSY), 10, 50000);
	if (ret) {
		v->failed_reg = VD1_GEN;
		return ret;
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
		} else {
			ret = s7d_afbc_engine_stop_finish(&v->afbc);
		}
		if (ret)
			return ret;
		ret = vpu_update(v, OSD_PATH, BIT(5), 0);
		if (!ret)
			ret = vpu_update(v, AFBC_UNPACK2, BIT(31), 0);
		if (!ret)
			ret = vpu_update(v, 0x1a4d, BIT(1), 0);
		if (ret)
			return ret;
		v->afbc_enabled = false;
	}
	ret = stop_encoders(v);
	if (ret)
		return ret;
	ret = vpu_update(v, VD1_GEN, VD1_FREE_CLK, v->vd1_free_clk);
	if (ret)
		return ret;
	v->vd1_draining = false;
	for (i = 0; i < ARRAY_SIZE(osds); i++) {
		ret = vpu_update(v, osds[i], OSD_FREE_CLK, v->osd_free_clk[i]);
		if (ret)
			return ret;
	}
	v->osd_draining = false;
	if (v->pixel_protected) {
		clk_rate_exclusive_put(v->clocks[PIXEL_CLK].clk);
		v->pixel_protected = false;
	}
	if (v->core_protected) {
		clk_rate_exclusive_put(v->clocks[0].clk);
		v->core_protected = false;
	}
	return 0;
}

static int s7d_vpu_stop(void *data)
{
	struct s7d_vpu *v = data;
	int ret = s7d_vpu_stop_hw(v);

	if (ret && v->afbc_enabled)
		return s7d_afbc_engine_fail(&v->afbc, ret, v->failed_reg);
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
	for (i = 0; i < S7D_OSD_SETUP_REG_COUNT; i++) {
		u32 reg = le32_to_cpu(state->osd.setup[i].reg);
		u32 expected = le32_to_cpu(state->osd.setup[i].value);
		u32 mask = U32_MAX;

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
	for (i = 0; i < S7D_POSTBLEND_REG_COUNT; i++) {
		ret = vpu_update(v, le32_to_cpu(state->postblend.regs[i].reg), U32_MAX,
				 le32_to_cpu(state->postblend.regs[i].value));
		if (ret)
			return ret;
	}
	v->vd1_enable = state->video.control.value & BIT(0);
	return vpu_update(v, state->video.control.reg,
			  state->video.control.mask | BIT(30),
			  state->video.control.value & ~BIT(0));
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
	/* stop() has released the PHY's exclusive PLL reference through HDMI. */
	ret = clk_set_rate(v->clocks[PIXEL_CLK].clk, state->pixel_rate);
	if (ret)
		return ret;
	if (!rate_matches(state->pixel_rate, clk_get_rate(v->clocks[PIXEL_CLK].clk)))
		return -ERANGE;
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
		u64 generation = s7d_scanout_pending_generation(&v->scanout);

		ret = s7d_vpu_afbc_resets(v);
		if (ret)
			return ret;
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
				v->afbc.generation, v->afbc.epoch);
		}
	}
	if (!ret)
		ret = vpu_update(v, VD1_GEN, BIT(0), v->vd1_enable ? BIT(0) : 0);
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
	clk_bulk_disable_unprepare(ARRAY_SIZE(v->clocks), v->clocks);
	pm_runtime_put(v->dev);
	v->acquired = false;
}

static void s7d_vpu_report_error(void *data, int error)
{
	struct s7d_vpu *v = data;

	if (!s7d_crtc_last_error(v->crtc))
		return;
	if (v->afbc.phase == S7D_AFBC_ERROR)
		dev_err(v->dev, "AFBC failed: %d reg %#x phase %u gen %llu epoch %llu command %#x last raw %#x status %#x top %#x surfaces %#x readback %d expected %#x observed %#x\n",
			v->afbc.last_error, v->afbc.failed_reg, v->afbc.failed_phase,
			v->afbc.failed_generation, v->afbc.failed_epoch,
			v->afbc_command_counter, v->afbc.observed.raw,
			v->afbc.observed.status, v->afbc.observed.top,
			v->afbc.observed.surfaces, v->afbc.failed_readback,
			v->afbc.failed_expected, v->afbc.failed_observed);
	dev_err(v->dev, "display stopped: %d, VCBUS word %#x; modeset required\n",
		error, v->failed_reg);
	drm_kms_helper_hotplug_event(v->crtc->dev);
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
			       const struct s7d_afbc_frame *afbc_frame)
{
	struct s7d_afbc_observation o;
	int ret;

	frame->afbc_gate = v->afbc_enabled;
	if (!v->afbc_enabled || v->afbc.phase == S7D_AFBC_PREPARED)
		return 0;
	if (result == S7D_RDMA_FAULT)
		return s7d_afbc_engine_fail(&v->afbc, -EIO, 0);
	ret = s7d_afbc_engine_read(&v->afbc, &o);
	if (!ret)
		ret = s7d_afbc_engine_sample(&v->afbc, afbc_frame, &o);
	if (ret < 0)
		return ret;
	if (result == S7D_RDMA_COMPLETE) {
		ret = s7d_afbc_engine_rdma_drained(&v->afbc,
				 s7d_scanout_pending_generation(&v->scanout));
		if (ret)
			return s7d_afbc_engine_fail(&v->afbc, ret, 0);
	}
	frame->afbc_completed_generation = v->afbc.completed_generation;
	frame->afbc_completed_epoch = v->afbc.completed_generation_epoch;
	frame->early = false;
	return 0;
}

static int s7d_vpu_afbc_restart(struct s7d_vpu *v,
				const struct s7d_afbc_frame *frame)
{
	u64 generation, previous_generation = v->afbc.generation;
	int ret;

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
		generation = s7d_scanout_pending_generation(&v->scanout);
		if (!generation || generation != v->afbc.generation)
			return s7d_afbc_engine_fail(&v->afbc, -EIO, 0);
		ret = s7d_scanout_afbc_started(&v->scanout, generation, v->afbc.epoch);
		if (ret)
			return s7d_afbc_engine_fail(&v->afbc, ret, 0);
	}
	return 0;
}

static void s7d_vpu_frame_irq(struct s7d_vpu *v, bool vblank,
			      enum s7d_rdma_result result)
{
	struct s7d_frame_state frame = {0};
	struct s7d_afbc_frame afbc_frame;
	unsigned long flags;
	u32 before, after, fifo, fifo2, vd1, arbiter;
	int ret;

	/* An older IRQ sample must not overtake the threaded RDMA completion. */
	spin_lock_irqsave(&v->frame_lock, flags);
	before = vpu_read(v, ENCP_INFO_READ);
	fifo = vpu_read(v, OSD1_CTRL + OSD_FIFO_OFFSET);
	fifo2 = vpu_read(v, OSD2_CTRL + OSD_FIFO_OFFSET);
	vd1 = vpu_read(v, VD1_GEN);
	arbiter = vpu_read(v, ASYNC_STAT);
	after = vpu_read(v, ENCP_INFO_READ);
	frame.field = after >> 29;
	frame.idle = (before >> 29) == frame.field &&
		     !((fifo | fifo2) & OSD_FIFO_STATE) && !(vd1 & VD1_BUSY) &&
		     (arbiter & ASYNC_IDLE);
	/* HOLD_FIFO_LINES prevents new OSD reads throughout this window. */
	frame.early = frame.idle && !(vd1 & BIT(0)) &&
		      ((before >> 16) & 0x1fff) >= v->flip_start &&
		      ((before >> 16) & 0x1fff) <= ((after >> 16) & 0x1fff) &&
		      ((after >> 16) & 0x1fff) < v->flip_end;
	afbc_frame = (struct s7d_afbc_frame) {
		.sequence = READ_ONCE(v->scanout.vblank_seq) + (vblank ? 1 : 0),
		.field = frame.field, .vblank = vblank,
		.idle = frame.idle, .window = frame.early,
	};
	ret = s7d_vpu_afbc_sample(v, result, &frame, &afbc_frame);
	if (ret) {
		v->failed_reg = v->afbc.failed_reg;
		result = S7D_RDMA_FAULT;
	}
	trace_s7d_frame_idle(before, after, fifo, fifo2, vd1, arbiter, vblank);
	s7d_crtc_irq(v->crtc, vblank, result, &frame);
	ret = s7d_vpu_afbc_restart(v, &afbc_frame);
	if (ret) {
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
	s7d_vpu_frame_irq(v, true, S7D_RDMA_NO_IRQ);
	return IRQ_HANDLED;
}

static irqreturn_t s7d_vpu_rdma_irq(int irq, void *data)
{
	struct s7d_vpu *v = data;
	enum s7d_rdma_result result = s7d_rdma_irq(&v->rdma);

	if (result == S7D_RDMA_NO_IRQ)
		return IRQ_NONE;
	if (result == S7D_RDMA_NEEDS_DRAIN) {
		s7d_vpu_trace_frame(v, 1, result);
		return IRQ_WAKE_THREAD;
	}
	s7d_vpu_frame_irq(v, false, result);
	return IRQ_HANDLED;
}

static irqreturn_t s7d_vpu_rdma_thread(int irq, void *data)
{
	struct s7d_vpu *v = data;
	int ret = s7d_rdma_finish(&v->rdma);

	s7d_vpu_trace_frame(v, 2, ret);
	/* No framebuffer retirement/event until reset has drained DMA reads. */
	s7d_vpu_frame_irq(v, false, ret ? S7D_RDMA_FAULT : S7D_RDMA_COMPLETE);
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
