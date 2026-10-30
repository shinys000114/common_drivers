// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t phys_addr_t;
typedef int64_t ktime_t;
#define U8_MAX UINT8_MAX
#define regmap_get_device(map) (map)
#define dev_err(dev, ...) ((void)(dev))
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define I2C_M_RD 1
#define I2C_M_DMA_SAFE 0x200
struct mutex {
	bool held;
};
static void mutex_lock(struct mutex *m)
{
	assert(!m->held);
	m->held = true;
}
static void mutex_unlock(struct mutex *m)
{
	assert(m->held);
	m->held = false;
}
struct i2c_adapter {
	void *data;
};
struct i2c_msg {
	unsigned addr, flags, len;
	u8 *buf;
};
struct regmap {
	unsigned regs[0x1000];
};
struct s7d_hdmi_ddc {
	struct i2c_adapter adapter;
	struct regmap *core;
	struct mutex lock;
	bool fault;
};
struct reg_sequence {
	unsigned reg, def;
};
static void *i2c_get_adapdata(struct i2c_adapter *a)
{
	return a->data;
}
static ktime_t now;
static ktime_t ktime_get(void)
{
	return now;
}
static ktime_t ktime_add_us(ktime_t a, unsigned b)
{
	return a + b;
}
static bool ktime_before(ktime_t a, ktime_t b)
{
	return a < b;
}
static void usleep_range(unsigned lo, unsigned hi)
{
	(void)hi;
	now += lo;
}
static unsigned reads, writes, fifo_reads, recoveries, read_commands,
	write_commands;
static unsigned supplied, transfer_len, cursor, status_fault, fifo_override;
static bool running, reset_stuck, initial_busy, nack_at_end,
	master_release_fails;
static int fail_reg, drop_reg;
static unsigned long smc_result, smc_args[5], smc_calls;
struct arm_smccc_res {
	unsigned long a0;
};
static void arm_smccc_smc(unsigned long fn, unsigned long addr,
			  unsigned long data, unsigned long width,
			  unsigned long a4, unsigned long a5, unsigned long a6,
			  unsigned long a7, struct arm_smccc_res *r)
{
	assert(!a4 && !a5 && !a6 && !a7);
	++smc_calls;
	smc_args[0] = fn;
	smc_args[1] = addr;
	smc_args[2] = data;
	smc_args[3] = width;
	r->a0 = smc_result;
}
static int regmap_read(struct regmap *m, unsigned reg, unsigned *val)
{
	++reads;
	assert(reg < ARRAY_SIZE(m->regs));
	if ((int)reg == fail_reg)
		return -EIO;
	if (reg == 8) {
		*val = status_fault;
		if (initial_busy || (running && cursor < transfer_len))
			*val |= 0x10;
		if (nack_at_end && cursor == transfer_len && running)
			*val |= 0x20;
	} else if (reg == 11) {
		unsigned remaining = supplied > cursor ? supplied - cursor : 0;
		*val = fifo_override ? fifo_override :
				       (remaining > 16 ? 16 : remaining);
	} else if (reg == 10) {
		assert(running && cursor < supplied);
		++fifo_reads;
		*val = cursor++ & 0xff;
	} else {
		*val = m->regs[reg];
	}
	return 0;
}
static int regmap_write(struct regmap *m, unsigned reg, unsigned value)
{
	++writes;
	assert(reg < ARRAY_SIZE(m->regs) && value <= 255);
	if ((int)reg == fail_reg)
		return -EIO;
	if ((int)reg == drop_reg)
		return 0;
	if (reg == 0x6f8 && !value && master_release_fails)
		return 0;
	m->regs[reg] = value;
	if (reg == 9) {
		unsigned cmd = value & 15;
		if (cmd == 15) {
			assert(m->regs[0x6f8] & 0x80);
			++recoveries;
			if (!reset_stuck) {
				running = false;
				initial_busy = false;
			}
		}
		if (cmd == 2 || cmd == 4 || cmd == 6) {
			assert(m->regs[0x6f8] & 0x80);
			running = true;
			transfer_len = m->regs[6] | (m->regs[7] << 8);
			if (cmd == 6) {
				++write_commands;
				cursor = transfer_len;
			} else {
				++read_commands;
				cursor = 0;
			}
		}
	}
	return 0;
}
static int regmap_update_bits(struct regmap *m, unsigned reg, unsigned mask,
			      unsigned v)
{
	unsigned old;
	int ret = regmap_read(m, reg, &old);
	if (ret)
		return ret;
	if ((old & mask) == v)
		return 0;
	return regmap_write(m, reg, (old & ~mask) | v);
}
#define regmap_read_poll_timeout(map, reg, val, cond, delay, timeout) \
	({                                                            \
		int _r = -ETIMEDOUT;                                  \
		ktime_t _end = now + timeout;                         \
		do {                                                  \
			_r = regmap_read(map, reg, &val);             \
			if (_r)                                       \
				break;                                \
			if (cond)                                     \
				break;                                \
			_r = -ETIMEDOUT;                              \
			now += delay;                                 \
		} while (now < _end);                                 \
		_r;                                                   \
	})
