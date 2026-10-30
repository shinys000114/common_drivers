// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 * Copyright (C) 2019 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 * Copyright (c) 2026 Hardkernel Co., Ltd.
 *
 * Adapted from the Meson G12A CECB protocol driver, with S7D resources.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <media/cec.h>
#include <media/cec-notifier.h>
#if IS_ENABLED(CONFIG_CEC_MESON_S7D_AOCPU)
#include <linux/amlogic/aml_mbox.h>
#endif

#define CECB_GEN		0x00
#define CECB_RW			0x04
#define CECB_IRQ_MASK		0x08
#define CECB_IRQ_CLEAR		0x0c
#define CECB_IRQ_STATUS		0x10
#define GEN_RESET		BIT(0)
#define GEN_CLOCK		GENMASK(2, 1)
#define GEN_SYS_CLOCK		BIT(3)
#define GEN_FILTER_TICK		GENMASK(9, 8)
#define GEN_FILTER_DELAY	GENMASK(14, 12)
#define RW_WRITE		BIT(16)
#define RW_BUSY			BIT(23)
#define RW_DATA			GENMASK(31, 24)
#define IRQ_DONE		BIT(0)
#define IRQ_RX			BIT(1)
#define IRQ_NACK		BIT(2)
#define IRQ_ARB_LOST		BIT(3)
#define IRQ_TX_ERROR		BIT(4)
#define IRQ_RX_ERROR		BIT(5)
#define IRQ_WAKE		BIT(6)
#define IRQ_TX			(IRQ_DONE | IRQ_NACK | IRQ_ARB_LOST | IRQ_TX_ERROR)
#define IRQ_ENABLED		(IRQ_TX | IRQ_RX | IRQ_RX_ERROR)
#define IRQ_ALL			(IRQ_ENABLED | IRQ_WAKE)

#define CEC_CTRL		0x00
#define CTRL_SEND		BIT(0)
#define CTRL_TYPE		GENMASK(2, 1)
#define CEC_CTRL2		0x01
#define CEC_LADDR_LOW		0x05
#define CEC_LADDR_HIGH		0x06
#define CEC_TX_COUNT		0x07
#define CEC_RX_COUNT		0x08
#define CEC_TX_DATA		0x10
#define CEC_RX_DATA		0x20
#define CEC_RX_LOCK		0x30
#define CEC_WAKE_CONTROL	0x31

/*
 * While the system sleeps or is powered off, the AOCPU firmware (BL30) runs
 * the controller. It reads its configuration from SYSCTRL_STATUS_REG0/1 and
 * records the wake source in SYSCTRL_STICKY_REG7.
 */
#define AO_CEC_CONFIG		0x0
#define AO_CEC_ON		BIT(0)
#define AO_CEC_AUTO_POWER_ON	BIT(3)
#define AO_CEC_ADDRESS		0x4
#define AO_CEC_PHYS_ADDR	GENMASK(15, 0)
#define AO_CEC_LOG_ADDR		GENMASK(19, 16)
#define AO_CEC_DEV_TYPE		GENMASK(23, 20)
#define WAKE_REASON		GENMASK(6, 0)
#define WAKE_REASON_CEC		8

struct s7d_cec {
	struct device *dev;
	void __iomem *base;
	void __iomem *handoff;
	void __iomem *wake_reason;
#if IS_ENABLED(CONFIG_CEC_MESON_S7D_AOCPU)
	struct mbox_client mbox_client;
	struct mbox_chan *mbox;
#endif
	struct clk_bulk_data clocks[2];
	struct reset_control *reset;
	struct cec_adapter *adap;
	struct cec_notifier *notifier;
	/* CEC core callbacks take adap->lock before this indirect-access lock. */
	struct mutex lock;
	u16 logical_mask;
	int irq;
	bool enabled;
	bool running;
	bool transmitting;
	bool suspended;
	bool stopping;
	bool handed_off;
};

static int s7d_cec_wait(struct s7d_cec *cec, u32 *value)
{
	return readl_poll_timeout(cec->base + CECB_RW, *value,
				 !(*value & RW_BUSY), 5, 1000);
}

static int s7d_cec_read(struct s7d_cec *cec, u8 reg, u8 *value)
{
	u32 data;
	int ret;

	lockdep_assert_held(&cec->lock);
	ret = s7d_cec_wait(cec, &data);
	if (ret)
		return ret;
	writel(reg, cec->base + CECB_RW);
	ret = s7d_cec_wait(cec, &data);
	if (!ret)
		*value = FIELD_GET(RW_DATA, data);
	return ret;
}

static int s7d_cec_write(struct s7d_cec *cec, u8 reg, u8 value)
{
	u32 data;
	int ret;

	lockdep_assert_held(&cec->lock);
	ret = s7d_cec_wait(cec, &data);
	if (ret)
		return ret;
	writel(reg | (value << 8) | RW_WRITE, cec->base + CECB_RW);
	return s7d_cec_wait(cec, &data);
}

static void s7d_cec_stop(struct s7d_cec *cec)
{
	lockdep_assert_held(&cec->lock);
	writel(0, cec->base + CECB_IRQ_MASK);
	writel(GEN_RESET, cec->base + CECB_GEN);
	writel(IRQ_ALL, cec->base + CECB_IRQ_CLEAR);
	cec->transmitting = false;
	cec->running = false;
}

static int s7d_cec_start(struct s7d_cec *cec)
{
	u32 control = GEN_RESET | GEN_SYS_CLOCK | FIELD_PREP(GEN_FILTER_TICK, 1) |
		      FIELD_PREP(GEN_FILTER_DELAY, 7);
	int ret;

	s7d_cec_stop(cec);
	writel(control, cec->base + CECB_GEN);
	control |= FIELD_PREP(GEN_CLOCK, 1);
	writel(control, cec->base + CECB_GEN);
	writel(control & ~GEN_RESET, cec->base + CECB_GEN);
	/* S7D uses rise-delay 8, not the SM1 value of 2. */
	ret = s7d_cec_write(cec, CEC_CTRL2, 8);
	if (ret)
		goto stop;
	ret = s7d_cec_write(cec, CEC_WAKE_CONTROL, 0);
	if (ret)
		goto stop;
	ret = s7d_cec_write(cec, CEC_LADDR_LOW, cec->logical_mask);
	if (ret)
		goto stop;
	ret = s7d_cec_write(cec, CEC_LADDR_HIGH, cec->logical_mask >> 8);
	if (ret)
		goto stop;
	ret = s7d_cec_write(cec, CEC_RX_LOCK, 0);
	if (ret)
		goto stop;
	writel(IRQ_ALL, cec->base + CECB_IRQ_CLEAR);
	writel(IRQ_ENABLED, cec->base + CECB_IRQ_MASK);
	cec->running = true;
	return 0;
stop:
	s7d_cec_stop(cec);
	return ret;
}

static int s7d_cec_enable(struct cec_adapter *adap, bool enable)
{
	struct s7d_cec *cec = adap->priv;
	int ret = 0;

	mutex_lock(&cec->lock);
	if (enable && (cec->suspended || cec->stopping)) {
		ret = -EHOSTDOWN;
	} else if (enable) {
		ret = s7d_cec_start(cec);
		if (!ret)
			cec->enabled = true;
	} else {
		cec->enabled = false;
		s7d_cec_stop(cec);
	}
	mutex_unlock(&cec->lock);
	return ret;
}

static int s7d_cec_log_addr(struct cec_adapter *adap, u8 address)
{
	struct s7d_cec *cec = adap->priv;
	u16 mask;
	int ret;

	mutex_lock(&cec->lock);
	mask = address == CEC_LOG_ADDR_INVALID ? 0 :
		cec->logical_mask | BIT(address) | BIT(CEC_LOG_ADDR_UNREGISTERED);
	if (!cec->running || cec->suspended || cec->stopping) {
		cec->logical_mask = mask;
		ret = 0;
		goto out;
	}
	ret = s7d_cec_write(cec, CEC_LADDR_LOW, mask);
	if (!ret)
		ret = s7d_cec_write(cec, CEC_LADDR_HIGH, mask >> 8);
	if (!ret)
		cec->logical_mask = mask;
	else
		s7d_cec_stop(cec);
out:
	mutex_unlock(&cec->lock);
	return ret;
}

static int s7d_cec_transmit(struct cec_adapter *adap, u8 attempts,
			    u32 signal_free_time, struct cec_msg *msg)
{
	struct s7d_cec *cec = adap->priv;
	u8 value, type;
	int i, ret;

	mutex_lock(&cec->lock);
	if (!cec->enabled || cec->suspended || cec->stopping) {
		ret = -EHOSTDOWN;
		goto out;
	}
	/* A new core request after a timeout must not inherit the old transfer. */
	if (!cec->running || cec->transmitting) {
		ret = s7d_cec_start(cec);
		if (ret)
			goto out;
	}
	ret = s7d_cec_read(cec, CEC_RX_LOCK, &value);
	if (ret)
		goto out;
	if (value & BIT(0)) {
		ret = -EBUSY;
		goto out;
	}
	ret = s7d_cec_read(cec, CEC_CTRL, &value);
	if (ret)
		goto out;
	if (value & CTRL_SEND) {
		ret = -EBUSY;
		goto out;
	}
	if (signal_free_time == CEC_SIGNAL_FREE_TIME_RETRY)
		type = 0;
	else if (signal_free_time == CEC_SIGNAL_FREE_TIME_NEXT_XFER)
		type = 2;
	else
		type = 1;
	for (i = 0; i < msg->len; i++) {
		ret = s7d_cec_write(cec, CEC_TX_DATA + i, msg->msg[i]);
		if (ret)
			goto out;
	}
	ret = s7d_cec_write(cec, CEC_TX_COUNT, msg->len);
	if (ret)
		goto out;
	writel(IRQ_TX, cec->base + CECB_IRQ_CLEAR);
	ret = s7d_cec_write(cec, CEC_CTRL,
			    (value & ~(CTRL_SEND | CTRL_TYPE)) |
			    CTRL_SEND | FIELD_PREP(CTRL_TYPE, type));
	if (!ret)
		cec->transmitting = true;
	else
		s7d_cec_stop(cec);
out:
	mutex_unlock(&cec->lock);
	return ret;
}

static u8 s7d_cec_tx_status(u32 status)
{
	if (status & IRQ_ARB_LOST)
		return CEC_TX_STATUS_ARB_LOST;
	if (status & IRQ_TX_ERROR)
		return CEC_TX_STATUS_ERROR;
	if (status & IRQ_NACK)
		return CEC_TX_STATUS_NACK;
	return CEC_TX_STATUS_OK;
}

static irqreturn_t s7d_cec_irq(int irq, void *data)
{
	struct s7d_cec *cec = data;
	struct cec_msg msg = {};
	u32 status;
	u8 value, tx_status = 0;
	int i, ret = 0;

	mutex_lock(&cec->lock);
	status = readl(cec->base + CECB_IRQ_STATUS) & IRQ_ALL;
	if (!status) {
		mutex_unlock(&cec->lock);
		return IRQ_NONE;
	}
	writel(status, cec->base + CECB_IRQ_CLEAR);
	if (!cec->running || cec->suspended || cec->stopping)
		goto out;
	if ((status & IRQ_TX) && cec->transmitting) {
		cec->transmitting = false;
		tx_status = s7d_cec_tx_status(status);
		if (status & (IRQ_ARB_LOST | IRQ_TX_ERROR)) {
			ret = s7d_cec_write(cec, CEC_CTRL, 0);
			if (!ret)
				ret = s7d_cec_write(cec, CEC_TX_COUNT, 0);
			if (ret) {
				tx_status = CEC_TX_STATUS_ERROR;
				s7d_cec_stop(cec);
				goto out;
			}
		}
	}
	if ((status & IRQ_RX) && !(status & IRQ_RX_ERROR)) {
		ret = s7d_cec_read(cec, CEC_RX_COUNT, &value);
		if (!ret && value && value <= CEC_MAX_MSG_SIZE) {
			msg.len = value;
			for (i = 0; i < msg.len; i++) {
				ret = s7d_cec_read(cec, CEC_RX_DATA + i, &msg.msg[i]);
				if (ret)
					break;
			}
		}
		if (ret)
			msg.len = 0;
	}
	if (status & (IRQ_RX | IRQ_RX_ERROR)) {
		int unlock = s7d_cec_write(cec, CEC_RX_LOCK, 0);

		if (ret || unlock) {
			msg.len = 0;
			if (cec->transmitting)
				tx_status = CEC_TX_STATUS_ERROR;
			s7d_cec_stop(cec);
		}
	}
out:
	mutex_unlock(&cec->lock);
	/* The framework takes adap->lock; never hold the register lock here. */
	if (tx_status == CEC_TX_STATUS_ERROR)
		dev_warn_ratelimited(&cec->adap->devnode.dev,
				     "CEC TX error: irq=%#x access=%d\n", status, ret);
	if (tx_status)
		cec_transmit_attempt_done(cec->adap, tx_status);
	if (msg.len)
		cec_received_msg(cec->adap, &msg);
	return IRQ_HANDLED;
}

static const struct cec_adap_ops s7d_cec_ops = {
	.adap_enable = s7d_cec_enable,
	.adap_log_addr = s7d_cec_log_addr,
	.adap_transmit = s7d_cec_transmit,
};

#if IS_ENABLED(CONFIG_CEC_MESON_S7D_AOCPU)
/* Layout of the firmware's MBOX_CMD_SET_CEC_DATA payload. */
struct s7d_cec_aocpu_data {
	__le32 config;
	__le32 address;
	__le32 vendor_id;
	u8 osd_name[16];
} __packed;

/* The mailbox provider is a module; look it up when first needed. */
static int s7d_cec_aocpu_xfer(struct s7d_cec *cec, u32 cmd, void *tx,
			      u32 tx_size, void *rx, u32 rx_size)
{
	struct aml_mbox_data data = {
		.cmd = cmd,
		.txbuf = tx,
		.txsize = tx_size,
		.rxbuf = rx,
		.rxsize = rx_size,
		.sync = MBOX_SYNC,
	};
	struct mbox_chan *chan;
	int ret;

	if (!cec->mbox) {
		cec->mbox_client.dev = cec->dev;
		cec->mbox_client.tx_block = true;
		cec->mbox_client.tx_tout = 1000;
		chan = mbox_request_channel_byname(&cec->mbox_client, "aocpu");
		if (IS_ERR(chan))
			return PTR_ERR(chan);
		cec->mbox = chan;
	}
	init_completion(&data.complete);
	ret = mbox_send_message(cec->mbox, &data);
	return ret < 0 ? ret : 0;
}

static void s7d_cec_aocpu_set(struct s7d_cec *cec, u32 config, u32 address)
{
	const struct cec_log_addrs *la = &cec->adap->log_addrs;
	struct s7d_cec_aocpu_data data = {
		.config = cpu_to_le32(config),
		.address = cpu_to_le32(address),
		.vendor_id = cpu_to_le32(la->vendor_id == CEC_VENDOR_ID_NONE ?
					 0xffffff : la->vendor_id),
	};
	int ret;

	memcpy(data.osd_name, la->osd_name, sizeof(la->osd_name));
	ret = s7d_cec_aocpu_xfer(cec, MBOX_CMD_SET_CEC_DATA, &data, sizeof(data),
				 NULL, 0);
	if (ret)
		dev_dbg(cec->dev, "AOCPU CEC data: %d\n", ret);
}

/* Deliver the messages that woke the system, as the firmware kept them. */
static void s7d_cec_aocpu_replay(struct s7d_cec *cec)
{
	static const u32 cmds[] = {
		MBOX_CMD_GET_CEC_OTP_MSG, MBOX_CMD_GET_CEC_AS_MSG,
	};
	u8 buf[1 + CEC_MAX_MSG_SIZE];
	struct cec_msg msg;
	int i;

	/* The firmware does not clear its copies, so check who woke us. */
	if (!cec->wake_reason ||
	    FIELD_GET(WAKE_REASON, readl(cec->wake_reason)) != WAKE_REASON_CEC)
		return;
	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		memset(buf, 0, sizeof(buf));
		if (s7d_cec_aocpu_xfer(cec, cmds[i], NULL, 0, buf, sizeof(buf)) ||
		    !buf[0] || buf[0] > CEC_MAX_MSG_SIZE)
			continue;
		memset(&msg, 0, sizeof(msg));
		msg.len = buf[0];
		memcpy(msg.msg, buf + 1, msg.len);
		cec_received_msg(cec->adap, &msg);
	}
}

static void s7d_cec_aocpu_release(struct s7d_cec *cec)
{
	if (cec->mbox)
		mbox_free_channel(cec->mbox);
	cec->mbox = NULL;
}
#else
static void s7d_cec_aocpu_set(struct s7d_cec *cec, u32 config, u32 address)
{
}

static void s7d_cec_aocpu_replay(struct s7d_cec *cec)
{
}

static void s7d_cec_aocpu_release(struct s7d_cec *cec)
{
}
#endif

/* Hand the controller to the AOCPU for wake; zero hands it nothing. */
static void s7d_cec_handoff(struct s7d_cec *cec)
{
	struct cec_adapter *adap = cec->adap;
	const struct cec_log_addrs *la = &adap->log_addrs;
	u32 config = 0, address = 0;

	lockdep_assert_held(&adap->lock);
	if (cec->enabled && adap->is_configured && la->num_log_addrs) {
		config = AO_CEC_ON | AO_CEC_AUTO_POWER_ON;
		address = FIELD_PREP(AO_CEC_PHYS_ADDR, adap->phys_addr) |
			  FIELD_PREP(AO_CEC_LOG_ADDR, la->log_addr[0]) |
			  FIELD_PREP(AO_CEC_DEV_TYPE, la->primary_device_type[0]);
	}
	writel(address, cec->handoff + AO_CEC_ADDRESS);
	writel(config, cec->handoff + AO_CEC_CONFIG);
	if (config)
		s7d_cec_aocpu_set(cec, config, address);
	cec->handed_off = true;
}

static int s7d_cec_suspend(struct device *dev)
{
	struct s7d_cec *cec = dev_get_drvdata(dev);
	bool deep = pm_suspend_target_state != PM_SUSPEND_TO_IDLE;
	bool abort;

	if (deep && !cec->handoff)
		return -EOPNOTSUPP;
	mutex_lock(&cec->adap->lock);
	mutex_lock(&cec->lock);
	cec->suspended = true;
	abort = cec->transmitting;
	s7d_cec_stop(cec);
	mutex_unlock(&cec->lock);
	if (deep)
		s7d_cec_handoff(cec);
	mutex_unlock(&cec->adap->lock);
	synchronize_irq(cec->irq);
	if (abort)
		cec_transmit_done(cec->adap,
				  CEC_TX_STATUS_ABORTED | CEC_TX_STATUS_MAX_RETRIES,
				  0, 0, 0, 0);
	return 0;
}

static int s7d_cec_resume(struct device *dev)
{
	struct s7d_cec *cec = dev_get_drvdata(dev);
	bool replay;
	int ret = 0;

	mutex_lock(&cec->adap->lock);
	mutex_lock(&cec->lock);
	if (cec->enabled)
		ret = s7d_cec_start(cec);
	if (!ret)
		cec->suspended = false;
	replay = !ret && cec->enabled && cec->handed_off;
	cec->handed_off = false;
	mutex_unlock(&cec->lock);
	mutex_unlock(&cec->adap->lock);
	/* The framework takes adap->lock to receive. */
	if (replay)
		s7d_cec_aocpu_replay(cec);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(s7d_cec_pm_ops, s7d_cec_suspend, s7d_cec_resume);

static void s7d_cec_shutdown(struct platform_device *pdev)
{
	struct s7d_cec *cec = platform_get_drvdata(pdev);

	mutex_lock(&cec->lock);
	cec->stopping = true;
	s7d_cec_stop(cec);
	mutex_unlock(&cec->lock);
	synchronize_irq(cec->irq);
}

/* Leave the firmware able to power the board on for the TV. */
static void s7d_cec_poweroff(struct platform_device *pdev)
{
	struct s7d_cec *cec = platform_get_drvdata(pdev);

	if (system_state == SYSTEM_POWER_OFF && cec->handoff) {
		mutex_lock(&cec->adap->lock);
		s7d_cec_handoff(cec);
		mutex_unlock(&cec->adap->lock);
	}
	s7d_cec_shutdown(pdev);
}

static int s7d_cec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hdmi_dev;
	struct resource *res;
	struct s7d_cec *cec;
	int ret;

	hdmi_dev = cec_notifier_parse_hdmi_phandle(dev);
	if (IS_ERR(hdmi_dev))
		return PTR_ERR(hdmi_dev);
	cec = devm_kzalloc(dev, sizeof(*cec), GFP_KERNEL);
	if (!cec)
		return -ENOMEM;
	cec->dev = dev;
	mutex_init(&cec->lock);
	cec->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cec->base))
		return PTR_ERR(cec->base);
	/* Without the firmware handoff only s2idle is supported. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "handoff");
	if (res) {
		cec->handoff = devm_ioremap_resource(dev, res);
		if (IS_ERR(cec->handoff))
			return PTR_ERR(cec->handoff);
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wake");
	if (res) {
		cec->wake_reason = devm_ioremap_resource(dev, res);
		if (IS_ERR(cec->wake_reason))
			return PTR_ERR(cec->wake_reason);
	}
	cec->clocks[0].id = "apb";
	cec->clocks[1].id = "core";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(cec->clocks), cec->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "CEC clocks\n");
	cec->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(cec->reset))
		return dev_err_probe(dev, PTR_ERR(cec->reset), "CEC reset\n");
	cec->irq = platform_get_irq(pdev, 0);
	if (cec->irq < 0)
		return cec->irq;
	ret = clk_set_rate_exclusive(cec->clocks[1].clk, 32768);
	if (ret)
		return dev_err_probe(dev, ret, "CEC core rate\n");
	if (clk_get_rate(cec->clocks[1].clk) != 32768) {
		ret = -ERANGE;
		goto put_rate;
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(cec->clocks), cec->clocks);
	if (ret)
		goto put_rate;
	ret = reset_control_deassert(cec->reset);
	if (ret)
		goto disable_clocks;
	ret = reset_control_status(cec->reset);
	if (ret) {
		if (ret > 0)
			ret = -EIO;
		goto disable_clocks;
	}
	mutex_lock(&cec->lock);
	s7d_cec_stop(cec);
	mutex_unlock(&cec->lock);
	cec->adap = cec_allocate_adapter(&s7d_cec_ops, cec, "s7d-cecb",
					(CEC_CAP_DEFAULTS & ~CEC_CAP_RC) | CEC_CAP_CONNECTOR_INFO,
					CEC_MAX_LOG_ADDRS);
	if (IS_ERR(cec->adap)) {
		ret = PTR_ERR(cec->adap);
		goto disable_clocks;
	}
	cec->adap->owner = THIS_MODULE;
	ret = devm_request_threaded_irq(dev, cec->irq, NULL, s7d_cec_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), cec);
	if (ret)
		goto delete_adapter;
	cec->notifier = cec_notifier_cec_adap_register(hdmi_dev, NULL, cec->adap);
	if (!cec->notifier) {
		ret = -ENOMEM;
		goto free_irq;
	}
	platform_set_drvdata(pdev, cec);
	enable_irq(cec->irq);
	ret = cec_register_adapter(cec->adap, dev);
	if (ret)
		goto unregister_notifier;
	return 0;
unregister_notifier:
	s7d_cec_shutdown(pdev);
	cec_notifier_cec_adap_unregister(cec->notifier, cec->adap);
free_irq:
	devm_free_irq(dev, cec->irq, cec);
delete_adapter:
	cec_delete_adapter(cec->adap);
disable_clocks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(cec->clocks), cec->clocks);
put_rate:
	clk_rate_exclusive_put(cec->clocks[1].clk);
	return dev_err_probe(dev, ret, "CEC initialization\n");
}

static void s7d_cec_remove(struct platform_device *pdev)
{
	struct s7d_cec *cec = platform_get_drvdata(pdev);

	s7d_cec_shutdown(pdev);
	s7d_cec_aocpu_release(cec);
	devm_free_irq(&pdev->dev, cec->irq, cec);
	cec_unregister_adapter(cec->adap);
	clk_bulk_disable_unprepare(ARRAY_SIZE(cec->clocks), cec->clocks);
	clk_rate_exclusive_put(cec->clocks[1].clk);
}

static const struct of_device_id s7d_cec_match[] = {
	{ .compatible = "amlogic,s7d-cecb" },
	{ }
};
MODULE_DEVICE_TABLE(of, s7d_cec_match);

static struct platform_driver s7d_cec_driver = {
	.probe = s7d_cec_probe,
	.remove = s7d_cec_remove,
	.shutdown = s7d_cec_poweroff,
	.driver = {
		.name = "s7d-cecb",
		.of_match_table = s7d_cec_match,
		.pm = pm_sleep_ptr(&s7d_cec_pm_ops),
	},
};
builtin_platform_driver(s7d_cec_driver);

MODULE_DESCRIPTION("Amlogic S7D standard CECB controller");
MODULE_LICENSE("GPL");
