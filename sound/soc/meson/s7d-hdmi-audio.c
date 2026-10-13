// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <sound/pcm_params.h>
#include <sound/pcm_iec958.h>
#include <sound/soc.h>

#define FRDDR_CTRL0	0x00
#define FRDDR_CTRL1	0x04
#define FRDDR_START	0x08
#define FRDDR_END	0x0c
#define FRDDR_PERIOD	0x10
#define FRDDR_STATUS	0x14
#define FRDDR_POSITION	0x18
#define FRDDR_INIT	0x24
#define FRDDR_CTRL2	0x28
#define ARB_CTRL		0x280
#define HDMI_ROUTE	0x744
#define TOACODEC_ROUTE	0x740
#define TDM_CTRL1	0x04
#define TDM_SWAP		0x08
#define TDM_MASK0	0x0c
#define SPDIF_CTRL1	0x04
#define SPDIF_SWAP	0x0c
#define SPDIF_CHSTS0	0x10
#define SPDIF_GAIN_EN	0x4d8
#define DMA_ENABLE	BIT(31)
#define DMA_STOP	BIT(21)
#define DMA_STOP_DONE	BIT(17)
#define DMA_IRQ_ENABLE	GENMASK(23, 16)
#define DMA_PERIOD_IRQ	BIT(2)
#define HDMI_DATA	BIT(29)
#define HDMI_CLOCK	BIT(28)
#define BUFFER_BYTES	SZ_128K
#define FIFO_BYTES	1024
#define BURST_BYTES	8

enum s7d_audio_output {
	S7D_AUDIO_HDMI,
	S7D_AUDIO_ANALOG,
	S7D_AUDIO_SPDIF,
};

struct s7d_audio_path {
	const char *dai_name;
	const char *stream_name;
	enum s7d_audio_output output;
	unsigned int fifo;
	unsigned int sink;
	unsigned int sink_ctrl;
	unsigned int sink_clock;
	unsigned int sink_ctrl2;
	unsigned int gain_enable;
	unsigned int sample_clocks;
	unsigned int num_resets;
};

static const struct s7d_audio_path s7d_hdmi_path = {
	.dai_name = "s7d-tdmb", .stream_name = "HDMI Playback",
	.output = S7D_AUDIO_HDMI, .fifo = 0, .sink = 1,
	.sink_ctrl = 0x540, .sink_clock = 0x94,
	.sink_ctrl2 = 0xac0, .gain_enable = 0xaf0,
	.sample_clocks = 4, .num_resets = 2,
};

static const struct s7d_audio_path s7d_analog_path = {
	.dai_name = "s7d-tdmc", .stream_name = "Analog Playback",
	.output = S7D_AUDIO_ANALOG, .fifo = 1, .sink = 2,
	.sink_ctrl = 0x580, .sink_clock = 0x98,
	.sink_ctrl2 = 0xb00, .gain_enable = 0xb30,
	.sample_clocks = 3, .num_resets = 3,
};

static const struct s7d_audio_path s7d_spdif_path = {
	.dai_name = "s7d-spdif-a", .stream_name = "SPDIF Playback",
	.output = S7D_AUDIO_SPDIF, .fifo = 2, .sink = 3,
	.sink_ctrl = 0x48c, .gain_enable = SPDIF_GAIN_EN,
	.sample_clocks = 1, .num_resets = 2,
};

struct s7d_audio {
	struct device *dev;
	struct regmap *map;
	struct clk_bulk_data clocks[7];
	struct clk_bulk_data spdif_clocks[3];
	struct clk *spdif_pll;
	struct reset_control_bulk_data resets[3];
	const struct s7d_audio_path *path;
	struct snd_soc_dai_driver dai;
	struct snd_dma_buffer *buffer;
	struct snd_pcm_substream *substream;
	spinlock_t lock; /* DMA state and IRQ/substream lifetime. */
	int irq;
	bool powered;
	bool sample_clocks;
	bool exclusive;
	bool running;
	bool prepared;
	bool faulted;
};

static unsigned int s7d_audio_frddr(struct s7d_audio *a, unsigned int reg)
{
	return 0x1c0 + a->path->fifo * 0x40 + reg;
}

static int s7d_audio_route_data(struct s7d_audio *a, bool enable)
{
	switch (a->path->output) {
	case S7D_AUDIO_HDMI:
		return regmap_update_bits(a->map, HDMI_ROUTE, HDMI_DATA,
					 enable ? HDMI_DATA : 0);
	case S7D_AUDIO_ANALOG:
		return regmap_update_bits(a->map, TOACODEC_ROUTE, BIT(31),
					 enable ? BIT(31) : 0);
	case S7D_AUDIO_SPDIF:
		return 0;
	}
	return -EINVAL;
}

static int s7d_audio_route_clocks(struct s7d_audio *a, bool enable)
{
	int ret;

	if (a->path->output == S7D_AUDIO_HDMI)
		return regmap_update_bits(a->map, HDMI_ROUTE, HDMI_CLOCK,
					 enable ? HDMI_CLOCK : 0);
	if (a->path->output != S7D_AUDIO_ANALOG)
		return 0;
	if (enable) {
		ret = regmap_update_bits(a->map, TOACODEC_ROUTE, BIT(29), BIT(29));
		if (ret)
			return ret;
		return regmap_update_bits(a->map, TOACODEC_ROUTE, BIT(30), BIT(30));
	}
	ret = regmap_update_bits(a->map, TOACODEC_ROUTE, BIT(30), 0);
	if (ret)
		return ret;
	return regmap_update_bits(a->map, TOACODEC_ROUTE, BIT(29), 0);
}

static void s7d_audio_spdif_clock_put(struct s7d_audio *a)
{
	if (a->spdif_pll) {
		clk_rate_exclusive_put(a->spdif_pll);
		a->spdif_pll = NULL;
	}
}

static int s7d_audio_spdif_clock_get(struct s7d_audio *a, unsigned int rate)
{
	struct clk *pll;
	unsigned long pll_rate;
	int ret;

	if (rate == 44100) {
		pll = a->spdif_clocks[1].clk;
		pll_rate = 451584000;
	} else if (rate == 48000) {
		pll = a->spdif_clocks[2].clk;
		pll_rate = 491520000;
	} else {
		return -EINVAL;
	}
	ret = clk_set_rate_exclusive(pll, pll_rate);
	if (ret)
		return ret;
	a->spdif_pll = pll;
	if (clk_get_rate(pll) != pll_rate) {
		ret = -ERANGE;
		goto fail;
	}
	ret = clk_set_parent(a->spdif_clocks[0].clk, pll);
	if (ret)
		goto fail;
	return 0;
fail:
	s7d_audio_spdif_clock_put(a);
	return ret;
}

/* Keep the buffer and clocks if outstanding DDR reads cannot be drained. */
static int s7d_audio_stop(struct s7d_audio *a)
{
	u32 val;
	int ret;

	if (!a->powered)
		return 0;
	ret = s7d_audio_route_data(a, false);
	if (ret)
		goto fail;
	ret = regmap_read(a->map, s7d_audio_frddr(a, FRDDR_CTRL0), &val);
	if (ret)
		goto fail;
	if (a->running || (val & DMA_ENABLE)) {
		ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL2), DMA_STOP, 0);
		if (ret)
			goto fail;
		ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL2),
					 DMA_STOP, DMA_STOP);
		if (ret)
			goto fail;
		ret = regmap_read_poll_timeout_atomic(a->map, s7d_audio_frddr(a, FRDDR_STATUS), val,
						     val & DMA_STOP_DONE, 1, 200);
		if (ret)
			goto fail;
	}
	ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL0),
				 DMA_ENABLE | DMA_IRQ_ENABLE, 0);
	if (ret)
		goto fail;
	ret = regmap_read(a->map, s7d_audio_frddr(a, FRDDR_CTRL0), &val);
	if (ret || (val & (DMA_ENABLE | DMA_IRQ_ENABLE))) {
		ret = ret ?: -EIO;
		goto fail;
	}
	ret = regmap_write(a->map, s7d_audio_frddr(a, FRDDR_CTRL2), 0);
	if (ret)
		goto fail;
	ret = regmap_update_bits(a->map, a->path->sink_ctrl, BIT(31) | GENMASK(29, 28), 0);
	if (ret)
		goto fail;
	a->running = false;
	a->prepared = false;
	return 0;
fail:
	a->faulted = true;
	dev_err_ratelimited(a->dev, "DMA stop failed: %d; retaining resources\n", ret);
	return ret;
}

static irqreturn_t s7d_audio_irq(int irq, void *data)
{
	struct s7d_audio *a = data;
	struct snd_pcm_substream *substream;
	unsigned long flags;
	u32 status = 0;
	int ret;

	spin_lock_irqsave(&a->lock, flags);
	if (!a->powered) {
		spin_unlock_irqrestore(&a->lock, flags);
		return IRQ_NONE;
	}
	ret = regmap_read(a->map, s7d_audio_frddr(a, FRDDR_STATUS), &status);
	status &= 0xff;
	if (ret || !status) {
		spin_unlock_irqrestore(&a->lock, flags);
		return IRQ_NONE;
	}
	regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL1), 0xff, status);
	regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL1), 0xff, 0);
	substream = a->running ? a->substream : NULL;
	spin_unlock_irqrestore(&a->lock, flags);
	if (substream && (status & DMA_PERIOD_IRQ))
		snd_pcm_period_elapsed(substream);
	return IRQ_HANDLED;
}

static const struct snd_pcm_hardware s7d_audio_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min = 44100,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES,
	.period_bytes_min = 256,
	.period_bytes_max = BUFFER_BYTES / 2,
	.periods_min = 2,
	.periods_max = BUFFER_BYTES / 256,
};

static int s7d_audio_power_get(struct s7d_audio *a)
{
	int ret;

	ret = pm_runtime_resume_and_get(a->dev);
	if (ret < 0)
		return ret;
	ret = clk_bulk_prepare_enable(3, a->clocks);
	if (ret) {
		pm_runtime_put_sync(a->dev);
		return ret;
	}
	a->powered = true;
	return 0;
}

static void s7d_audio_power_put(struct s7d_audio *a)
{
	a->powered = false;
	clk_bulk_disable_unprepare(3, a->clocks);
	pm_runtime_put_sync(a->dev);
}

static int s7d_audio_open(struct snd_soc_component *component,
			struct snd_pcm_substream *substream)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	int ret;

	if (a->faulted)
		return -EIO;
	if (a->substream)
		return -EBUSY;
	ret = snd_soc_set_runtime_hwparams(substream, &s7d_audio_hw);
	if (ret)
		return ret;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_PERIOD_BYTES, BURST_BYTES);
	if (ret)
		return ret;
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_BUFFER_BYTES, BURST_BYTES);
	if (ret)
		return ret;
	ret = snd_pcm_hw_constraint_integer(substream->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	ret = s7d_audio_power_get(a);
	if (ret)
		return ret;
	a->substream = substream;
	return 0;
}

static int s7d_audio_hw_free(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&a->lock, flags);
	ret = s7d_audio_stop(a);
	spin_unlock_irqrestore(&a->lock, flags);
	if (ret)
		return ret;
	ret = s7d_audio_route_clocks(a, false);
	if (!ret && a->path->output != S7D_AUDIO_SPDIF)
		ret = regmap_write(a->map, a->path->sink_clock, 0);
	if (ret) {
		a->faulted = true;
		return ret;
	}
	if (a->sample_clocks) {
		clk_bulk_disable_unprepare(a->path->sample_clocks, &a->clocks[3]);
		a->sample_clocks = false;
	}
	if (a->exclusive) {
		clk_rate_exclusive_put(a->clocks[3].clk);
		a->exclusive = false;
	}
	s7d_audio_spdif_clock_put(a);
	snd_pcm_set_runtime_buffer(substream, NULL);
	return 0;
}

static int s7d_audio_close(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int ret;

	ret = s7d_audio_hw_free(component, substream);
	spin_lock_irqsave(&a->lock, flags);
	a->substream = NULL;
	spin_unlock_irqrestore(&a->lock, flags);
	synchronize_irq(a->irq);
	if (!ret)
		s7d_audio_power_put(a);
	return ret;
}

static int s7d_audio_hw_params(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	unsigned long rate = params_rate(params);
	unsigned long rates[] = { rate * 256, rate * 64, rate, rate * 128 };
	unsigned int count = a->path->sample_clocks;
	int i, ret;

	if (!a->buffer || a->faulted)
		return -EIO;
	if (a->sample_clocks || a->exclusive)
		return -EBUSY;
	if (a->path->output == S7D_AUDIO_SPDIF) {
		rates[0] = rate * 128;
		ret = s7d_audio_spdif_clock_get(a, rate);
		if (ret)
			return ret;
	}
	ret = clk_set_rate_exclusive(a->clocks[3].clk, rates[0]);
	if (ret)
		goto fail_pll;
	a->exclusive = true;
	for (i = 1; i < count; i++) {
		ret = clk_set_rate(a->clocks[3 + i].clk, rates[i]);
		if (ret)
			goto fail;
	}
	for (i = 0; i < count; i++) {
		if (clk_get_rate(a->clocks[3 + i].clk) != rates[i]) {
			ret = -ERANGE;
			goto fail;
		}
	}
	if (a->path->output != S7D_AUDIO_SPDIF) {
		ret = clk_set_duty_cycle(a->clocks[5].clk, 1, 2);
		if (ret)
			goto fail;
		ret = clk_set_phase(a->clocks[4].clk, 0);
		if (ret)
			goto fail;
		ret = clk_set_phase(a->clocks[5].clk, 180);
		if (ret)
			goto fail;
	}
	ret = clk_bulk_prepare_enable(count, &a->clocks[3]);
	if (ret)
		goto fail;
	a->sample_clocks = true;
	snd_pcm_set_runtime_buffer(substream, a->buffer);
	substream->runtime->dma_bytes = params_buffer_bytes(params);
	return 0;
fail:
	clk_rate_exclusive_put(a->clocks[3].clk);
	a->exclusive = false;
fail_pll:
	s7d_audio_spdif_clock_put(a);
	return ret;
}

static int s7d_audio_prepare_sink(struct s7d_audio *a,
		struct snd_pcm_runtime *runtime, unsigned int width)
{
	const struct s7d_audio_path *p = a->path;
	unsigned int base = p->sink_ctrl;
	const struct reg_sequence tdm_sequence[] = {
		{ base, (2 << 15) | (1 << 5) | 31 },
		{ base + TDM_CTRL1, BIT(28) | (p->fifo << 24) |
			((width - 1) << 8) | ((width == 16 ? 2 : 4) << 4) },
		{ p->sink_ctrl2, 0 },
		{ base + TDM_SWAP, 0x76543210 },
		{ base + TDM_MASK0, 3 },
		{ base + TDM_MASK0 + 4, 0 },
		{ base + TDM_MASK0 + 8, 0 },
		{ base + TDM_MASK0 + 12, 0 },
		{ p->gain_enable, 0 },
		{ p->sink_clock, BIT(31) | BIT(30) | BIT(28) |
			(p->sink << 24) | (p->sink << 20) },
	};
	int ret;

	if (p->output == S7D_AUDIO_SPDIF) {
		u8 cs[24];
		unsigned int i, channel;
		const struct reg_sequence sequence[] = {
			{ base, 3 },
			{ base + SPDIF_CTRL1, (p->fifo << 24) |
				((width - 1) << 8) | ((width == 16 ? 1 : 4) << 4) },
			{ base + SPDIF_SWAP, 0x10 },
			{ p->gain_enable, 0 },
		};

		ret = snd_pcm_create_iec958_consumer(runtime, cs, sizeof(cs));
		if (ret < 0)
			return ret;
		ret = regmap_multi_reg_write(a->map, sequence, ARRAY_SIZE(sequence));
		if (ret)
			return ret;
		for (channel = 0; channel < 2; channel++) {
			for (i = 0; i < 6; i++) {
				ret = regmap_write(a->map, base + SPDIF_CHSTS0 +
					(channel * 6 + i) * 4, get_unaligned_le32(cs + i * 4));
				if (ret)
					return ret;
			}
		}
		return 0;
	}

	ret = regmap_multi_reg_write(a->map, tdm_sequence, ARRAY_SIZE(tdm_sequence));
	if (ret || p->output != S7D_AUDIO_ANALOG)
		return ret;
	ret = regmap_write(a->map, TOACODEC_ROUTE,
			(17 << 22) | (16 << 16) | (2 << 12) | BIT(9) | (2 << 4) | 2);
	if (ret)
		return ret;
	return s7d_audio_route_clocks(a, true);
}

static int s7d_audio_prepare(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int period = frames_to_bytes(runtime, runtime->period_size);
	unsigned int threshold = min(period, FIFO_BYTES) / 2;
	unsigned int width = snd_pcm_format_width(runtime->format);
	const struct reg_sequence fifo_sequence[] = {
		{ s7d_audio_frddr(a, FRDDR_CTRL0), BIT(0) },
		{ s7d_audio_frddr(a, FRDDR_CTRL1), ((FIFO_BYTES / BURST_BYTES - 1) << 24) |
			((threshold / BURST_BYTES - 1) << 16) | (2 << 8) },
		{ s7d_audio_frddr(a, FRDDR_START), lower_32_bits(runtime->dma_addr) },
		{ s7d_audio_frddr(a, FRDDR_END),
		  lower_32_bits(runtime->dma_addr + runtime->dma_bytes - BURST_BYTES) },
		{ s7d_audio_frddr(a, FRDDR_INIT), lower_32_bits(runtime->dma_addr) },
		{ s7d_audio_frddr(a, FRDDR_PERIOD), period / BURST_BYTES },
		{ s7d_audio_frddr(a, FRDDR_CTRL2), BIT(24) | a->path->sink },
	};
	unsigned long flags;
	int ret;

	if (!a->sample_clocks || a->faulted)
		return -EIO;
	spin_lock_irqsave(&a->lock, flags);
	ret = s7d_audio_stop(a);
	spin_unlock_irqrestore(&a->lock, flags);
	if (ret)
		return ret;
	ret = reset_control_bulk_assert(a->path->num_resets, a->resets);
	if (ret)
		return ret;
	udelay(2);
	ret = reset_control_bulk_deassert(a->path->num_resets, a->resets);
	if (ret)
		return ret;
	ret = regmap_update_bits(a->map, ARB_CTRL, BIT(31) | BIT(a->path->fifo + 4),
				 BIT(31) | BIT(a->path->fifo + 4));
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(a->map, fifo_sequence, ARRAY_SIZE(fifo_sequence));
	if (ret)
		return ret;
	ret = s7d_audio_prepare_sink(a, runtime, width);
	if (ret)
		return ret;
	ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL1), BIT(12), BIT(12));
	if (ret)
		return ret;
	ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL1), BIT(12) | 0xff, 0xff);
	if (ret)
		return ret;
	ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL1), 0xff, 0);
	if (ret)
		return ret;
	if (a->path->output != S7D_AUDIO_HDMI) {
		a->prepared = true;
		return 0;
	}
	/* CCF owns HDMI MCLK fields [27:16]. */
	ret = regmap_update_bits(a->map, HDMI_ROUTE,
				GENMASK(31, 28) | GENMASK(13, 0),
				HDMI_CLOCK | BIT(12) | BIT(8) | BIT(7) | BIT(4));
	if (!ret)
		a->prepared = true;
	return ret;
}

static int s7d_audio_trigger(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream, int cmd)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&a->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!a->prepared || a->faulted) {
			ret = -EIO;
			break;
		}
		ret = regmap_update_bits(a->map, a->path->sink_ctrl, BIT(29), BIT(29));
		if (!ret)
			ret = regmap_update_bits(a->map, a->path->sink_ctrl, BIT(28), BIT(28));
		if (!ret)
			ret = regmap_update_bits(a->map, a->path->sink_ctrl, BIT(31), BIT(31));
		if (!ret) {
			a->running = true;
			ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL0),
						DMA_ENABLE | DMA_IRQ_ENABLE,
						DMA_ENABLE | (DMA_PERIOD_IRQ << 16));
		}
		if (!ret)
			ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL2),
						 BIT(4), BIT(4));
		if (!ret)
			ret = s7d_audio_route_data(a, true);
		if (ret)
			s7d_audio_stop(a);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ret = s7d_audio_stop(a);
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&a->lock, flags);
	return ret;
}

static snd_pcm_uframes_t s7d_audio_pointer(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 addr, offset;

	if (regmap_read(a->map, s7d_audio_frddr(a, FRDDR_POSITION), &addr))
		return SNDRV_PCM_POS_XRUN;
	offset = addr - lower_32_bits(runtime->dma_addr);
	if (offset == runtime->dma_bytes)
		return 0;
	if (offset > runtime->dma_bytes)
		return SNDRV_PCM_POS_XRUN;
	return bytes_to_frames(runtime, offset);
}

static int s7d_audio_pcm_new(struct snd_soc_component *component,
			     struct snd_soc_pcm_runtime *rtd)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	struct snd_dma_buffer *buffer;
	int ret;

	if (a->buffer)
		return -EBUSY;
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, a->dev, BUFFER_BYTES, buffer);
	if (ret) {
		kfree(buffer);
		return ret;
	}
	a->buffer = buffer;
	return 0;
}

static void s7d_audio_pcm_free(struct snd_soc_component *component,
			      struct snd_pcm *pcm)
{
	struct s7d_audio *a = snd_soc_component_get_drvdata(component);
	unsigned long flags;

	if (!a->buffer)
		return;
	spin_lock_irqsave(&a->lock, flags);
	s7d_audio_stop(a);
	spin_unlock_irqrestore(&a->lock, flags);
	if (a->faulted) {
		dev_err(a->dev, "quarantining DMA buffer until reboot\n");
		return;
	}
	snd_dma_free_pages(a->buffer);
	kfree(a->buffer);
	a->buffer = NULL;
}

static int s7d_audio_mmap(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream,
			  struct vm_area_struct *vma)
{
	return snd_pcm_lib_default_mmap(substream, vma);
}

static int s7d_audio_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;
	return 0;
}

static const struct snd_soc_dai_ops s7d_audio_dai_ops = {
	.set_fmt = s7d_audio_set_fmt,
};

static const struct snd_soc_dai_driver s7d_audio_dai = {
	.name = "s7d-tdmb",
	.playback = {
		.stream_name = "HDMI Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
	.ops = &s7d_audio_dai_ops,
};

static const struct snd_soc_component_driver s7d_audio_component = {
	.name = "s7d-hdmi-audio",
	.pcm_construct = s7d_audio_pcm_new,
	.pcm_destruct = s7d_audio_pcm_free,
	.open = s7d_audio_open,
	.close = s7d_audio_close,
	.hw_params = s7d_audio_hw_params,
	.hw_free = s7d_audio_hw_free,
	.prepare = s7d_audio_prepare,
	.trigger = s7d_audio_trigger,
	.pointer = s7d_audio_pointer,
	.mmap = s7d_audio_mmap,
};

static int s7d_audio_probe(struct platform_device *pdev)
{
	static const char * const names[] = {
		"arb", "fifo", "tdm", "mclk", "bclk", "lrclk", "hdmi-mclk",
	};
	struct device *dev = &pdev->dev;
	struct s7d_audio *a;
	int i, ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	a->path = of_device_get_match_data(dev);
	if (!a->path)
		return -EINVAL;
	a->dai = s7d_audio_dai;
	a->dai.name = a->path->dai_name;
	a->dai.playback.stream_name = a->path->stream_name;
	if (a->path->output == S7D_AUDIO_SPDIF)
		a->dai.ops = NULL;
	a->map = dev_get_regmap(dev->parent, NULL);
	if (!a->map)
		return -EPROBE_DEFER;
	spin_lock_init(&a->lock);
	platform_set_drvdata(pdev, a);
	for (i = 0; i < 3 + a->path->sample_clocks; i++)
		a->clocks[i].id = names[i];
	ret = devm_clk_bulk_get(dev, 3 + a->path->sample_clocks, a->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get audio clocks\n");
	if (a->path->output == S7D_AUDIO_SPDIF) {
		static const char * const source_names[] = {
			"mclk-sel", "pll-44k", "pll-48k",
		};

		for (i = 0; i < ARRAY_SIZE(source_names); i++)
			a->spdif_clocks[i].id = source_names[i];
		ret = devm_clk_bulk_get(dev, ARRAY_SIZE(a->spdif_clocks), a->spdif_clocks);
		if (ret)
			return dev_err_probe(dev, ret, "failed to get SPDIF sources\n");
	}
	a->resets[0].id = "fifo";
	a->resets[1].id = a->path->output == S7D_AUDIO_SPDIF ? "spdif" : "tdm";
	a->resets[2].id = "route";
	ret = devm_reset_control_bulk_get_exclusive(dev, a->path->num_resets, a->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get audio resets\n");
	a->irq = platform_get_irq(pdev, 0);
	if (a->irq < 0)
		return a->irq;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = s7d_audio_power_get(a);
	if (ret)
		return ret;
	ret = s7d_audio_stop(a);
	if (ret)
		return ret;
	ret = reset_control_bulk_deassert(a->path->num_resets, a->resets);
	if (!ret)
		ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL1), 0xff, 0xff);
	if (!ret)
		ret = regmap_update_bits(a->map, s7d_audio_frddr(a, FRDDR_CTRL1), 0xff, 0);
	s7d_audio_power_put(a);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, a->irq, s7d_audio_irq, 0, dev_name(dev), a);
	if (ret)
		return ret;
	return devm_snd_soc_register_component(dev, &s7d_audio_component, &a->dai, 1);
}

static const struct of_device_id s7d_audio_of_match[] = {
	{ .compatible = "amlogic,s7d-hdmi-audio", .data = &s7d_hdmi_path },
	{ .compatible = "amlogic,s7d-analog-audio", .data = &s7d_analog_path },
	{ .compatible = "amlogic,s7d-spdif-audio", .data = &s7d_spdif_path },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_audio_of_match);

static struct platform_driver s7d_audio_driver = {
	.probe = s7d_audio_probe,
	.driver = {
		.name = "s7d-hdmi-audio",
		.of_match_table = s7d_audio_of_match,
	},
};
module_platform_driver(s7d_audio_driver);

MODULE_DESCRIPTION("Amlogic S7D HDMI, analog and SPDIF PCM transports");
MODULE_LICENSE("GPL");
