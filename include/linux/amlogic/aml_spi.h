/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __AML_SPI_H_
#define __AML_SPI_H_

#ifdef CONFIG_ARCH_MESON_ODROID_COMMON
#include <linux/atomic.h>

#endif
#define SPI_INVALID_CS		((s8)-1)
#define MESON_SPI_CS_CNT_MAX	4
#define SPICC_DEFAULT_CS	0

enum {
	DMA_TRIG_NORMAL = 0,
	DMA_TRIG_PWM,
	DMA_TRIG_PWM_VS,
	DMA_TRIG_VSYNC,
	DMA_TRIG_LINE_N,
	DMA_TRIG_GPIO,
};

enum {
	SPICC_V1 = 1,
	SPICC_V2,
};

#define MESON_SPICC_HW_IF

#define CONTROLLER_SPICC	1
#define CONTROLLER_SPISG	2

#define CAP_DMA_TRIG_PWM	BIT(0)
#define CAP_DMA_TRIG_PWM_VS	BIT(1)
#define CAP_DMA_TRIG_VSYNC	BIT(2)
#define CAP_DMA_TRIG_LINE_N	BIT(3)
#define CAP_DMA_TRIG_GPIO	BIT(4)
#define CAP_TRIG_DELAY		BIT(5)

#define DC_MODE_NONE		0
#define DC_MODE_PIN		1
#define DC_MODE_9BIT		2

#define DC_LEVEL_LOW		0
#define DC_LEVEL_HIGH		1

struct spicc_controller_data {
	unsigned int	controller_version;
	unsigned int	controller_capabilities;
	unsigned	use_ctrl_cs:1;
	unsigned	use_dirspi:1;
	unsigned	ccxfer_en:1;
	unsigned	timing_en:1;
	unsigned	ss_leading_gap:16;
	unsigned	ss_trailing_gap:15;
	unsigned	tx_tuning:4;
	unsigned	rx_tuning:4;
	unsigned	dummy_ctl:1;
	unsigned	word_gap:2;
	unsigned	mosi_idle_level:2;
	unsigned	dc_mode:2;
	unsigned	dc_level:1;
	unsigned	read_turn_around:8;
	unsigned	miso_latency_en:1;
	unsigned	split_xfer_en:1;
	int		miso_latency; // in ns, signed
	unsigned int	dma_trig_delay;
	unsigned int	pclk_rate;
	void *priv;
	void (*dirspi_start)(struct spi_device *spi);
	void (*dirspi_stop)(struct spi_device *spi);
	int (*dirspi_async)(struct spi_device *spi,
			    dma_addr_t tx_dma,
			    dma_addr_t rx_dma,
			    int len,
			    void (*complete)(void *context),
			    void *context);
	int (*dirspi_sync)(struct spi_device *spi,
			   dma_addr_t tx_dma,
			   dma_addr_t rx_dma,
			   int len);
	int (*dirspi_xfer)(struct spi_device *spi,
			   u8 *tx_buf,
			   u8 *rx_buf,
			   int len);
	int (*dirspi_dma_trig)(struct spi_device *spi,
			       dma_addr_t tx_dma,
			       dma_addr_t rx_dma,
			       int len,
			       u8 src);
	int (*dirspi_dma_trig_start)(struct spi_device *spi);
	int (*dirspi_dma_trig_stop)(struct spi_device *spi);
	int (*dirspi_dma_trig_release)(struct spi_device *spi);
	int (*dirspi_busy_proc)(struct spi_device *spi);
	bool (*dmatrig_is_busy)(struct spi_device *spi);
};

static inline bool is_spicc_capable(struct spicc_controller_data *cdata, unsigned int cap_type)
{
	return cdata->controller_capabilities & cap_type;
}

#ifdef CONTROLLER_SPICC
struct meson_spicc_device {
	struct spi_controller		*controller;
	struct platform_device		*pdev;
	void __iomem			*base;
	struct pinctrl *pinctrl;
	struct pinctrl_state *default_state;
	struct pinctrl_state *ctrl_cs;
	struct pinctrl_state *standby;
	struct clk			*core;
#ifdef CONFIG_AMLOGIC_MODIFY
	struct clk			*async_clk;
	struct clk			*clk;
	struct meson_spicc_data	*data;
	unsigned int			speed_hz;
	unsigned int			bits_per_word;
	unsigned int			wordmode;
	unsigned int			endian;
	struct				class cls;
	bool				using_dma;
	struct spi_device		*spi;
	bool				tx_dma_mapped;
	bool				rx_dma_mapped;
	void				(*complete)(void *context);
	void				*context;
	s32				miso_latency_default;   // in pclk, signed
	s32				miso_latency_applied;	// in ns, signed
	unsigned int			cs_setup_default;	// in ns
	unsigned int			cs_setup_applied;	// in ns
	unsigned int			cs_hold_default;	// in ns
	unsigned int			cs_hold_applied;	// in ns
	unsigned int			pclk_rate;
	bool				parent_clk_fixed;
	bool				clk_div_none;
	bool				toggle_cs_every_word;
	struct gpio_desc	**cs_gpiods;
#endif
	//struct spi_message		*message;
	struct spi_transfer		*xfer;
	struct spi_transfer		*orig_xfer;
	struct spi_transfer		dma_xfer;
	struct spi_transfer		pio_xfer;
	u8				*tx_buf;
	u8				*rx_buf;
	unsigned int			bytes_per_word;
	unsigned long			tx_remain;
	unsigned long			rx_remain;
	unsigned int			*store_buf;
	spinlock_t			lock; /* dirspi_xfer in interrupt */
	struct reset_control		*rst;
	bool				pending;
	int				dirspi_status;
};
#endif // CONTROLLER_SPICC

#ifdef CONTROLLER_SPISG
union spicc_cfg_spi {
	u32			d32;
	struct  {
		u32		bus64_en:1;
		u32		slave_en:1;
		u32		ss:2;
		u32		flash_wp_pin_en:1;
		u32		flash_hold_pin_en:1;
		u32		hw_pos:1; /* start on vsync rising */
		u32		hw_neg:1; /* start on vsync falling */
		u32		word_gap:2;
		u32		mosi_idle_level:2;
		u32		cs_hold:15;
		u32		cs_setup_extend:4;
		u32		rsv:1;
	} b;
};

union spicc_cfg_start {
	u32			d32;
	struct  {
		u32		block_num:20;
#define SPICC_BLOCK_MAX			0x100000

		u32		block_size:3;
		u32		dc_level:1;
#define SPICC_DC_DATA0_CMD1		0
#define SPICC_DC_DATA1_CMD0		1

		u32		op_mode:2;
#define SPICC_OP_MODE_WRITE_CMD		0
#define SPICC_OP_MODE_READ_STS		1
#define SPICC_OP_MODE_WRITE		2
#define SPICC_OP_MODE_READ		3

		u32		rx_data_mode:2;
		u32		tx_data_mode:2;
#define SPICC_DATA_MODE_NONE		0
#define SPICC_DATA_MODE_PIO		1
#define SPICC_DATA_MODE_MEM		2
#define SPICC_DATA_MODE_SG		3

		u32		eoc:1;
		u32		pending:1;
	} b;
};

union spicc_cfg_bus {
	u32			d32;
	struct  {
		u32		clk_div:8;
#define SPICC_CLK_DIV_SHIFT	0
#define SPICC_CLK_DIV_WIDTH	8
#define SPICC_CLK_DIV_MASK	GENMASK(7, 0)
#define SPICC_CLK_DIV_MAX	256
#define SPICC_CLK_DIV_MIN	4 /* recommended by specification */

		/* signed, -8~-1 early, 1~7 later */
		u32		rx_tuning:4;
		u32		tx_tuning:4;

		u32		cs_setup:4;
		u32		lane:2;
#define SPICC_SINGLE_SPI		0
#define SPICC_DUAL_SPI			1
#define SPICC_QUAD_SPI			2

		u32		half_duplex_en:1;
		u32		little_endian_en:1;
		u32		dc_mode:1;
#define SPICC_DC_MODE_PIN		0
#define SPICC_DC_MODE_9BIT		1

		u32		null_ctl:1;
		u32		dummy_ctl:1;
		u32		read_turn_around:2;
#define SPICC_READ_TURN_AROUND_MAX	3

		u32		keep_ss:1;
		u32		cpha:1;
		u32		cpol:1;
	} b;
};

struct spicc_descriptor {
	union spicc_cfg_start		cfg_start;
	union spicc_cfg_bus		cfg_bus;
	u64				tx_paddr;
	u64				rx_paddr;
};

struct meson_spicc_v2_data {
	bool				idle_ctrl;
	bool				cs_hold_ctrl;
	bool				cs_setup_extend_ctrl;
	bool				clk_gate_ctrl;
	bool				cs_hold_ctrl_conflict_with_keep_ss;
};

struct spicc_device {
	struct spi_controller		*controller;
	struct platform_device		*pdev;
	void __iomem			*base;
	struct pinctrl	*pinctrl;
	struct pinctrl_state	*spi_pinmux;
	struct clk			*sys_clk;
	struct clk			*spi_clk;
	struct clk			*sclk;
	struct meson_spicc_v2_data		*data;
	struct completion		completion;
	u32				status;
	u32			speed_hz;
	u32				actual_speed_hz;
	u32				pclk_rate;
	u32			bytes_per_word;
	union spicc_cfg_spi		cfg_spi;
	union spicc_cfg_start		cfg_start;
	union spicc_cfg_bus		cfg_bus;
	struct spi_delay		cs_setup;
	struct spi_delay		cs_hold;
	int				cs_hold_min_in_pclk;
	bool				ready;
	bool				keep_ready;
	bool				pending;
	spinlock_t			lock; /* for unexpect interrupt */
#ifdef CONFIG_ARCH_MESON_ODROID_COMMON
	atomic_t			transfer_active;
#endif
#ifdef MESON_SPICC_HW_IF
	void (*dirspi_complete)(void *context);
	void *dirspi_context;
	void __iomem			*trig_reg;
	/* in select of pwm/pwm_vs/vsync/line_n/gpio */
	unsigned int			trig_in_selects[5];
	struct spicc_descriptor		*dirspi_desc;
	dma_addr_t			dirspi_desc_paddr;
	int				dirspi_desc_len;
	int				dirspi_status;
#define DIRSPI_STA_IDLE		0
#define DIRSPI_STA_READY	1
#define DIRSPI_STA_RUNNING	2
#endif
};
#endif // CONTROLLER_SPISG

#endif
