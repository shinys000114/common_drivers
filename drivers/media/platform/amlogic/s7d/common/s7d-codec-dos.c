// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "s7d-codec-dos.h"

#define DOS_REG(word)		((word) * 4)
#define DOS_MMC_CTRL1		DOS_REG(0x0002)
#define DOS_MBOX1_MASK		DOS_REG(0x0076)
#define DOS_MPSR			DOS_REG(0x0301)
#define DOS_CPSR			DOS_REG(0x0321)
#define DOS_MPSR_RUN			BIT(0)
#define DOS_IMEM_DMA_CTRL		DOS_REG(0x0340)
#define DOS_LMEM_DMA_CTRL		DOS_REG(0x0350)
#define DOS_WRRSP_LMEM		DOS_REG(0x0353)
#define DOS_PIC_DC_MUX_CTRL	DOS_REG(0x098d)
#define DOS_PIC_DC_CTRL		DOS_REG(0x098e)
#define DOS_EXTIF_CFG1		DOS_REG(0x09e5)
#define DOS_VFIFO_CTRL		DOS_REG(0x0c44)
#define DOS_SW_RESET0		DOS_REG(0x3f00)
#define DOS_GCLK_EN0		DOS_REG(0x3f01)
#define DOS_MEM_PD_VDEC		DOS_REG(0x3f30)
#define DOS_MEM_PD_HCODEC		DOS_REG(0x3f32)
#define DOS_SW_RESET1		DOS_REG(0x3f07)
#define HCODEC_MMC_CTRL1		DOS_REG(0x1002)
#define HCODEC_MFDIN_SCMD	DOS_REG(0x100f)
#define HCODEC_MFDIN_DCFG	DOS_REG(0x100e)
#define HCODEC_MBOX2_MASK	DOS_REG(0x107a)
#define HCODEC_MPSR		DOS_REG(0x1301)
#define HCODEC_CPSR		DOS_REG(0x1321)
#define HCODEC_IMEM_DMA_CTRL	DOS_REG(0x1340)
#define HCODEC_VLC_VB_CONTROL	DOS_REG(0x1d16)
#define HCODEC_MFDIN_ENABLE	BIT(28)
#define HCODEC_LOCAL_RESET	(BIT(2) | BIT(6) | BIT(7) | BIT(8) | \
				 BIT(14) | BIT(16) | BIT(17))
#define DOS_MEM_PD_HEVC		DOS_REG(0x3f33)
#define DOS_SW_RESET3		DOS_REG(0x3f34)
#define DOS_GCLK_EN3		DOS_REG(0x3f35)
#define HEVC_AFIFO_CTRL		DOS_REG(0x3001)
#define HEVC_MBOX0_MASK		DOS_REG(0x3072)
#define HEVC_AXI_CTRL		DOS_REG(0x307c)
#define HEVC_AXI_STATUS		DOS_REG(0x307d)
#define HEVC_STREAM_CONTROL	DOS_REG(0x3101)
#define HEVC_STREAM_FIFO_CTL	DOS_REG(0x3107)
#define HEVC_MPSR		DOS_REG(0x3301)
#define HEVC_CPSR		DOS_REG(0x3321)
#define HEVC_IMEM_DMA_CTRL	DOS_REG(0x3340)
#define HEVC_LMEM_DMA_CTRL	DOS_REG(0x3350)
#define HEVC_WRRSP_LMEM		DOS_REG(0x3353)
#define HEVC_SAO_MMU_RESET	DOS_REG(0x3641)
#define HEVC_AXI_STOP		BIT(6)
#define HEVC_AXI_IDLE		(BIT(11) | BIT(15))
#define HEVC_AFIFO_BUSY		BIT(28)
#define HEVC_STREAM_FETCH	BIT(0)
#define HEVC_STREAM_ENDIAN	GENMASK(6, 4)
#define HEVC_STREAM_FIFO_HOLE	BIT(29)
#define HEVC_IQIT_RESET		BIT(14)
#define HEVC_CPU_RESET		(BIT(11) | BIT(12))
#define HEVC_LMEM_PRIORITY	GENMASK(27, 25)
#define HEVC_CORE_RESET		(BIT(3) | BIT(4) | BIT(8) | BIT(10) | \
				 BIT(11) | BIT(12) | BIT(13) | BIT(14) | \
				 BIT(15) | BIT(17) | BIT(18) | BIT(19) | \
				 BIT(24) | BIT(26))

#define DOS_VDEC_GATES		GENMASK(9, 0)
#define DOS_VDEC_COLD_RESET	0xfffffffcU
#define DOS_VDEC_RESET		GENMASK(9, 3)
#define DOS_CPU_RESET		(BIT(11) | BIT(12))
#define DOS_VFIFO_ENABLE		(BIT(1) | BIT(2))
#define DMC_REQ_CTRL		0x0000
#define DMC_CHAN_STS		(0x00cf * 4)
#define DMC_VDEC_PORT		BIT(8)
#define DMC_HCODEC_PORT		BIT(9)
#define DOS_JPEG_RATE		666666666UL
#define DOS_ENGINE_RATE		800000000UL
#define DMA_TIMEOUT_US		100000
#define RESPONSE_TIMEOUT_US	12500
#define DMC_TIMEOUT_US		100000
#define HEVC_RESPONSE_TIMEOUT_US	50000
#define HEVC_IDLE_TIMEOUT_US	100000

/*
 * VDEC, the HEVC core and HCODEC each have their own power domain, clock,
 * IRQ, reset and memory power registers, so each is leased on its own.
 * Two registers carry disjoint bits of several engines: DOS_GCLK_EN0
 * (VDEC [9:0], HCODEC [26:12]) and DMC_REQ_CTRL (VDEC bit 8, HCODEC bit 9).
 * They are changed only with regmap_update_bits() and read back masked,
 * so one engine never touches or checks another's bits.
 */
struct dos_engine {
	/* Serializes this engine's lease, its sequences and the state below. */
	struct mutex lock;
	unsigned int holders;
	/* Holders the engine takes at once. Fixed at create. */
	unsigned int slots;
	/*
	 * The holders take turns, each running the engine between run_begin
	 * and run_end; otherwise the one holder runs it. Fixed at create.
	 */
	bool shared;
	/* The runner and the leases waiting to run, in arrival order. */
	spinlock_t run_lock;
	/* The lease whose sequences the engine takes. */
	struct s7d_codec_lease *runner;
	struct list_head queue;
	wait_queue_head_t run_wait;
	bool local_access_ready;
	bool prepared;
	bool hevc_init_written;
	bool halted;
	bool started;
	bool faulted;
	/*
	 * The runner whose sequence faulted the engine, NULL when not faulted
	 * or when it faulted with no runner. Its holder keeps the engine.
	 */
	struct s7d_codec_lease *fault_lease;
	bool error_reported;
	bool release_failed;
	bool release_pin;
	/* The loader's tag of the image in the instruction memory, 0 for unknown. */
	u32 resident;
};

struct s7d_codec_dos {
	struct device *dev;
	struct module *owner;
	struct device *pd_dev;
	struct device *hevc_pd_dev;
	struct device *hcodec_pd_dev;
	struct clk *bus_clk;
	struct clk *vdec_clk;
	struct clk *hevc_clk;
	struct clk *hcodec_clk;
	struct reset_control *hevc_reset;
	struct regmap *map;
	struct regmap *dmc;
	void __iomem *base;
	resource_size_t start;
	resource_size_t size;
	/* Taken before every engine lock, in index order, to see all engines. */
	struct mutex lock;
	int irq;
	int hevc_irq;
	int hcodec_irq;
	struct dos_engine eng[S7D_CODEC_ENGINES];
	/* Set with every engine lock held; read under any of them. */
	bool closing;
};

static const struct regmap_config dos_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = SZ_64K - 4,
	.cache_type = REGCACHE_NONE,
	.fast_io = true,
};

static int dos_engine_check(enum s7d_codec_engine engine)
{
	if (engine == S7D_CODEC_ENGINE_HEVC || engine == S7D_CODEC_ENGINE_VDEC)
		return 0;
	if (engine == S7D_CODEC_ENGINE_HCODEC)
		return IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) ? 0 : -EOPNOTSUPP;
	return -EINVAL;
}

static int dos_vdec_engine_check(enum s7d_codec_engine engine)
{
	if (engine == S7D_CODEC_ENGINE_VDEC)
		return 0;
	if (engine == S7D_CODEC_ENGINE_HEVC || engine == S7D_CODEC_ENGINE_HCODEC)
		return -EOPNOTSUPP;
	return -EINVAL;
}

/* Callers have checked @engine. */
static struct dos_engine *dos_eng(struct s7d_codec_dos *dos,
				  enum s7d_codec_engine engine)
{
	return &dos->eng[engine];
}

/* The engine of a held lease, or an error for any other. */
static int dos_lease_engine(const struct s7d_codec_lease *lease,
			    struct dos_engine **out)
{
	int ret;

	if (!lease || !lease->dos || !lease->held)
		return -EINVAL;
	ret = dos_engine_check(lease->engine);
	if (!ret)
		*out = dos_eng(lease->dos, lease->engine);
	return ret;
}

static bool dos_runner_is(struct dos_engine *e, const struct s7d_codec_lease *lease)
{
	bool runs;

	spin_lock(&e->run_lock);
	runs = e->runner == lease;
	spin_unlock(&e->run_lock);
	return runs;
}

/*
 * Under the engine lock: @lease runs the engine now. Only the runner clears
 * the runner, so the answer holds for the runner's own call.
 */
static bool dos_runs(struct dos_engine *e, const struct s7d_codec_lease *lease)
{
	lockdep_assert_held(&e->lock);
	return lease->held && e->holders && dos_runner_is(e, lease);
}

/*
 * Under the engine lock, in a sequence of the runner, or of the last holder
 * at release: the engine's state is unknown. Only the runner's run may have
 * left it so; the other holders' runs ended halted.
 */
static void dos_fault_locked(struct dos_engine *e)
{
	lockdep_assert_held(&e->lock);
	e->faulted = true;
	spin_lock(&e->run_lock);
	e->fault_lease = e->runner;
	spin_unlock(&e->run_lock);
}

/* Under the engine lock: another holder's run faulted the engine, not @lease's. */
static bool dos_faulted_by_other(struct dos_engine *e, const struct s7d_codec_lease *lease)
{
	lockdep_assert_held(&e->lock);
	return e->faulted && e->fault_lease && e->fault_lease != lease;
}

static void dos_set_runner(struct dos_engine *e, struct s7d_codec_lease *lease)
{
	spin_lock(&e->run_lock);
	e->runner = lease;
	spin_unlock(&e->run_lock);
}

static void dos_lock_all(struct s7d_codec_dos *dos)
{
	unsigned int i;

	mutex_lock(&dos->lock);
	for (i = 0; i < ARRAY_SIZE(dos->eng); i++)
		mutex_lock_nested(&dos->eng[i].lock, i);
}

static void dos_unlock_all(struct s7d_codec_dos *dos)
{
	unsigned int i;

	for (i = ARRAY_SIZE(dos->eng); i; i--)
		mutex_unlock(&dos->eng[i - 1].lock);
	mutex_unlock(&dos->lock);
}

static struct clk *dos_engine_clk(struct s7d_codec_dos *dos,
				 enum s7d_codec_engine engine)
{
	if (engine == S7D_CODEC_ENGINE_HCODEC)
		return dos->hcodec_clk;
	return engine == S7D_CODEC_ENGINE_HEVC ? dos->hevc_clk : dos->vdec_clk;
}

static struct device *dos_engine_pd(struct s7d_codec_dos *dos,
				   enum s7d_codec_engine engine)
{
	if (engine == S7D_CODEC_ENGINE_HCODEC)
		return dos->hcodec_pd_dev;
	return engine == S7D_CODEC_ENGINE_HEVC ? dos->hevc_pd_dev : dos->pd_dev;
}

static int dos_check(struct regmap *map, unsigned int reg, u32 mask, u32 want)
{
	u32 val;
	int ret;

	ret = regmap_read(map, reg, &val);
	if (ret)
		return ret;
	return (val & mask) == want ? 0 : -EIO;
}

static int dos_write(struct regmap *map, unsigned int reg, u32 val)
{
	int ret = regmap_write(map, reg, val);

	return ret ? ret : dos_check(map, reg, U32_MAX, val);
}

/*
 * Only bit 0 of an AMRISC MPSR runs the CPU. The other bits carry live
 * status (bits 2 and 6 were seen while a decode ran), which the vendor
 * amvdec_start()/amvdec_stop() never read back, so check the run bit only.
 */
static int dos_write_mpsr(struct regmap *map, unsigned int reg, u32 run)
{
	int ret = regmap_write(map, reg, run);

	return ret ? ret : dos_check(map, reg, DOS_MPSR_RUN, run);
}

static int dos_update(struct regmap *map, unsigned int reg, u32 mask, u32 val)
{
	int ret = regmap_update_bits(map, reg, mask, val);

	return ret ? ret : dos_check(map, reg, mask, val);
}

/* The HEVC sequences report their first failure per lease. */
static int dos_report_error(struct s7d_codec_dos *dos, const char *stage,
			    int result, int error)
{
	struct dos_engine *e = dos_eng(dos, S7D_CODEC_ENGINE_HEVC);

	if (error && !e->error_reported) {
		e->error_reported = true;
		dev_err(dos->dev, "HEVC DOS %s result=%d error=%d\n", stage, result, error);
	}
	return error;
}

static int dos_report_io(struct s7d_codec_dos *dos, const char *stage,
			 unsigned int reg, u32 mask, u32 want,
			 u32 observed, bool observed_valid, int error)
{
	struct dos_engine *e = dos_eng(dos, S7D_CODEC_ENGINE_HEVC);

	if (error && !e->error_reported) {
		e->error_reported = true;
		dev_err(dos->dev,
			"HEVC DOS %s reg=%#x mask=%#x want=%#x observed=%#x valid=%u error=%d\n",
			stage, reg, mask, want,
			observed, observed_valid, error);
	}
	return error;
}

static int dos_hevc_check(struct s7d_codec_dos *dos, unsigned int reg,
			  u32 mask, u32 want)
{
	u32 val = 0;
	int ret;

	ret = regmap_read(dos->map, reg, &val);
	if (ret)
		return dos_report_io(dos, "read", reg, mask, want, val, false, ret);
	ret = (val & mask) == want ? 0 : -EIO;
	return dos_report_io(dos, "check", reg, mask, want, val, true, ret);
}

static int dos_hevc_write_raw(struct s7d_codec_dos *dos, unsigned int reg, u32 val)
{
	int ret = regmap_write(dos->map, reg, val);

	return dos_report_io(dos, "write", reg, U32_MAX, val, 0, false, ret);
}

static int dos_hevc_write(struct s7d_codec_dos *dos, unsigned int reg, u32 val)
{
	int ret = dos_hevc_write_raw(dos, reg, val);

	return ret ? ret : dos_hevc_check(dos, reg, U32_MAX, val);
}

static int dos_hevc_write_mpsr(struct s7d_codec_dos *dos, u32 run)
{
	int ret = dos_hevc_write_raw(dos, HEVC_MPSR, run);

	return ret ? ret : dos_hevc_check(dos, HEVC_MPSR, DOS_MPSR_RUN, run);
}

static int dos_hevc_update(struct s7d_codec_dos *dos, unsigned int reg,
			   u32 mask, u32 val)
{
	int ret = regmap_update_bits(dos->map, reg, mask, val);

	return ret ? dos_report_io(dos, "update", reg, mask, val, 0, false, ret) :
		dos_hevc_check(dos, reg, mask, val);
}

static int dos_hevc_read(struct s7d_codec_dos *dos, unsigned int reg, u32 *val)
{
	int ret = regmap_read(dos->map, reg, val);

	return dos_report_io(dos, "read", reg, 0, 0, 0, false, ret);
}

/* DOS_SW_RESET0 resets VDEC only; the vendor pulses it while HEVC runs. */
static int dos_reset(struct s7d_codec_dos *dos, u32 mask)
{
	u32 val;
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(dos->map, DOS_SW_RESET0, &val);
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(dos->map, DOS_SW_RESET0, mask, mask);
	if (ret)
		return ret;
	usleep_range(20, 30);
	ret = regmap_update_bits(dos->map, DOS_SW_RESET0, mask, 0);
	if (ret)
		return ret;
	usleep_range(10, 20);
	for (i = 0; i < 3; i++) {
		ret = regmap_read(dos->map, DOS_SW_RESET0, &val);
		if (ret)
			return ret;
	}
	return val & mask ? -EIO : 0;
}

static int dos_hcodec_block_locked(struct s7d_codec_dos *dos)
{
	u32 val;
	int ret;

	/* HCODEC's bit of the shared DMC_REQ_CTRL, changed and checked masked. */
	ret = dos_update(dos->dmc, DMC_REQ_CTRL, DMC_HCODEC_PORT, 0);
	if (ret)
		return ret;
	return regmap_read_poll_timeout(dos->dmc, DMC_CHAN_STS, val,
					val & DMC_HCODEC_PORT, 10, DMC_TIMEOUT_US);
}

static int dos_hcodec_reset(struct s7d_codec_dos *dos, u32 mask)
{
	u32 val;
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(dos->map, DOS_SW_RESET1, &val);
		if (ret)
			return ret;
	}
	ret = dos_update(dos->map, DOS_SW_RESET1, mask, mask);
	if (ret)
		return ret;
	ret = dos_update(dos->map, DOS_SW_RESET1, mask, 0);
	if (ret)
		return ret;
	for (i = 0; i < 3; i++) {
		ret = regmap_read(dos->map, DOS_SW_RESET1, &val);
		if (ret)
			return ret;
	}
	return val & mask ? -EIO : 0;
}

static int dos_hcodec_prepare_access_locked(struct s7d_codec_dos *dos)
{
	int ret;

	ret = dos_hcodec_block_locked(dos);
	if (ret)
		return ret;
	/* HCODEC's gates in the shared DOS_GCLK_EN0. */
	ret = dos_update(dos->map, DOS_GCLK_EN0, GENMASK(26, 12),
			 GENMASK(26, 12));
	if (ret)
		return ret;
	return dos_write(dos->map, DOS_MEM_PD_HCODEC, 0);
}

static int dos_hevc_prepare_access_locked(struct s7d_codec_dos *dos)
{
	int ret;

	usleep_range(20, 30);
	ret = dos_hevc_write_raw(dos, DOS_GCLK_EN3, U32_MAX);
	if (ret)
		return ret;
	ret = dos_hevc_write(dos, DOS_MEM_PD_HEVC, 0);
	if (ret)
		return ret;
	ret = dos_hevc_write_raw(dos, DOS_SW_RESET3, U32_MAX);
	if (ret)
		return ret;
	usleep_range(20, 30);
	ret = dos_hevc_write_raw(dos, DOS_SW_RESET3, 0);
	if (ret)
		return ret;
	usleep_range(10, 20);
	ret = dos_hevc_check(dos, DOS_SW_RESET3, U32_MAX, 0);
	if (ret)
		return ret;
	return dos_hevc_write(dos, DOS_MEM_PD_HEVC, 0);
}

static int dos_prepare_access_locked(struct s7d_codec_dos *dos,
				     enum s7d_codec_engine engine)
{
	struct dos_engine *e = dos_eng(dos, engine);
	int ret;

	if (e->local_access_ready)
		return 0;
	if (e->faulted || e->release_failed || dos->closing)
		return -EIO;
	if (engine == S7D_CODEC_ENGINE_HCODEC) {
		ret = dos_hcodec_prepare_access_locked(dos);
		if (ret)
			goto fault;
		e->local_access_ready = true;
		return 0;
	}
	if (engine == S7D_CODEC_ENGINE_HEVC) {
		ret = dos_hevc_prepare_access_locked(dos);
		if (ret)
			goto fault;
		e->local_access_ready = true;
		return 0;
	}
	usleep_range(20, 30);
	/* VDEC's gates in the shared DOS_GCLK_EN0. */
	ret = dos_update(dos->map, DOS_GCLK_EN0, DOS_VDEC_GATES, DOS_VDEC_GATES);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, DOS_MEM_PD_VDEC, 0);
	if (ret)
		goto fault;
	ret = regmap_write(dos->map, DOS_SW_RESET0, DOS_VDEC_COLD_RESET);
	if (ret)
		goto fault;
	usleep_range(20, 30);
	ret = regmap_write(dos->map, DOS_SW_RESET0, 0);
	if (ret)
		goto fault;
	usleep_range(10, 20);
	ret = dos_check(dos->map, DOS_SW_RESET0, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, DOS_MEM_PD_VDEC, 0);
	if (ret)
		goto fault;
	e->local_access_ready = true;
	return 0;
fault:
	dos_fault_locked(e);
	e->halted = false;
	return ret;
}

static int dos_non_mmu(struct s7d_codec_dos *dos)
{
	int ret;

	ret = dos_update(dos->map, DOS_MMC_CTRL1, BIT(3), 0);
	if (ret)
		return ret;
	ret = dos_update(dos->map, DOS_PIC_DC_MUX_CTRL, BIT(31), 0);
	if (ret)
		return ret;
	return dos_write(dos->map, DOS_EXTIF_CFG1, 0);
}

static int dos_hcodec_stopped_locked(struct s7d_codec_dos *dos)
{
	int ret;

	ret = dos_check(dos->map, HCODEC_MBOX2_MASK, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_check(dos->map, HCODEC_MPSR, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_check(dos->map, HCODEC_CPSR, U32_MAX, 0);
	if (ret)
		return ret;
	return dos_check(dos->map, HCODEC_IMEM_DMA_CTRL, BIT(15), 0);
}

static int dos_hcodec_halt_locked(struct s7d_codec_dos *dos)
{
	struct dos_engine *e = dos_eng(dos, S7D_CODEC_ENGINE_HCODEC);
	u32 val;
	int ret;

	e->halted = false;
	ret = dos_hcodec_block_locked(dos);
	if (ret)
		return ret;
	ret = dos_write(dos->map, HCODEC_MBOX2_MASK, 0);
	if (ret)
		return ret;
	ret = dos_write_mpsr(dos->map, HCODEC_MPSR, 0);
	if (ret)
		return ret;
	ret = dos_write(dos->map, HCODEC_CPSR, 0);
	if (ret)
		return ret;
	ret = dos_update(dos->map, HCODEC_MFDIN_SCMD, HCODEC_MFDIN_ENABLE, 0);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(dos->map, HCODEC_IMEM_DMA_CTRL, val,
				      !(val & BIT(15)), 10, DMA_TIMEOUT_US);
	if (ret)
		return ret;
	ret = dos_hcodec_reset(dos, HCODEC_LOCAL_RESET | DOS_CPU_RESET);
	if (ret)
		return ret;
	ret = dos_hcodec_stopped_locked(dos);
	if (ret)
		return ret;
	ret = dos_check(dos->map, HCODEC_MFDIN_SCMD, HCODEC_MFDIN_ENABLE, 0);
	if (ret)
		return ret;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_HCODEC_PORT, 0);
	if (ret)
		return ret;
	ret = dos_check(dos->dmc, DMC_CHAN_STS, DMC_HCODEC_PORT,
			DMC_HCODEC_PORT);
	if (ret)
		return ret;
	e->halted = true;
	e->prepared = false;
	e->started = false;
	e->faulted = false;
	e->fault_lease = NULL;
	return 0;
}

static int dos_hevc_halt_locked(struct s7d_codec_dos *dos)
{
	struct dos_engine *e = dos_eng(dos, S7D_CODEC_ENGINE_HEVC);
	u32 val = 0;
	int ret;

	e->halted = false;
	ret = dos_hevc_write(dos, HEVC_MBOX0_MASK, 0);
	if (ret)
		return ret;
	ret = dos_hevc_write_mpsr(dos, 0);
	if (ret)
		return ret;
	ret = dos_hevc_write(dos, HEVC_CPSR, 0);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(dos->map, HEVC_IMEM_DMA_CTRL, val,
				      !(val & BIT(15)), 10, DMA_TIMEOUT_US);
	if (ret)
		return dos_report_io(dos, "poll", HEVC_IMEM_DMA_CTRL,
				     BIT(15), 0, val,
				     ret == -ETIMEDOUT, ret);
	ret = regmap_read_poll_timeout(dos->map, HEVC_LMEM_DMA_CTRL, val,
				      !(val & BIT(15)), 10, DMA_TIMEOUT_US);
	if (ret)
		return dos_report_io(dos, "poll", HEVC_LMEM_DMA_CTRL,
				     BIT(15), 0, val,
				     ret == -ETIMEDOUT, ret);
	ret = regmap_read_poll_timeout(dos->map, HEVC_WRRSP_LMEM, val,
				      !(val & GENMASK(11, 0)), 10,
				      HEVC_RESPONSE_TIMEOUT_US);
	if (ret)
		return dos_report_io(dos, "poll", HEVC_WRRSP_LMEM,
				     GENMASK(11, 0), 0, val,
				     ret == -ETIMEDOUT, ret);
	ret = dos_hevc_update(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH, 0);
	if (ret)
		return ret;
	ret = dos_hevc_update(dos, HEVC_AXI_CTRL, HEVC_AXI_STOP, HEVC_AXI_STOP);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(dos->map, HEVC_AXI_STATUS, val,
				      (val & HEVC_AXI_IDLE) == HEVC_AXI_IDLE,
				      10, HEVC_IDLE_TIMEOUT_US);
	if (ret)
		return dos_report_io(dos, "poll", HEVC_AXI_STATUS,
				     HEVC_AXI_IDLE, HEVC_AXI_IDLE, val,
				     ret == -ETIMEDOUT, ret);
	ret = regmap_read_poll_timeout(dos->map, HEVC_AFIFO_CTRL, val,
				      !(val & HEVC_AFIFO_BUSY), 10,
				      HEVC_IDLE_TIMEOUT_US);
	if (ret)
		return dos_report_io(dos, "poll", HEVC_AFIFO_CTRL,
				     HEVC_AFIFO_BUSY, 0, val,
				     ret == -ETIMEDOUT, ret);
	ret = dos_hevc_update(dos, HEVC_SAO_MMU_RESET, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = dos_hevc_update(dos, DOS_SW_RESET3, HEVC_CORE_RESET, HEVC_CORE_RESET);
	if (ret)
		return ret;
	usleep_range(20, 30);
	ret = dos_hevc_update(dos, DOS_SW_RESET3, HEVC_CORE_RESET, 0);
	if (ret)
		return ret;
	usleep_range(10, 20);
	ret = regmap_read_poll_timeout(dos->map, HEVC_WRRSP_LMEM, val,
				      !(val & GENMASK(11, 0)), 10,
				      HEVC_RESPONSE_TIMEOUT_US);
	if (ret)
		return dos_report_io(dos, "poll", HEVC_WRRSP_LMEM,
				     GENMASK(11, 0), 0, val,
				     ret == -ETIMEDOUT, ret);
	ret = dos_hevc_update(dos, HEVC_SAO_MMU_RESET, BIT(0), 0);
	if (ret)
		return ret;
	ret = reset_control_assert(dos->hevc_reset);
	if (ret)
		return dos_report_error(dos, "reset-assert", ret, ret);
	ret = reset_control_status(dos->hevc_reset);
	if (ret != 1)
		return dos_report_error(dos, "reset-assert-status", ret,
					ret < 0 ? ret : -EIO);
	usleep_range(20, 30);
	ret = reset_control_deassert(dos->hevc_reset);
	if (ret)
		return dos_report_error(dos, "reset-deassert", ret, ret);
	ret = reset_control_status(dos->hevc_reset);
	if (ret)
		return dos_report_error(dos, "reset-deassert-status", ret,
					ret < 0 ? ret : -EIO);
	ret = dos_hevc_check(dos, HEVC_MBOX0_MASK, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_MPSR, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_CPSR, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH, 0);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AXI_CTRL, HEVC_AXI_STOP, HEVC_AXI_STOP);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AXI_STATUS, HEVC_AXI_IDLE, HEVC_AXI_IDLE);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AFIFO_CTRL, HEVC_AFIFO_BUSY, 0);
	if (ret)
		return ret;
	e->halted = true;
	e->prepared = false;
	e->hevc_init_written = false;
	e->started = false;
	e->faulted = false;
	e->fault_lease = NULL;
	return 0;
}

static int dos_hevc_exec_check_locked(struct s7d_codec_dos *dos,
				      const struct s7d_codec_lease *lease)
{
	struct dos_engine *e = dos_eng(dos, S7D_CODEC_ENGINE_HEVC);

	if (!dos_runs(e, lease) || !e->local_access_ready || !e->prepared ||
	    e->halted || e->started || e->faulted || e->release_failed ||
	    dos->closing)
		return -EBUSY;
	return 0;
}

static int dos_hevc_stopped_cpu_locked(struct s7d_codec_dos *dos)
{
	int ret;

	ret = dos_hevc_check(dos, HEVC_MPSR, U32_MAX, 0);
	if (ret)
		return ret;
	return dos_hevc_check(dos, HEVC_CPSR, U32_MAX, 0);
}

static int dos_hevc_dma_idle_locked(struct s7d_codec_dos *dos)
{
	int ret;

	ret = dos_hevc_check(dos, HEVC_IMEM_DMA_CTRL, BIT(15), 0);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_LMEM_DMA_CTRL, BIT(15), 0);
	if (ret)
		return ret;
	return dos_hevc_check(dos, HEVC_WRRSP_LMEM, GENMASK(11, 0), 0);
}

static int dos_hevc_port_enable_locked(struct s7d_codec_dos *dos)
{
	int ret;

	ret = dos_hevc_check(dos, HEVC_MBOX0_MASK, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_hevc_stopped_cpu_locked(dos);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH, 0);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, DOS_SW_RESET3, HEVC_CORE_RESET, 0);
	if (ret)
		return ret;
	ret = dos_hevc_dma_idle_locked(dos);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AXI_CTRL, HEVC_AXI_STOP, HEVC_AXI_STOP);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AXI_STATUS, HEVC_AXI_IDLE, HEVC_AXI_IDLE);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AFIFO_CTRL, HEVC_AFIFO_BUSY, 0);
	if (ret)
		return ret;
	dos_eng(dos, S7D_CODEC_ENGINE_HEVC)->halted = false;
	return dos_hevc_update(dos, HEVC_AXI_CTRL, HEVC_AXI_STOP, 0);
}

static int dos_hevc_cpu_reset_locked(struct s7d_codec_dos *dos)
{
	u32 val;
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = dos_hevc_read(dos, DOS_SW_RESET3, &val);
		if (ret)
			return ret;
	}
	ret = dos_hevc_write_raw(dos, DOS_SW_RESET3, HEVC_CPU_RESET);
	if (ret)
		return ret;
	ret = dos_hevc_write_raw(dos, DOS_SW_RESET3, 0);
	if (ret)
		return ret;
	for (i = 0; i < 3; i++) {
		ret = dos_hevc_read(dos, DOS_SW_RESET3, &val);
		if (ret)
			return ret;
	}
	return dos_report_io(dos, "cpu-reset", DOS_SW_RESET3,
			     HEVC_CPU_RESET | HEVC_IQIT_RESET, 0, val, true,
			     val & (HEVC_CPU_RESET | HEVC_IQIT_RESET) ? -EIO : 0);
}

static int dos_hevc_start_locked(struct s7d_codec_dos *dos)
{
	struct dos_engine *e = dos_eng(dos, S7D_CODEC_ENGINE_HEVC);
	int ret;

	if (!e->hevc_init_written)
		return -EBUSY;
	ret = dos_hevc_check(dos, HEVC_MBOX0_MASK, U32_MAX, 1);
	if (ret)
		return ret;
	ret = dos_hevc_stopped_cpu_locked(dos);
	if (ret)
		return ret;
	ret = dos_hevc_dma_idle_locked(dos);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_STREAM_CONTROL,
			HEVC_STREAM_FETCH | HEVC_STREAM_ENDIAN,
			HEVC_STREAM_FETCH | HEVC_STREAM_ENDIAN);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AXI_CTRL, HEVC_AXI_STOP, 0);
	if (ret)
		return ret;
	e->started = true;
	ret = dos_hevc_cpu_reset_locked(dos);
	if (ret)
		return ret;
	ret = dos_hevc_update(dos, HEVC_WRRSP_LMEM, HEVC_LMEM_PRIORITY,
			 HEVC_LMEM_PRIORITY);
	if (ret)
		return ret;
	return dos_hevc_write_mpsr(dos, 1);
}

static int dos_halt_locked(struct s7d_codec_dos *dos,
			   enum s7d_codec_engine engine)
{
	struct dos_engine *e = dos_eng(dos, engine);
	u32 val;
	int ret;

	if (engine == S7D_CODEC_ENGINE_HCODEC)
		return dos_hcodec_halt_locked(dos);
	if (engine == S7D_CODEC_ENGINE_HEVC)
		return dos_hevc_halt_locked(dos);

	e->halted = false;
	ret = dos_write(dos->map, DOS_MBOX1_MASK, 0);
	if (ret)
		return ret;
	ret = dos_write_mpsr(dos->map, DOS_MPSR, 0);
	if (ret)
		return ret;
	ret = dos_write(dos->map, DOS_CPSR, 0);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(dos->map, DOS_IMEM_DMA_CTRL, val,
				      !(val & BIT(15)), 10, DMA_TIMEOUT_US);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(dos->map, DOS_LMEM_DMA_CTRL, val,
				      !(val & BIT(15)), 10, DMA_TIMEOUT_US);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(dos->map, DOS_WRRSP_LMEM, val,
				      !(val & GENMASK(11, 0)), 10,
				      RESPONSE_TIMEOUT_US);
	if (ret)
		return ret;
	/* VDEC's bit of the shared DMC_REQ_CTRL, changed and checked masked. */
	ret = dos_update(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, 0);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(dos->dmc, DMC_CHAN_STS, val,
				      val & DMC_VDEC_PORT, 10, DMC_TIMEOUT_US);
	if (ret)
		return ret;
	ret = dos_reset(dos, DOS_VDEC_RESET);
	if (ret)
		return ret;
	ret = dos_reset(dos, DOS_CPU_RESET);
	if (ret)
		return ret;
	ret = dos_non_mmu(dos);
	if (ret)
		return ret;
	ret = dos_check(dos->map, DOS_MBOX1_MASK, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_check(dos->map, DOS_MPSR, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_check(dos->map, DOS_CPSR, U32_MAX, 0);
	if (ret)
		return ret;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, 0);
	if (ret)
		return ret;
	e->halted = true;
	e->prepared = false;
	e->started = false;
	e->faulted = false;
	e->fault_lease = NULL;
	return 0;
}

static int dos_pic_reset(struct s7d_codec_dos *dos)
{
	u32 mask = 0xbfU << 24;
	int ret;

	ret = regmap_update_bits(dos->map, DOS_PIC_DC_CTRL, mask, mask);
	if (ret)
		return ret;
	return dos_update(dos->map, DOS_PIC_DC_CTRL, mask, 0);
}

struct s7d_codec_dos *
s7d_codec_dos_create(struct platform_device *pdev, struct module *owner)
{
	struct s7d_codec_dos *dos;
	struct resource *res;
	unsigned int i;
	int ret;

	if (!pdev || !of_device_is_compatible(pdev->dev.of_node,
					      "amlogic,s7d-vdec"))
		return ERR_PTR(-ENODEV);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dos");
	if (!res || resource_size(res) != SZ_64K)
		return ERR_PTR(-EINVAL);
	dos = kzalloc(sizeof(*dos), GFP_KERNEL);
	if (!dos)
		return ERR_PTR(-ENOMEM);
	dos->dev = get_device(&pdev->dev);
	dos->owner = owner;
	mutex_init(&dos->lock);
	for (i = 0; i < ARRAY_SIZE(dos->eng); i++) {
		mutex_init(&dos->eng[i].lock);
		spin_lock_init(&dos->eng[i].run_lock);
		INIT_LIST_HEAD(&dos->eng[i].queue);
		init_waitqueue_head(&dos->eng[i].run_wait);
	}
	for (i = 0; i < ARRAY_SIZE(dos->eng); i++)
		dos->eng[i].slots = 1;
	/* HEVC, VP9 and AV1 sessions take turns on the HEVC core picture by picture. */
	dos->eng[S7D_CODEC_ENGINE_HEVC].shared = true;
	dos->eng[S7D_CODEC_ENGINE_HEVC].slots = S7D_CODEC_HEVC_SESSIONS;
	/* H.264 sessions take turns on VDEC the same way. */
	dos->eng[S7D_CODEC_ENGINE_VDEC].shared = true;
	dos->eng[S7D_CODEC_ENGINE_VDEC].slots = S7D_CODEC_VDEC_SESSIONS;
	dos->irq = platform_get_irq_byname(pdev, "mailbox_1");
	if (dos->irq < 0) {
		ret = dos->irq;
		goto err_device;
	}
	dos->dmc = syscon_regmap_lookup_by_phandle(dos->dev->of_node,
						 "amlogic,dmc-syscon");
	if (IS_ERR(dos->dmc)) {
		ret = PTR_ERR(dos->dmc);
		goto err_device;
	}
	dos->bus_clk = clk_get(dos->dev, "dos");
	if (IS_ERR(dos->bus_clk)) {
		ret = PTR_ERR(dos->bus_clk);
		goto err_device;
	}
	dos->vdec_clk = clk_get(dos->dev, "vdec");
	if (IS_ERR(dos->vdec_clk)) {
		ret = PTR_ERR(dos->vdec_clk);
		goto err_bus_clk;
	}
	dos->start = res->start;
	dos->size = resource_size(res);
	if (!request_mem_region(dos->start, dos->size, dev_name(dos->dev))) {
		ret = -EBUSY;
		goto err_vdec_clk;
	}
	dos->base = ioremap(dos->start, dos->size);
	if (!dos->base) {
		ret = -ENOMEM;
		goto err_region;
	}
	dos->map = regmap_init_mmio(dos->dev, dos->base, &dos_regmap_config);
	if (IS_ERR(dos->map)) {
		ret = PTR_ERR(dos->map);
		goto err_map;
	}
	dos->pd_dev = dev_pm_domain_attach_by_name(dos->dev, "vdec");
	if (IS_ERR_OR_NULL(dos->pd_dev)) {
		ret = dos->pd_dev ? PTR_ERR(dos->pd_dev) : -ENODEV;
		goto err_regmap;
	}
	dos->hevc_irq = platform_get_irq_byname(pdev, "mailbox_0");
	if (dos->hevc_irq < 0) {
		ret = dos->hevc_irq;
		goto err_vdec_pd;
	}
	dos->hevc_clk = clk_get(dos->dev, "hevc");
	if (IS_ERR(dos->hevc_clk)) {
		ret = PTR_ERR(dos->hevc_clk);
		goto err_vdec_pd;
	}
	dos->hevc_reset = reset_control_get_exclusive(dos->dev, "hevc-pipeline");
	if (IS_ERR(dos->hevc_reset)) {
		ret = PTR_ERR(dos->hevc_reset);
		goto err_hevc_clk;
	}
	dos->hevc_pd_dev = dev_pm_domain_attach_by_name(dos->dev, "hevc");
	if (IS_ERR_OR_NULL(dos->hevc_pd_dev)) {
		ret = dos->hevc_pd_dev ? PTR_ERR(dos->hevc_pd_dev) : -ENODEV;
		goto err_hevc_reset;
	}
	if (!IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL))
		return dos;
	dos->hcodec_irq = platform_get_irq_byname(pdev, "mailbox_2");
	if (dos->hcodec_irq < 0) {
		ret = dos->hcodec_irq;
		goto err_hevc_pd;
	}
	dos->hcodec_clk = clk_get(dos->dev, "hcodec");
	if (IS_ERR(dos->hcodec_clk)) {
		ret = PTR_ERR(dos->hcodec_clk);
		goto err_hevc_pd;
	}
	dos->hcodec_pd_dev = dev_pm_domain_attach_by_name(dos->dev, "hcodec");
	if (IS_ERR_OR_NULL(dos->hcodec_pd_dev)) {
		ret = dos->hcodec_pd_dev ? PTR_ERR(dos->hcodec_pd_dev) : -ENODEV;
		goto err_hcodec_clk;
	}
	return dos;

err_hcodec_clk:
	clk_put(dos->hcodec_clk);
err_hevc_pd:
	dev_pm_domain_detach(dos->hevc_pd_dev, false);
err_hevc_reset:
	reset_control_put(dos->hevc_reset);
err_hevc_clk:
	clk_put(dos->hevc_clk);
err_vdec_pd:
	dev_pm_domain_detach(dos->pd_dev, false);
err_regmap:
	regmap_exit(dos->map);
err_map:
	iounmap(dos->base);
err_region:
	release_mem_region(dos->start, dos->size);
err_vdec_clk:
	clk_put(dos->vdec_clk);
err_bus_clk:
	clk_put(dos->bus_clk);
err_device:
	put_device(dos->dev);
	kfree(dos);
	return ERR_PTR(ret);
}

int s7d_codec_dos_irq(struct s7d_codec_dos *dos, enum s7d_codec_engine engine)
{
	int ret = dos_engine_check(engine);

	if (ret)
		return ret;
	if (engine == S7D_CODEC_ENGINE_HCODEC)
		return dos->hcodec_irq;
	return engine == S7D_CODEC_ENGINE_HEVC ? dos->hevc_irq : dos->irq;
}

struct regmap *s7d_codec_dos_regmap(struct s7d_codec_dos *dos,
				 enum s7d_codec_engine engine)
{
	int ret = dos_engine_check(engine);

	return ret ? ERR_PTR(ret) : dos->map;
}

static int dos_hold_rate(struct s7d_codec_dos *dos,
			 enum s7d_codec_engine engine, unsigned long rate,
			 struct s7d_codec_lease *lease)
{
	struct dos_engine *e;
	struct device *pd_dev;
	struct clk *clk;
	long rounded;
	int ret = dos_engine_check(engine);

	if (ret)
		return ret;
	if (!dos || !lease)
		return -EINVAL;
	/* A second session of a one-session codec, with the same lease. */
	if (lease->held)
		return -EBUSY;
	e = dos_eng(dos, engine);
	mutex_lock(&e->lock);
	if (e->holders >= e->slots || e->faulted || e->release_failed ||
	    dos->closing) {
		ret = -EBUSY;
		goto out;
	}
	/* The first holder powers the engine; the others share it. */
	if (e->holders)
		goto held;
	clk = dos_engine_clk(dos, engine);
	pd_dev = dos_engine_pd(dos, engine);
	rounded = clk_round_rate(clk, rate);
	if (rounded < 0) {
		ret = rounded;
		goto out;
	}
	if (rounded < rate - rate / 1000000 || rounded > rate) {
		ret = -ERANGE;
		goto out;
	}
	ret = clk_set_rate_exclusive(clk, rate);
	if (ret)
		goto out;
	if (clk_get_rate(clk) != rounded) {
		ret = -ERANGE;
		goto put_rate;
	}
	ret = clk_prepare_enable(dos->bus_clk);
	if (ret)
		goto put_rate;
	ret = clk_prepare_enable(clk);
	if (ret)
		goto disable_bus;
	ret = pm_runtime_resume_and_get(pd_dev);
	if (ret < 0)
		goto disable_vdec;
	e->error_reported = false;
	e->local_access_ready = false;
	e->halted = false;
	e->resident = 0;
held:
	e->holders++;
	lease->dos = dos;
	lease->engine = engine;
	INIT_LIST_HEAD(&lease->queue);
	lease->held = true;
	if (!e->shared)
		dos_set_runner(e, lease);
	ret = 0;
	goto out;

disable_vdec:
	clk_disable_unprepare(clk);
disable_bus:
	clk_disable_unprepare(dos->bus_clk);
put_rate:
	clk_rate_exclusive_put(clk);
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_hold(struct s7d_codec_dos *dos, enum s7d_codec_engine engine,
		       struct s7d_codec_lease *lease)
{
	if (engine == S7D_CODEC_ENGINE_HCODEC)
		return -EOPNOTSUPP;
	return dos_hold_rate(dos, engine, DOS_ENGINE_RATE, lease);
}

int s7d_codec_dos_jpeg_hold(struct s7d_codec_dos *dos, struct s7d_codec_lease *lease)
{
	return dos_hold_rate(dos, S7D_CODEC_ENGINE_HCODEC, DOS_JPEG_RATE, lease);
}

/* The engine of a held lease of @engine, or an error. */
static int dos_lease_of(const struct s7d_codec_lease *lease, enum s7d_codec_engine engine,
			struct dos_engine **out)
{
	int ret = dos_lease_engine(lease, out);

	return ret ? ret : lease->engine == engine ? 0 : -EINVAL;
}

int s7d_codec_dos_jpeg_prepare(struct s7d_codec_lease *lease)
{
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_lease_of(lease, S7D_CODEC_ENGINE_HCODEC, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || e->faulted || e->release_failed || dos->closing ||
	    e->prepared) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_prepare_access_locked(dos, S7D_CODEC_ENGINE_HCODEC);
	if (ret)
		goto fault;
	ret = dos_hcodec_halt_locked(dos);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, HCODEC_MMC_CTRL1, 0x32);
	if (ret)
		goto fault;
	e->prepared = true;
	goto out;
fault:
	dos_fault_locked(e);
	e->halted = false;
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_jpeg_port_enable(struct s7d_codec_lease *lease)
{
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_lease_of(lease, S7D_CODEC_ENGINE_HCODEC, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || !e->prepared || !e->halted || e->faulted ||
	    e->release_failed || dos->closing) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_hcodec_stopped_locked(dos);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_HCODEC_PORT, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_CHAN_STS, DMC_HCODEC_PORT,
			DMC_HCODEC_PORT);
	if (ret)
		goto fault;
	e->halted = false;
	ret = dos_update(dos->dmc, DMC_REQ_CTRL, DMC_HCODEC_PORT, DMC_HCODEC_PORT);
	if (ret)
		goto fault;
	goto out;
fault:
	dos_fault_locked(e);
	e->halted = false;
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_jpeg_start(struct s7d_codec_lease *lease)
{
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_lease_of(lease, S7D_CODEC_ENGINE_HCODEC, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || !e->prepared || e->halted || e->started ||
	    e->faulted || e->release_failed || dos->closing) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_check(dos->map, HCODEC_MBOX2_MASK, U32_MAX, U32_MAX);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, HCODEC_MPSR, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, HCODEC_CPSR, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, HCODEC_IMEM_DMA_CTRL, BIT(15), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, HCODEC_MMC_CTRL1, U32_MAX, 0x32);
	if (ret)
		goto fault;
	/* MFDIN_SCMD bit 28 (enable) is write-only: it reads back 0 once set. */
	ret = dos_check(dos->map, HCODEC_MFDIN_DCFG, BIT(16) | BIT(17), BIT(17));
	if (ret)
		goto fault;
	ret = dos_check(dos->map, HCODEC_VLC_VB_CONTROL, GENMASK(5, 0), 0x3a);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_HCODEC_PORT, DMC_HCODEC_PORT);
	if (ret)
		goto fault;
	e->started = true;
	ret = dos_hcodec_reset(dos, DOS_CPU_RESET);
	if (ret)
		goto fault;
	ret = dos_write_mpsr(dos->map, HCODEC_MPSR, 1);
	if (ret)
		goto fault;
	goto out;
fault:
	dos_fault_locked(e);
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_prepare_access(struct s7d_codec_lease *lease)
{
	struct dos_engine *e;
	int ret = dos_lease_engine(lease, &e);

	if (ret)
		return ret;
	mutex_lock(&e->lock);
	ret = dos_runs(e, lease) ? dos_prepare_access_locked(lease->dos, lease->engine) :
		-EINVAL;
	mutex_unlock(&e->lock);
	return ret;
}

/* A held lease of VDEC or the HEVC core, the engines the generic sequences serve. */
static int dos_decoder_lease(const struct s7d_codec_lease *lease, struct dos_engine **out)
{
	int ret = dos_lease_engine(lease, out);

	if (ret || lease->engine == S7D_CODEC_ENGINE_HEVC)
		return ret;
	return dos_vdec_engine_check(lease->engine);
}

int s7d_codec_dos_prepare(struct s7d_codec_lease *lease)
{
	enum s7d_codec_engine engine;
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_decoder_lease(lease, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	engine = lease->engine;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || e->faulted || e->release_failed || e->prepared) {
		ret = -EBUSY;
		goto out;
	}
	e->halted = false;
	ret = dos_prepare_access_locked(dos, engine);
	if (ret)
		goto fault;
	ret = dos_halt_locked(dos, engine);
	if (ret)
		goto fault;
	if (engine == S7D_CODEC_ENGINE_HEVC) {
		e->prepared = true;
		goto out;
	}
	ret = dos_pic_reset(dos);
	if (ret)
		goto fault;
	e->prepared = true;
	goto out;
fault:
	dos_fault_locked(e);
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_port_enable(struct s7d_codec_lease *lease)
{
	enum s7d_codec_engine engine;
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_decoder_lease(lease, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	engine = lease->engine;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || !e->prepared || !e->halted || e->faulted ||
	    e->release_failed) {
		ret = -EBUSY;
		goto out;
	}
	if (engine == S7D_CODEC_ENGINE_HEVC) {
		ret = dos_hevc_port_enable_locked(dos);
		if (ret)
			goto fault;
		goto out;
	}
	ret = dos_check(dos->map, DOS_MBOX1_MASK, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_MPSR, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_CPSR, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_VFIFO_CTRL, DOS_VFIFO_ENABLE, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_MMC_CTRL1, BIT(3), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_PIC_DC_MUX_CTRL, BIT(31), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_EXTIF_CFG1, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_IMEM_DMA_CTRL, BIT(15), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_LMEM_DMA_CTRL, BIT(15), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_WRRSP_LMEM, GENMASK(11, 0), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_CHAN_STS, DMC_VDEC_PORT, DMC_VDEC_PORT);
	if (ret)
		goto fault;
	e->halted = false;
	ret = dos_update(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, DMC_VDEC_PORT);
	if (ret)
		goto fault;
	goto out;
fault:
	dos_fault_locked(e);
	e->halted = false;
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_hevc_init(struct s7d_codec_lease *lease)
{
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_lease_of(lease, S7D_CODEC_ENGINE_HEVC, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	mutex_lock(&e->lock);
	ret = dos_hevc_exec_check_locked(dos, lease);
	if (ret)
		goto out;
	if (e->hevc_init_written) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_hevc_check(dos, HEVC_MBOX0_MASK, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_hevc_stopped_cpu_locked(dos);
	if (ret)
		goto fault;
	ret = dos_hevc_dma_idle_locked(dos);
	if (ret)
		goto fault;
	ret = dos_hevc_check(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH, 0);
	if (ret)
		goto fault;
	ret = dos_hevc_check(dos, HEVC_AXI_CTRL, HEVC_AXI_STOP, 0);
	if (ret)
		goto fault;
	ret = dos_hevc_write_raw(dos, DOS_SW_RESET3, HEVC_IQIT_RESET);
	if (ret)
		goto fault;
	e->hevc_init_written = true;
	goto out;
fault:
	dos_fault_locked(e);
	e->halted = false;
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_hevc_irq_mask(struct s7d_codec_lease *lease, bool enable)
{
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_lease_of(lease, S7D_CODEC_ENGINE_HEVC, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || !e->local_access_ready || e->release_failed) {
		ret = -EBUSY;
		goto out;
	}
	if (enable) {
		ret = dos_hevc_exec_check_locked(dos, lease);
		if (ret)
			goto out;
		if (!e->hevc_init_written) {
			ret = -EBUSY;
			goto out;
		}
		ret = dos_hevc_stopped_cpu_locked(dos);
		if (ret)
			goto fault;
		ret = dos_hevc_check(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH, 0);
		if (ret)
			goto fault;
	}
	ret = dos_hevc_write(dos, HEVC_MBOX0_MASK, enable ? 1 : 0);
	if (ret)
		goto fault;
	goto out;
fault:
	dos_fault_locked(e);
	e->halted = false;
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_hevc_input_enable(struct s7d_codec_lease *lease)
{
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_lease_of(lease, S7D_CODEC_ENGINE_HEVC, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	mutex_lock(&e->lock);
	ret = dos_hevc_exec_check_locked(dos, lease);
	if (ret)
		goto out;
	if (!e->hevc_init_written) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_hevc_check(dos, HEVC_MBOX0_MASK, U32_MAX, 1);
	if (ret)
		goto fault;
	ret = dos_hevc_stopped_cpu_locked(dos);
	if (ret)
		goto fault;
	ret = dos_hevc_check(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH, 0);
	if (ret)
		goto fault;
	ret = dos_hevc_update(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_ENDIAN,
			 HEVC_STREAM_ENDIAN);
	if (ret)
		goto fault;
	ret = regmap_update_bits(dos->map, HEVC_STREAM_FIFO_CTL,
				 HEVC_STREAM_FIFO_HOLE, HEVC_STREAM_FIFO_HOLE);
	if (ret) {
		dos_report_io(dos, "update", HEVC_STREAM_FIFO_CTL,
			      HEVC_STREAM_FIFO_HOLE, HEVC_STREAM_FIFO_HOLE, 0, false, ret);
		goto fault;
	}
	ret = dos_hevc_update(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH,
			 HEVC_STREAM_FETCH);
	if (ret)
		goto fault;
	goto out;
fault:
	dos_fault_locked(e);
	e->halted = false;
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_halt(struct s7d_codec_lease *lease)
{
	enum s7d_codec_engine engine;
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_lease_engine(lease, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	engine = lease->engine;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease)) {
		ret = -EINVAL;
		goto out;
	}
	ret = dos_prepare_access_locked(dos, engine);
	if (ret)
		goto out;
	ret = dos_halt_locked(dos, engine);
	if (ret)
		dos_fault_locked(e);
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_start(struct s7d_codec_lease *lease)
{
	enum s7d_codec_engine engine;
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	int ret = dos_decoder_lease(lease, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	engine = lease->engine;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || !e->prepared || e->halted || e->started ||
	    e->faulted || e->release_failed || dos->closing) {
		ret = -EBUSY;
		goto out;
	}
	if (engine == S7D_CODEC_ENGINE_HEVC) {
		ret = dos_hevc_start_locked(dos);
		if (ret)
			goto fault;
		goto out;
	}
	ret = dos_check(dos->map, DOS_MBOX1_MASK, U32_MAX, 1);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_MPSR, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_CPSR, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_IMEM_DMA_CTRL, BIT(15), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_LMEM_DMA_CTRL, BIT(15), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_WRRSP_LMEM, GENMASK(11, 0), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_VFIFO_CTRL, DOS_VFIFO_ENABLE, DOS_VFIFO_ENABLE);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_MMC_CTRL1, BIT(3), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_PIC_DC_MUX_CTRL, BIT(31), 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_EXTIF_CFG1, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, DMC_VDEC_PORT);
	if (ret)
		goto fault;
	e->started = true;
	ret = dos_reset(dos, DOS_CPU_RESET);
	if (ret)
		goto fault;
	ret = dos_write_mpsr(dos->map, DOS_MPSR, 1);
	if (ret)
		goto fault;
	goto out;
fault:
	dos_fault_locked(e);
out:
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_park(struct s7d_codec_lease *lease)
{
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	u32 val;
	int ret = dos_lease_of(lease, S7D_CODEC_ENGINE_VDEC, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease) || !e->prepared || e->halted || !e->started ||
	    e->faulted || e->release_failed || dos->closing) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_check(dos->map, DOS_MBOX1_MASK, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_write_mpsr(dos->map, DOS_MPSR, 0);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, DOS_CPSR, 0);
	if (ret)
		goto fault;
	ret = dos_update(dos->map, DOS_VFIFO_CTRL, DOS_VFIFO_ENABLE, 0);
	if (ret)
		goto fault;
	ret = regmap_read_poll_timeout(dos->map, DOS_IMEM_DMA_CTRL, val,
				      !(val & BIT(15)), 10, DMA_TIMEOUT_US);
	if (ret)
		goto fault;
	ret = regmap_read_poll_timeout(dos->map, DOS_LMEM_DMA_CTRL, val,
				      !(val & BIT(15)), 10, DMA_TIMEOUT_US);
	if (ret)
		goto fault;
	ret = regmap_read_poll_timeout(dos->map, DOS_WRRSP_LMEM, val,
				      !(val & GENMASK(11, 0)), 10,
				      RESPONSE_TIMEOUT_US);
	if (ret)
		goto fault;
	ret = dos_update(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, 0);
	if (ret)
		goto fault;
	ret = regmap_read_poll_timeout(dos->dmc, DMC_CHAN_STS, val,
				      val & DMC_VDEC_PORT, 10, DMC_TIMEOUT_US);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_MBOX1_MASK, U32_MAX, 0);
	if (ret)
		goto fault;
	/* A CPU stopped while it waits for the host keeps status bits set. */
	ret = dos_check(dos->map, DOS_MPSR, DOS_MPSR_RUN, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_CPSR, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, DOS_VFIFO_CTRL, DOS_VFIFO_ENABLE, 0);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, 0);
	if (ret)
		goto fault;
	e->halted = true;
	e->prepared = false;
	e->started = false;
	goto out;
fault:
	dos_fault_locked(e);
	e->halted = false;
out:
	mutex_unlock(&e->lock);
	return ret;
}

/*
 * Vendor power-off order: reset the engine and power down its memories while
 * its clocks still run. Without it the memories keep stale state, and opening
 * the DMC port soon after the next power-up can hang the bus.
 */
static int dos_power_down_locked(struct s7d_codec_dos *dos,
				 enum s7d_codec_engine engine)
{
	int ret;

	if (engine == S7D_CODEC_ENGINE_HEVC) {
		ret = dos_hevc_write_raw(dos, DOS_SW_RESET3, U32_MAX);
		if (ret)
			return ret;
		usleep_range(20, 30);
		ret = dos_hevc_write_raw(dos, DOS_SW_RESET3, 0);
		if (ret)
			return ret;
		usleep_range(10, 20);
		return dos_hevc_write_raw(dos, DOS_MEM_PD_HEVC, U32_MAX);
	}
	if (engine != S7D_CODEC_ENGINE_VDEC)
		return 0;
	ret = regmap_write(dos->map, DOS_SW_RESET0, DOS_VDEC_COLD_RESET);
	if (ret)
		return ret;
	usleep_range(20, 30);
	ret = regmap_write(dos->map, DOS_SW_RESET0, 0);
	if (ret)
		return ret;
	usleep_range(10, 20);
	return regmap_write(dos->map, DOS_MEM_PD_VDEC, U32_MAX);
}

int s7d_codec_dos_release(struct s7d_codec_lease *lease)
{
	enum s7d_codec_engine engine;
	struct s7d_codec_dos *dos;
	struct dos_engine *e;
	struct device *pd_dev;
	struct clk *clk;
	bool drop_pin = false;
	int ret = dos_lease_engine(lease, &e);

	if (ret)
		return ret;
	dos = lease->dos;
	engine = lease->engine;
	mutex_lock(&e->lock);
	/* A shared engine's holder ends its run first; another one runs it alone. */
	if (e->shared == dos_runner_is(e, lease)) {
		ret = -EBUSY;
		goto out;
	}
	/*
	 * The last holder powers the engine down; the others leave it running,
	 * or faulted by another holder's run: that holder keeps it.
	 */
	if (e->holders > 1 && (!e->faulted || dos_faulted_by_other(e, lease))) {
		e->holders--;
		lease->held = false;
		goto out;
	}
	if (e->faulted || !e->halted) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_power_down_locked(dos, engine);
	if (ret) {
		dos_fault_locked(e);
		goto out;
	}
	clk = dos_engine_clk(dos, engine);
	pd_dev = dos_engine_pd(dos, engine);
	ret = pm_runtime_put_sync(pd_dev);
	if (ret < 0) {
		pm_runtime_get_noresume(pd_dev);
		e->release_failed = true;
		if (!e->release_pin) {
			if (try_module_get(dos->owner))
				e->release_pin = true;
			else
				ret = -ESHUTDOWN;
		}
		goto out;
	}
	clk_disable_unprepare(clk);
	clk_disable_unprepare(dos->bus_clk);
	clk_rate_exclusive_put(clk);
	e->holders = 0;
	dos_set_runner(e, NULL);
	lease->held = false;
	e->resident = 0;
	e->local_access_ready = false;
	e->prepared = false;
	e->hevc_init_written = false;
	e->halted = false;
	e->started = false;
	e->release_failed = false;
	drop_pin = e->release_pin;
	e->release_pin = false;
	ret = 0;
out:
	mutex_unlock(&e->lock);
	if (drop_pin)
		module_put(dos->owner);
	return ret;
}

/* Inside wait_event(): take a free engine when first in line, or leave the line. */
static bool dos_run_turn(struct dos_engine *e, struct s7d_codec_lease *lease,
			 bool (*cancelled)(void *priv), void *priv, int *ret)
{
	bool done = true, others = false;

	spin_lock(&e->run_lock);
	if (READ_ONCE(e->faulted) || READ_ONCE(e->release_failed) ||
	    READ_ONCE(lease->dos->closing)) {
		*ret = -EIO;
	} else if (!e->runner &&
		   list_first_entry(&e->queue, struct s7d_codec_lease, queue) == lease) {
		e->runner = lease;
		*ret = 0;
	} else if (cancelled && cancelled(priv)) {
		*ret = -ECANCELED;
	} else {
		done = false;
	}
	if (done) {
		list_del_init(&lease->queue);
		others = *ret && !list_empty(&e->queue);
	}
	spin_unlock(&e->run_lock);
	/* Leaving the head of the line lets the next lease take a free engine. */
	if (others)
		wake_up_all(&e->run_wait);
	return done;
}

int s7d_codec_dos_run_begin(struct s7d_codec_lease *lease,
			    bool (*cancelled)(void *priv), void *priv)
{
	struct s7d_codec_lease *runner;
	struct dos_engine *e;
	int ret = dos_lease_engine(lease, &e);

	if (ret || !e->shared)
		return ret;
	spin_lock(&e->run_lock);
	if (e->runner == lease || !list_empty(&lease->queue)) {
		spin_unlock(&e->run_lock);
		return -EBUSY;
	}
	list_add_tail(&lease->queue, &e->queue);
	runner = e->runner;
	if (runner && runner->contended)
		runner->contended(runner->priv);
	spin_unlock(&e->run_lock);
	wait_event(e->run_wait, dos_run_turn(e, lease, cancelled, priv, &ret));
	return ret;
}

/* The next runner may program the engine: nothing of this run still runs. */
static bool dos_run_idle_locked(struct dos_engine *e)
{
	return e->faulted || e->release_failed ||
	       (!e->prepared && !e->started && (e->halted || !e->local_access_ready));
}

int s7d_codec_dos_run_end(struct s7d_codec_lease *lease)
{
	struct dos_engine *e;
	int ret = dos_lease_engine(lease, &e);

	if (ret || !e->shared)
		return ret;
	mutex_lock(&e->lock);
	if (!dos_runs(e, lease)) {
		mutex_unlock(&e->lock);
		return -EINVAL;
	}
	if (!dos_run_idle_locked(e)) {
		dev_warn(lease->dos->dev, "engine %u left running at the end of a run; halting it\n",
			 lease->engine);
		ret = dos_prepare_access_locked(lease->dos, lease->engine);
		if (!ret)
			ret = dos_halt_locked(lease->dos, lease->engine);
		if (ret) {
			dos_fault_locked(e);
			e->halted = false;
		}
	}
	dos_set_runner(e, NULL);
	mutex_unlock(&e->lock);
	wake_up_all(&e->run_wait);
	return ret;
}

void s7d_codec_dos_run_wake(struct s7d_codec_lease *lease)
{
	struct s7d_codec_dos *dos = lease ? lease->dos : NULL;

	if (dos && !dos_engine_check(lease->engine))
		wake_up_all(&dos_eng(dos, lease->engine)->run_wait);
}

bool s7d_codec_dos_run_contended(struct s7d_codec_lease *lease)
{
	struct dos_engine *e;
	bool contended;

	if (dos_lease_engine(lease, &e) || !e->shared)
		return false;
	spin_lock(&e->run_lock);
	contended = e->runner == lease && !list_empty(&e->queue);
	spin_unlock(&e->run_lock);
	return contended;
}

bool s7d_codec_dos_faulted_by_other(struct s7d_codec_lease *lease)
{
	struct dos_engine *e;
	bool other;

	if (dos_lease_engine(lease, &e))
		return false;
	mutex_lock(&e->lock);
	other = !dos_runner_is(e, lease) && dos_faulted_by_other(e, lease);
	mutex_unlock(&e->lock);
	return other;
}

u32 s7d_codec_dos_resident(struct s7d_codec_lease *lease)
{
	struct dos_engine *e;
	u32 tag = 0;

	if (dos_lease_engine(lease, &e))
		return 0;
	mutex_lock(&e->lock);
	/* A fault leaves the instruction memory unknown, as does a power-up. */
	if (dos_runs(e, lease) && e->local_access_ready && !e->faulted && !e->release_failed)
		tag = e->resident;
	mutex_unlock(&e->lock);
	return tag;
}

int s7d_codec_dos_set_resident(struct s7d_codec_lease *lease, u32 tag)
{
	struct dos_engine *e;
	int ret = dos_lease_engine(lease, &e);

	if (ret)
		return ret;
	mutex_lock(&e->lock);
	if (dos_runs(e, lease))
		e->resident = tag;
	else
		ret = -EBUSY;
	mutex_unlock(&e->lock);
	return ret;
}

int s7d_codec_dos_engine_state(struct s7d_codec_dos *dos, enum s7d_codec_engine engine)
{
	struct dos_engine *e;
	int ret = dos_engine_check(engine);

	if (ret)
		return ret;
	e = dos_eng(dos, engine);
	mutex_lock(&e->lock);
	if (e->faulted || e->release_failed)
		ret = -EIO;
	else if (e->holders >= e->slots)
		ret = -EBUSY;
	mutex_unlock(&e->lock);
	return ret;
}

/* Any engine still held, prepared or faulted keeps the DOS from sleeping. */
static bool dos_busy_locked(struct s7d_codec_dos *dos)
{
	struct dos_engine *e;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dos->eng); i++) {
		e = &dos->eng[i];
		if (e->holders || e->prepared || e->faulted || e->release_failed ||
		    e->release_pin)
			return true;
	}
	return false;
}

int s7d_codec_dos_suspend_check(struct s7d_codec_dos *dos)
{
	int ret;

	dos_lock_all(dos);
	ret = dos_busy_locked(dos) ? -EBUSY : 0;
	dos_unlock_all(dos);
	return ret;
}

int s7d_codec_dos_destroy(struct s7d_codec_dos *dos)
{
	unsigned int i;

	dos_lock_all(dos);
	if (dos_busy_locked(dos)) {
		dos_unlock_all(dos);
		return -EBUSY;
	}
	dos->closing = true;
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL))
		dev_pm_domain_detach(dos->hcodec_pd_dev, false);
	dev_pm_domain_detach(dos->hevc_pd_dev, false);
	dev_pm_domain_detach(dos->pd_dev, false);
	regmap_exit(dos->map);
	iounmap(dos->base);
	release_mem_region(dos->start, dos->size);
	reset_control_put(dos->hevc_reset);
	if (IS_ENABLED(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL))
		clk_put(dos->hcodec_clk);
	clk_put(dos->hevc_clk);
	clk_put(dos->vdec_clk);
	clk_put(dos->bus_clk);
	dos_unlock_all(dos);
	for (i = 0; i < ARRAY_SIZE(dos->eng); i++)
		mutex_destroy(&dos->eng[i].lock);
	mutex_destroy(&dos->lock);
	put_device(dos->dev);
	kfree(dos);
	return 0;
}
