// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "s7d-hdmi-ddc.h"
#include "s7d-hdmi-io.h"

#define DDC_ADDR	0x0003
#define DDC_SEGMENT	0x0004
#define DDC_OFFSET	0x0005
#define DDC_COUNT_LO	0x0006
#define DDC_COUNT_HI	0x0007
#define DDC_STATUS	0x0008
#define DDC_CMD		0x0009
#define DDC_DATA	0x000a
#define DDC_FIFO_COUNT	0x000b
#define DDC_DELAY	0x000c
#define DDC_CYP		0x0018
#define DDC_DUTY	0x001d
#define DDC_MASTER	0x06f8
#define DDC_MASTER_EN	BIT(7)
#define DDC_BUSLOW	BIT(6)
#define DDC_NACK	BIT(5)
#define DDC_BUSY	BIT(4)
#define DDC_FIFO_SIZE	16
#define CMD_EDID_READ	0x04
#define CMD_READ	0x02
#define CMD_WRITE	0x06
#define CMD_FIFO_CLEAR	0x09
#define CMD_CLOCK_RESET	0x0a
#define CMD_ABORT	0x0f
#define DDC_TIMEOUT_US	200000

static int ddc_wait_idle(struct s7d_hdmi_ddc *ddc)
{
	unsigned int status;

	return regmap_read_poll_timeout(ddc->core, DDC_STATUS, status,
					!(status & DDC_BUSY), 100, DDC_TIMEOUT_US);
}

/* Always run on errors after acquiring the master, including short reads. */
static int ddc_recover(struct s7d_hdmi_ddc *ddc)
{
	int ret;

	ret = s7d_hdmi_update_checked(ddc->core, DDC_MASTER, DDC_MASTER_EN, DDC_MASTER_EN);
	if (ret)
		return ret;
	ret = regmap_write(ddc->core, DDC_CMD, CMD_ABORT);
	if (ret)
		return ret;
	usleep_range(1, 2);
	ret = regmap_write(ddc->core, DDC_CMD, CMD_CLOCK_RESET);
	if (ret)
		return ret;
	ret = ddc_wait_idle(ddc);
	if (ret)
		return ret;
	ret = regmap_write(ddc->core, DDC_STATUS, 0);
	if (ret)
		return ret;
	return s7d_hdmi_update_checked(ddc->core, DDC_MASTER, DDC_MASTER_EN, 0);
}

static int ddc_status_error(unsigned int status)
{
	if (status & DDC_BUSLOW)
		return -EIO;
	if (status & DDC_NACK)
		return -ENXIO;
	return 0;
}

static int ddc_begin(struct s7d_hdmi_ddc *ddc, u8 segment, u8 addr, u8 offset,
		     unsigned int count)
{
	struct regmap *map = ddc->core;
	const struct reg_sequence setup[] = {
		{ DDC_ADDR, addr << 1 },
		{ DDC_SEGMENT, segment },
		{ DDC_OFFSET, offset },
		{ DDC_COUNT_HI, count >> 8 },
		{ DDC_COUNT_LO, count & 0xff },
	};
	unsigned int i;
	int ret;

	ret = ddc_wait_idle(ddc);
	if (ret)
		return ret;
	ret = s7d_hdmi_update_checked(map, DDC_MASTER, DDC_MASTER_EN, DDC_MASTER_EN);
	if (ret)
		return ret;
	ret = regmap_write(map, DDC_STATUS, 0);
	if (ret)
		return ret;
	ret = regmap_write(map, DDC_CMD, CMD_FIFO_CLEAR);
	if (ret)
		return ret;
	usleep_range(1, 2);
	for (i = 0; i < ARRAY_SIZE(setup); i++) {
		ret = s7d_hdmi_update_checked(map, setup[i].reg, 0xff, setup[i].def);
		if (ret)
			return ret;
	}
	return 0;
}

static int ddc_read(struct s7d_hdmi_ddc *ddc, u8 segment, u8 addr, u8 offset,
		    u8 *buf, unsigned int len)
{
	ktime_t deadline;
	unsigned int status, count, value, done = 0;
	int ret;

	ret = ddc_begin(ddc, segment, addr, offset, len);
	if (ret)
		return ret;
	ret = regmap_write(ddc->core, DDC_CMD, addr == 0x50 ? CMD_EDID_READ : CMD_READ);
	if (ret)
		return ret;
	deadline = ktime_add_us(ktime_get(), DDC_TIMEOUT_US);
	/* Do not interpret a pre-start empty FIFO as a completed short read. */
	usleep_range(2000, 2500);
	while (ktime_before(ktime_get(), deadline)) {
		ret = regmap_read(ddc->core, DDC_STATUS, &status);
		if (ret)
			return ret;
		ret = ddc_status_error(status);
		if (ret)
			return ret;
		ret = regmap_read(ddc->core, DDC_FIFO_COUNT, &count);
		if (ret)
			return ret;
		count &= 0x1f;
		if (count > DDC_FIFO_SIZE || count > len - done)
			return -EIO;
		while (count--) {
			ret = regmap_read(ddc->core, DDC_DATA, &value);
			if (ret)
				return ret;
			buf[done++] = value;
		}
		if (done == len) {
			ret = ddc_wait_idle(ddc);
			if (ret)
				return ret;
			ret = regmap_read(ddc->core, DDC_STATUS, &status);
			return ret ? ret : ddc_status_error(status);
		}
		usleep_range(500, 1000);
	}
	return -ETIMEDOUT;
}

static int ddc_write(struct s7d_hdmi_ddc *ddc, u8 offset, u8 value)
{
	unsigned int status;
	int ret;

	ret = ddc_begin(ddc, 0, 0x54, offset, 1);
	if (ret)
		return ret;
	ret = regmap_write(ddc->core, DDC_CMD, CMD_FIFO_CLEAR | 0x30);
	if (ret)
		return ret;
	ret = regmap_write(ddc->core, DDC_DATA, value);
	if (ret)
		return ret;
	/* Keep the vendor's SDA delay/input-filter bits for write transactions. */
	ret = regmap_write(ddc->core, DDC_CMD, CMD_WRITE | 0x30);
	if (ret)
		return ret;
	usleep_range(2000, 2500);
	ret = ddc_wait_idle(ddc);
	if (ret)
		return ret;
	ret = regmap_read(ddc->core, DDC_STATUS, &status);
	return ret ? ret : ddc_status_error(status);
}

static int s7d_ddc_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
	struct s7d_hdmi_ddc *ddc = i2c_get_adapdata(adapter);
	int first = 0, ret, finish;
	u8 segment = 0;
	bool write;

	/* Exact DRM EDID/SCDC patterns; reject unsupported requests before I/O. */
	if (num < 1 || num > 3)
		return -EOPNOTSUPP;
	if (num == 3) {
		if (msgs[0].addr != 0x30 || msgs[0].flags || msgs[0].len != 1)
			return -EOPNOTSUPP;
		segment = msgs[0].buf[0];
		first = 1;
	}
	write = num == 1;
	if (write) {
		if (msgs[0].addr != 0x54 || msgs[0].flags || msgs[0].len != 2)
			return -EOPNOTSUPP;
	} else {
		if (msgs[first].flags || msgs[first].len != 1 ||
		    msgs[first + 1].flags != I2C_M_RD ||
		    msgs[first].addr != msgs[first + 1].addr ||
		    (msgs[first].addr != 0x50 && msgs[first].addr != 0x54) ||
		    !msgs[first + 1].len || msgs[first + 1].len > 128 ||
		    (first && msgs[first].addr != 0x50))
			return -EOPNOTSUPP;
		/* One transaction may not silently wrap the sink's byte offset. */
		if (msgs[first].buf[0] + msgs[first + 1].len > 256)
			return -EOPNOTSUPP;
	}

	mutex_lock(&ddc->lock);
	if (ddc->fault) {
		ret = ddc_recover(ddc);
		if (ret)
			goto unlock;
		ddc->fault = false;
	}
	if (write)
		ret = ddc_write(ddc, msgs[0].buf[0], msgs[0].buf[1]);
	else
		ret = ddc_read(ddc, segment, msgs[first].addr, msgs[first].buf[0],
			       msgs[first + 1].buf, msgs[first + 1].len);
	if (ret) {
		ddc->fault = !!ddc_recover(ddc);
	} else {
		finish = s7d_hdmi_update_checked(ddc->core, DDC_MASTER, DDC_MASTER_EN, 0);
		if (finish) {
			ret = finish;
			ddc->fault = !!ddc_recover(ddc);
		}
	}
unlock:
	mutex_unlock(&ddc->lock);
	return ret ? ret : num;
}

static u32 s7d_ddc_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm s7d_ddc_algorithm = {
	.master_xfer = s7d_ddc_xfer,
	.functionality = s7d_ddc_functionality,
};

static const struct i2c_adapter_quirks s7d_ddc_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
	.max_num_msgs = 3,
	.max_read_len = 128,
	.max_write_len = 2,
};

int s7d_hdmi_ddc_register(struct device *dev, struct s7d_hdmi_ddc *ddc,
			struct regmap *core)
{
	int ret;

	ddc->core = core;
	mutex_init(&ddc->lock);
	/* Vendor default 75 kHz profile with the parent's 200 MHz basic clocks. */
	ret = s7d_hdmi_update_checked(core, DDC_CYP, 0x3, 2);
	if (ret)
		return ret;
	ret = s7d_hdmi_update_checked(core, DDC_DUTY, 0x3, 0);
	if (ret)
		return ret;
	ret = s7d_hdmi_update_checked(core, DDC_DELAY, 0xff, 0x26);
	if (ret)
		return ret;
	ret = ddc_recover(ddc);
	if (ret)
		return ret;
	strscpy(ddc->adapter.name, "S7D HDMI DDC", sizeof(ddc->adapter.name));
	ddc->adapter.owner = THIS_MODULE;
	ddc->adapter.algo = &s7d_ddc_algorithm;
	ddc->adapter.quirks = &s7d_ddc_quirks;
	ddc->adapter.dev.parent = dev;
	i2c_set_adapdata(&ddc->adapter, ddc);
	return i2c_add_adapter(&ddc->adapter);
}

void s7d_hdmi_ddc_unregister(struct s7d_hdmi_ddc *ddc)
{
	i2c_del_adapter(&ddc->adapter);
}
