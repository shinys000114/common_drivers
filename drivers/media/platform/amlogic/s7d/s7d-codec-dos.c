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

#include "s7d-codec-dos.h"

#define DOS_REG(word)		((word) * 4)
#define DOS_MMC_CTRL1		DOS_REG(0x0002)
#define DOS_MBOX1_MASK		DOS_REG(0x0076)
#define DOS_MPSR			DOS_REG(0x0301)
#define DOS_CPSR			DOS_REG(0x0321)
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
#define HEVC_FIFO_ENABLE		BIT(29)
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
	struct mutex lock;
	int irq;
	int hevc_irq;
	int hcodec_irq;
	enum s7d_codec_engine engine;
	bool leased;
	bool local_access_ready;
	bool prepared;
	bool hevc_init_written;
	bool halted;
	bool started;
	bool faulted;
	bool error_reported;
	bool release_failed;
	bool closing;
	bool release_pin;
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

static int dos_lease_check(struct s7d_codec_dos *dos,
			   enum s7d_codec_engine engine)
{
	return dos->leased && dos->engine != engine ? -EBUSY : 0;
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

static int dos_update(struct regmap *map, unsigned int reg, u32 mask, u32 val)
{
	int ret = regmap_update_bits(map, reg, mask, val);

	return ret ? ret : dos_check(map, reg, mask, val);
}

static int dos_report_error(struct s7d_codec_dos *dos, const char *stage,
			    int result, int error)
{
	if (error && dos->engine == S7D_CODEC_ENGINE_HEVC && !dos->error_reported) {
		dos->error_reported = true;
		dev_err(dos->dev, "HEVC DOS %s result=%d error=%d\n", stage, result, error);
	}
	return error;
}

static int dos_report_io(struct s7d_codec_dos *dos, const char *stage,
			 unsigned int reg, u32 mask, u32 want,
			 u32 observed, bool observed_valid, int error)
{
	if (error && dos->engine == S7D_CODEC_ENGINE_HEVC && !dos->error_reported) {
		dos->error_reported = true;
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

static int dos_prepare_access_locked(struct s7d_codec_dos *dos)
{
	int ret;

	if (!dos->leased)
		return -EINVAL;
	if (dos->local_access_ready)
		return 0;
	if (dos->faulted || dos->release_failed || dos->closing)
		return -EIO;
	if (dos->engine == S7D_CODEC_ENGINE_HCODEC) {
		ret = dos_hcodec_prepare_access_locked(dos);
		if (ret)
			goto fault;
		dos->local_access_ready = true;
		return 0;
	}
	if (dos->engine == S7D_CODEC_ENGINE_HEVC) {
		ret = dos_hevc_prepare_access_locked(dos);
		if (ret)
			goto fault;
		dos->local_access_ready = true;
		return 0;
	}
	usleep_range(20, 30);
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
	dos->local_access_ready = true;
	return 0;
fault:
	dos->faulted = true;
	dos->halted = false;
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
	u32 val;
	int ret;

	dos->halted = false;
	ret = dos_hcodec_block_locked(dos);
	if (ret)
		return ret;
	ret = dos_write(dos->map, HCODEC_MBOX2_MASK, 0);
	if (ret)
		return ret;
	ret = dos_write(dos->map, HCODEC_MPSR, 0);
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
	dos->halted = true;
	dos->prepared = false;
	dos->started = false;
	dos->faulted = false;
	return 0;
}

static int dos_hevc_halt_locked(struct s7d_codec_dos *dos)
{
	u32 val = 0;
	int ret;

	dos->halted = false;
	ret = dos_hevc_write(dos, HEVC_MBOX0_MASK, 0);
	if (ret)
		return ret;
	ret = dos_hevc_write(dos, HEVC_MPSR, 0);
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
	ret = dos_hevc_write(dos, HEVC_STREAM_CONTROL, 0);
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
	ret = dos_hevc_check(dos, HEVC_STREAM_CONTROL, U32_MAX, 0);
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
	dos->halted = true;
	dos->prepared = false;
	dos->hevc_init_written = false;
	dos->started = false;
	dos->faulted = false;
	return 0;
}

static int dos_hevc_exec_check_locked(struct s7d_codec_dos *dos)
{
	int ret = dos_lease_check(dos, S7D_CODEC_ENGINE_HEVC);

	if (ret)
		return ret;
	if (!dos->leased || !dos->local_access_ready || !dos->prepared ||
	    dos->halted || dos->started || dos->faulted || dos->release_failed ||
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
	dos->halted = false;
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
	int ret;

	if (!dos->hevc_init_written)
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
	ret = dos_hevc_check(dos, HEVC_STREAM_FIFO_CTL, HEVC_FIFO_ENABLE,
			HEVC_FIFO_ENABLE);
	if (ret)
		return ret;
	ret = dos_hevc_check(dos, HEVC_AXI_CTRL, HEVC_AXI_STOP, 0);
	if (ret)
		return ret;
	dos->started = true;
	ret = dos_hevc_cpu_reset_locked(dos);
	if (ret)
		return ret;
	ret = dos_hevc_update(dos, HEVC_WRRSP_LMEM, HEVC_LMEM_PRIORITY,
			 HEVC_LMEM_PRIORITY);
	if (ret)
		return ret;
	return dos_hevc_write(dos, HEVC_MPSR, 1);
}

static int dos_halt_locked(struct s7d_codec_dos *dos)
{
	u32 val;
	int ret;

	if (dos->engine == S7D_CODEC_ENGINE_HCODEC)
		return dos_hcodec_halt_locked(dos);
	if (dos->engine == S7D_CODEC_ENGINE_HEVC)
		return dos_hevc_halt_locked(dos);

	dos->halted = false;
	ret = dos_write(dos->map, DOS_MBOX1_MASK, 0);
	if (ret)
		return ret;
	ret = dos_write(dos->map, DOS_MPSR, 0);
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
	dos->halted = true;
	dos->prepared = false;
	dos->started = false;
	dos->faulted = false;
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
			 enum s7d_codec_engine engine, unsigned long rate)
{
	struct device *pd_dev;
	struct clk *clk;
	long rounded;
	int ret = dos_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	if (dos->leased || dos->faulted || dos->release_failed || dos->closing) {
		ret = -EBUSY;
		goto out;
	}
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
	dos->leased = true;
	dos->engine = engine;
	dos->error_reported = false;
	dos->local_access_ready = false;
	dos->halted = false;
	ret = 0;
	goto out;

disable_vdec:
	clk_disable_unprepare(clk);
disable_bus:
	clk_disable_unprepare(dos->bus_clk);
put_rate:
	clk_rate_exclusive_put(clk);
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_hold(struct s7d_codec_dos *dos,
		       enum s7d_codec_engine engine)
{
	if (engine == S7D_CODEC_ENGINE_HCODEC)
		return -EOPNOTSUPP;
	return dos_hold_rate(dos, engine, DOS_ENGINE_RATE);
}

int s7d_codec_dos_jpeg_hold(struct s7d_codec_dos *dos)
{
	return dos_hold_rate(dos, S7D_CODEC_ENGINE_HCODEC, DOS_JPEG_RATE);
}

int s7d_codec_dos_jpeg_prepare(struct s7d_codec_dos *dos)
{
	int ret;

	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, S7D_CODEC_ENGINE_HCODEC);
	if (ret)
		goto out;
	if (!dos->leased || dos->faulted || dos->release_failed || dos->closing ||
	    dos->prepared) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_prepare_access_locked(dos);
	if (ret)
		goto fault;
	ret = dos_hcodec_halt_locked(dos);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, HCODEC_MMC_CTRL1, 0x32);
	if (ret)
		goto fault;
	dos->prepared = true;
	goto out;
fault:
	dos->faulted = true;
	dos->halted = false;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_jpeg_port_enable(struct s7d_codec_dos *dos)
{
	int ret;

	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, S7D_CODEC_ENGINE_HCODEC);
	if (ret)
		goto out;
	if (!dos->leased || !dos->prepared || !dos->halted || dos->faulted ||
	    dos->release_failed || dos->closing) {
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
	dos->halted = false;
	ret = dos_update(dos->dmc, DMC_REQ_CTRL, DMC_HCODEC_PORT, DMC_HCODEC_PORT);
	if (ret)
		goto fault;
	goto out;
fault:
	dos->faulted = true;
	dos->halted = false;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_jpeg_start(struct s7d_codec_dos *dos)
{
	int ret;

	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, S7D_CODEC_ENGINE_HCODEC);
	if (ret)
		goto out;
	if (!dos->leased || !dos->prepared || dos->halted || dos->started ||
	    dos->faulted || dos->release_failed || dos->closing) {
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
	ret = dos_check(dos->map, HCODEC_MFDIN_SCMD, HCODEC_MFDIN_ENABLE,
			HCODEC_MFDIN_ENABLE);
	if (ret)
		goto fault;
	ret = dos_check(dos->map, HCODEC_MFDIN_DCFG, BIT(16) | BIT(17), BIT(17));
	if (ret)
		goto fault;
	ret = dos_check(dos->map, HCODEC_VLC_VB_CONTROL, GENMASK(5, 0), 0x3a);
	if (ret)
		goto fault;
	ret = dos_check(dos->dmc, DMC_REQ_CTRL, DMC_HCODEC_PORT, DMC_HCODEC_PORT);
	if (ret)
		goto fault;
	dos->started = true;
	ret = dos_hcodec_reset(dos, DOS_CPU_RESET);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, HCODEC_MPSR, 1);
	if (ret)
		goto fault;
	goto out;
fault:
	dos->faulted = true;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_prepare_access(struct s7d_codec_dos *dos,
			       enum s7d_codec_engine engine)
{
	int ret = dos_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, engine);
	if (!ret)
		ret = dos_prepare_access_locked(dos);
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_prepare(struct s7d_codec_dos *dos,
			  enum s7d_codec_engine engine)
{
	int ret = engine == S7D_CODEC_ENGINE_HEVC ? 0 : dos_vdec_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, engine);
	if (ret)
		goto out;
	if (!dos->leased || dos->faulted || dos->release_failed || dos->prepared) {
		ret = -EBUSY;
		goto out;
	}
	dos->halted = false;
	ret = dos_prepare_access_locked(dos);
	if (ret)
		goto fault;
	ret = dos_halt_locked(dos);
	if (ret)
		goto fault;
	if (engine == S7D_CODEC_ENGINE_HEVC) {
		dos->prepared = true;
		goto out;
	}
	ret = dos_pic_reset(dos);
	if (ret)
		goto fault;
	dos->prepared = true;
	goto out;
fault:
	dos->faulted = true;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_port_enable(struct s7d_codec_dos *dos,
			      enum s7d_codec_engine engine)
{
	int ret = engine == S7D_CODEC_ENGINE_HEVC ? 0 : dos_vdec_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, engine);
	if (ret)
		goto out;
	if (!dos->leased || !dos->prepared || !dos->halted || dos->faulted ||
	    dos->release_failed) {
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
	dos->halted = false;
	ret = dos_update(dos->dmc, DMC_REQ_CTRL, DMC_VDEC_PORT, DMC_VDEC_PORT);
	if (ret)
		goto fault;
	goto out;
fault:
	dos->faulted = true;
	dos->halted = false;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_hevc_init(struct s7d_codec_dos *dos)
{
	int ret;

	mutex_lock(&dos->lock);
	ret = dos_hevc_exec_check_locked(dos);
	if (ret)
		goto out;
	if (dos->hevc_init_written) {
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
	dos->hevc_init_written = true;
	goto out;
fault:
	dos->faulted = true;
	dos->halted = false;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_hevc_irq_mask(struct s7d_codec_dos *dos, bool enable)
{
	int ret;

	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, S7D_CODEC_ENGINE_HEVC);
	if (ret)
		goto out;
	if (!dos->leased || !dos->local_access_ready || dos->release_failed) {
		ret = -EBUSY;
		goto out;
	}
	if (enable) {
		ret = dos_hevc_exec_check_locked(dos);
		if (ret)
			goto out;
		if (!dos->hevc_init_written) {
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
	dos->faulted = true;
	dos->halted = false;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_hevc_input_enable(struct s7d_codec_dos *dos)
{
	int ret;

	mutex_lock(&dos->lock);
	ret = dos_hevc_exec_check_locked(dos);
	if (ret)
		goto out;
	if (!dos->hevc_init_written) {
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
	ret = dos_hevc_update(dos, HEVC_STREAM_FIFO_CTL, HEVC_FIFO_ENABLE,
			 HEVC_FIFO_ENABLE);
	if (ret)
		goto fault;
	ret = dos_hevc_update(dos, HEVC_STREAM_CONTROL, HEVC_STREAM_FETCH,
			 HEVC_STREAM_FETCH);
	if (ret)
		goto fault;
	goto out;
fault:
	dos->faulted = true;
	dos->halted = false;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_halt(struct s7d_codec_dos *dos,
		       enum s7d_codec_engine engine)
{
	int ret = dos_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, engine);
	if (ret)
		goto out;
	if (!dos->leased) {
		ret = -EINVAL;
		goto out;
	}
	ret = dos_prepare_access_locked(dos);
	if (ret)
		goto out;
	ret = dos_halt_locked(dos);
	if (ret)
		dos->faulted = true;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_start(struct s7d_codec_dos *dos,
			enum s7d_codec_engine engine)
{
	int ret = engine == S7D_CODEC_ENGINE_HEVC ? 0 : dos_vdec_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, engine);
	if (ret)
		goto out;
	if (!dos->leased || !dos->prepared || dos->halted || dos->started ||
	    dos->faulted || dos->release_failed || dos->closing) {
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
	dos->started = true;
	ret = dos_reset(dos, DOS_CPU_RESET);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, DOS_MPSR, 1);
	if (ret)
		goto fault;
	goto out;
fault:
	dos->faulted = true;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_park(struct s7d_codec_dos *dos,
		       enum s7d_codec_engine engine)
{
	u32 val;
	int ret = dos_vdec_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, engine);
	if (ret)
		goto out;
	if (!dos->leased || !dos->prepared || dos->halted || !dos->started ||
	    dos->faulted || dos->release_failed || dos->closing) {
		ret = -EBUSY;
		goto out;
	}
	ret = dos_check(dos->map, DOS_MBOX1_MASK, U32_MAX, 0);
	if (ret)
		goto fault;
	ret = dos_write(dos->map, DOS_MPSR, 0);
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
	ret = dos_check(dos->map, DOS_MPSR, U32_MAX, 0);
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
	dos->halted = true;
	dos->prepared = false;
	dos->started = false;
	goto out;
fault:
	dos->faulted = true;
	dos->halted = false;
out:
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_release(struct s7d_codec_dos *dos,
			  enum s7d_codec_engine engine)
{
	struct device *pd_dev;
	struct clk *clk;
	bool drop_pin = false;
	int ret = dos_engine_check(engine);

	if (ret)
		return ret;
	mutex_lock(&dos->lock);
	ret = dos_lease_check(dos, engine);
	if (ret)
		goto out;
	if (!dos->leased || !dos->halted || dos->faulted) {
		ret = -EBUSY;
		goto out;
	}
	clk = dos_engine_clk(dos, engine);
	pd_dev = dos_engine_pd(dos, engine);
	ret = pm_runtime_put_sync(pd_dev);
	if (ret < 0) {
		pm_runtime_get_noresume(pd_dev);
		dos->release_failed = true;
		if (!dos->release_pin) {
			if (try_module_get(dos->owner))
				dos->release_pin = true;
			else
				ret = -ESHUTDOWN;
		}
		goto out;
	}
	clk_disable_unprepare(clk);
	clk_disable_unprepare(dos->bus_clk);
	clk_rate_exclusive_put(clk);
	dos->leased = false;
	dos->local_access_ready = false;
	dos->prepared = false;
	dos->hevc_init_written = false;
	dos->halted = false;
	dos->started = false;
	dos->release_failed = false;
	drop_pin = dos->release_pin;
	dos->release_pin = false;
	ret = 0;
out:
	mutex_unlock(&dos->lock);
	if (drop_pin)
		module_put(dos->owner);
	return ret;
}

int s7d_codec_dos_suspend_check(struct s7d_codec_dos *dos)
{
	int ret;

	mutex_lock(&dos->lock);
	ret = dos->leased || dos->prepared || dos->faulted ||
	      dos->release_failed ? -EBUSY : 0;
	mutex_unlock(&dos->lock);
	return ret;
}

int s7d_codec_dos_destroy(struct s7d_codec_dos *dos)
{
	mutex_lock(&dos->lock);
	if (dos->leased || dos->prepared || dos->faulted || dos->release_failed ||
	    dos->release_pin) {
		mutex_unlock(&dos->lock);
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
	mutex_unlock(&dos->lock);
	put_device(dos->dev);
	kfree(dos);
	return 0;
}
