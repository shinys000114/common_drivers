// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

static struct regmap map;
static struct s7d_hdmi_ddc d;
static u8 offset, segment, buf[130], wr[2];
static struct i2c_msg msg[3];
static void init(unsigned count)
{
	memset(&map, 0, sizeof(map));
	memset(&d, 0, sizeof(d));
	memset(buf, 0xee, sizeof(buf));
	d.core = &map;
	d.adapter.data = &d;
	now = reads = writes = fifo_reads = recoveries = 0;
	read_commands = write_commands = cursor = status_fault = fifo_override =
		0;
	supplied = transfer_len = count;
	running = reset_stuck = initial_busy = nack_at_end =
		master_release_fails = false;
	fail_reg = drop_reg = -1;
	offset = 0;
	segment = 2;
	wr[0] = 0x20;
	wr[1] = 0;
	msg[0] = (struct i2c_msg){ 0x50, 0, 1, &offset };
	msg[1] = (struct i2c_msg){ 0x50, I2C_M_RD, count, buf + 1 };
}
static void released(void)
{
	assert(!(map.regs[0x6f8] & 0x80));
	assert(!d.lock.held && !d.fault);
	assert(buf[0] == 0xee && buf[129] == 0xee);
}
int main(void)
{
	struct s7d_hdmi_bus bus = { 0xfe310000, 8 };
	unsigned value = 0xdead;
	for (unsigned reg = 0xf80; reg <= 0xfa9; ++reg) {
		assert(s7d_hdmi_reg_read(&bus, reg, &value) == -EOPNOTSUPP);
		assert(s7d_hdmi_reg_write(&bus, reg, 0) == -EOPNOTSUPP);
	}
	assert(!smc_calls && value == 0xdead);
	assert(s7d_hdmi_core_reg_valid(0xf7f) &&
	       s7d_hdmi_core_reg_valid(0xfaa));
	smc_result = 0xaa;
	assert(!s7d_hdmi_reg_read(&bus, 0x101, &value) && value == 0xaa);
	assert(smc_args[0] == 0x82000018 && smc_args[1] == 0xfe310101 &&
	       smc_args[2] == 8);
	smc_result = ~0UL;
	assert(s7d_hdmi_reg_read(&bus, 0, &value) == -EIO && value == 0xaa);
	bus.base = 0xfe300000;
	bus.width = 32;
	smc_result = 0x80007000;
	assert(!s7d_hdmi_reg_read(&bus, 8, &value) && value == 0x80007000);
	assert(!s7d_hdmi_reg_write(&bus, 8, 0x80007000));
	assert(smc_args[0] == 0x82000019 && smc_args[1] == 0xfe300008 &&
	       smc_args[2] == 0x80007000 && smc_args[3] == 32);
	for (unsigned len = 1; len <= 128; ++len) {
		init(len);
		assert(s7d_ddc_xfer(&d.adapter, msg, 2) == 2);
		assert(fifo_reads == len && read_commands == 1 && !recoveries);
		for (unsigned i = 0; i < len; ++i)
			assert(buf[i + 1] == (u8)i);
		assert(map.regs[3] == 0xa0 && map.regs[4] == 0);
		released();
	}
	init(128);
	msg[2] = msg[1];
	msg[1] = msg[0];
	msg[0] = (struct i2c_msg){ 0x30, 0, 1, &segment };
	offset = 128;
	assert(s7d_ddc_xfer(&d.adapter, msg, 3) == 3);
	assert(map.regs[4] == 2 && map.regs[5] == 128);
	released();
	init(1);
	msg[0].addr = msg[1].addr = 0x54;
	offset = 0x21;
	assert(s7d_ddc_xfer(&d.adapter, msg, 2) == 2 && map.regs[9] == 2);
	released();
	init(1);
	msg[0] = (struct i2c_msg){ 0x54, 0, 2, wr };
	assert(s7d_ddc_xfer(&d.adapter, msg, 1) == 1 && write_commands == 1);
	released();
	for (unsigned pattern = 0; pattern < 3; ++pattern) {
		init(1);
		int n = 2;
		msg[0].flags |= I2C_M_DMA_SAFE;
		msg[1].flags |= I2C_M_DMA_SAFE;
		if (pattern == 0)
			msg[0].addr = msg[1].addr = 0x54;
		if (pattern == 1) {
			msg[0] =
				(struct i2c_msg){ 0x54, I2C_M_DMA_SAFE, 2, wr };
			n = 1;
		}
		if (pattern == 2) {
			msg[2] = msg[1];
			msg[1] = msg[0];
			msg[0] = (struct i2c_msg){ 0x30, I2C_M_DMA_SAFE, 1,
						   &segment };
			n = 3;
		}
		assert(s7d_ddc_xfer(&d.adapter, msg, n) == n);
		released();
	}
	// Reject malformed/unsupported messages before any register access.
	for (unsigned i = 0; i < 9; ++i) {
		init(128);
		int n = 2;
		if (i == 0)
			n = 0;
		if (i == 1)
			n = 4;
		if (i == 2)
			msg[0].addr = 0x51;
		if (i == 3)
			msg[0].flags = I2C_M_RD;
		if (i == 4)
			msg[1].len = 129;
		if (i == 5)
			msg[1].len = 0;
		if (i == 6)
			msg[1].flags |= 0x400;
		if (i == 7)
			offset = 129;
		if (i == 8)
			n = 1;
		assert(s7d_ddc_xfer(&d.adapter, msg, n) == -EOPNOTSUPP);
		assert(!writes && !reads && !read_commands && !write_commands);
	}
	for (unsigned i = 0; i < 8; ++i) {
		init(128);
		int expected = -EIO;
		if (i == 0) {
			supplied = 0;
			expected = -ETIMEDOUT;
		}
		if (i == 1) {
			supplied = 64;
			expected = -ETIMEDOUT;
		}
		if (i == 2)
			fifo_override = 17;
		if (i == 3)
			status_fault = 0x40;
		if (i == 4) {
			status_fault = 0x20;
			expected = -ENXIO;
		}
		if (i == 5) {
			nack_at_end = true;
			expected = -ENXIO;
		}
		if (i == 6)
			drop_reg = 3;
		if (i == 7)
			fail_reg = 10;
		assert(s7d_ddc_xfer(&d.adapter, msg, 2) == expected);
		assert(recoveries == 1);
		released();
		if (i == 0 || i == 1)
			assert(now >= 200000 && now < 410000);
	}
	init(128);
	initial_busy = reset_stuck = true;
	assert(s7d_ddc_xfer(&d.adapter, msg, 2) == -ETIMEDOUT && d.fault);
	assert(!read_commands); // No new transfer while recovery cannot stop hardware.
	assert(s7d_ddc_xfer(&d.adapter, msg, 2) == -ETIMEDOUT && d.fault);
	reset_stuck = false;
	assert(s7d_ddc_xfer(&d.adapter, msg, 2) == 2);
	released();
	init(128);
	master_release_fails = true;
	assert(s7d_ddc_xfer(&d.adapter, msg, 2) == -EIO && d.fault);
	master_release_fails = false;
	assert(s7d_ddc_xfer(&d.adapter, msg, 2) == 2);
	released();
	puts("PASS: SMC widths/addresses, DDC 128 lengths, segment/SCDC, invalid messages, "
	     "short/timeout/FIFO/NACK/readback faults and recovery retry; no hardware");
}
