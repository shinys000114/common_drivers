// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/amlogic/clk/s7d-hdmi-pll-rate.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
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
	ret = check_handoff(v);
	if (ret) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(v->clocks), v->clocks);
		pm_runtime_put(v->dev);
		return ret;
	}
	v->acquired = true;
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

static int s7d_vpu_stop(void *data)
{
	struct s7d_vpu *v = data;
	static const u32 osds[] = { OSD1_CTRL, OSD2_CTRL };
	u32 free_clk[ARRAY_SIZE(osds)];
	u32 value;
	unsigned int i;
	int ret;

	mask_irqs(v);
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
	for (i = 0; i < ARRAY_SIZE(osds); i++) {
		/*
		 * Keep the local OSD clock running until disable reaches OSD_ENABLE and DMA
		 * drains (S905X5M, VIU_OSD1_CTRL_STAT ENABLE_FREE_CLK).
		 */
		free_clk[i] = vpu_read(v, osds[i]) & OSD_FREE_CLK;
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
	ret = stop_encoders(v);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(osds); i++) {
		ret = vpu_update(v, osds[i], OSD_FREE_CLK, free_clk[i]);
		if (ret)
			return ret;
	}
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
	    ((vpu_read(v, VD1_BLEND) | vpu_read(v, VD2_BLEND)) & 0x0f0f))
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

		writel(expected, v->regs + reg * 4);
		/* Mixed status/config and Rev.B's documented alpha encoding. */
		if (reg == OSD1_CTRL)
			mask = OSD_CFG_SYNC | GENMASK(20, 12) | GENMASK(3, 0);
		else if (reg == 0x1a2d)
			mask = GENMASK(15, 0);
		else if (reg == 0x39ba)
			expected = 0x04020000;
		if ((vpu_read(v, reg) & mask) != (expected & mask)) {
			v->failed_reg = reg;
			return -EIO;
		}
	}
	return 0;
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
	/* RDMA done was acknowledged by prepare; clear stale GIC edge state. */
	ret = irq_set_irqchip_state(v->rdma_irq, IRQCHIP_STATE_PENDING, false);
	if (ret)
		return ret;
	return irq_set_irqchip_state(v->vsync_irq, IRQCHIP_STATE_PENDING, false);
}

static int s7d_vpu_start(void *data)
{
	struct s7d_vpu *v = data;

	if (!v->acquired || !v->touched)
		return -EIO;
	enable_irq(v->rdma_irq);
	v->rdma_enabled = true;
	return vpu_update(v, ENCP_EN, BIT(0), BIT(0));
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

static void s7d_vpu_frame_irq(struct s7d_vpu *v, bool vblank,
			      enum s7d_rdma_result result)
{
	struct s7d_frame_state frame;
	unsigned long flags;
	u32 before, after, fifo, arbiter;

	/* An older IRQ sample must not overtake the threaded RDMA completion. */
	spin_lock_irqsave(&v->frame_lock, flags);
	before = vpu_read(v, ENCP_INFO_READ);
	fifo = vpu_read(v, OSD1_CTRL + OSD_FIFO_OFFSET);
	arbiter = vpu_read(v, ASYNC_STAT);
	after = vpu_read(v, ENCP_INFO_READ);
	frame.field = after >> 29;
	frame.idle = (before >> 29) == frame.field &&
		     !(fifo & OSD_FIFO_STATE) && (arbiter & ASYNC_IDLE);
	trace_s7d_frame_idle(before, after, fifo, arbiter, vblank);
	s7d_crtc_irq(v->crtc, vblank, result, &frame);
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

int s7d_vpu_init(struct platform_device *pdev, struct s7d_vpu *v,
		 const struct s7d_vpu_link *link)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rdma_reset;
	struct resource *res;
	int ret;

	if (!link || !link->quiesce || !link->prepare)
		return -EINVAL;
	v->link = *link;
	v->dev = dev;
	spin_lock_init(&v->frame_lock);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vcbus");
	if (!res || resource_size(res) != SZ_64K)
		return -EINVAL;
	v->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(v->regs))
		return PTR_ERR(v->regs);
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
	mask_irqs(v);
	ret = s7d_scanout_fini(&v->scanout);
	return ret ? ret : s7d_rdma_fini(&v->rdma);
}
