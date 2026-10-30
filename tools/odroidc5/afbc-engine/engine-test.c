// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/*
 * Host model of the S7D Mali AFBCD plan and decoder state machine against a
 * fake register file. It checks the native call sequence, not silicon timing.
 */
#include <assert.h>
#include <stdio.h>

#include "s7d-afbc-engine.h"

#define RAW 0x3a01
#define CLEAR 0x3a02
#define COMMAND 0x3a05
#define STATUS 0x3a06
#define SURFACES 0x3a07
#define TOP 0x1a0f
#define ENCP_EN 0x1b80
#define READ0_STATUS 0x27ad
#define READOUT BIT(0)
#define SWAPPED BIT(1)
#define MANUAL_RESET BIT(23)

static u32 regs[0x4000];
static u32 stuck_reg;
static unsigned int commands, resets, cases;

static int fake_read(void *data, u32 reg, u32 *value)
{
	assert(reg < ARRAY_SIZE(regs));
	*value = regs[reg];
	return 0;
}

static int fake_write(void *data, u32 reg, u32 value)
{
	assert(reg < ARRAY_SIZE(regs));
	if (reg == stuck_reg)
		return 0;
	if (reg == CLEAR) {
		regs[RAW] &= ~value;
		return 0;
	}
	if (reg == COMMAND)
		commands++;
	regs[reg] = value;
	return 0;
}

static int fake_check_start(void *data, u64 sequence, u8 field)
{
	return 0;
}

static int fake_reset_idle(void *data)
{
	resets++;
	regs[STATUS] = 0;
	return 0;
}

static const struct s7d_afbc_engine_io io = {
	.read = fake_read,
	.write = fake_write,
	.check_start = fake_check_start,
	.reset_idle = fake_reset_idle,
};

static const struct drm_format_info abgr = { DRM_FORMAT_ABGR8888, 1 };

static void build(unsigned int surface, u32 width, u32 height, u64 addr,
		  struct s7d_afbc_state *out)
{
	static struct drm_gem_dma_object obj[4];
	static struct drm_afbc_framebuffer fb[4];
	static unsigned int n;
	u32 aligned_width = ALIGN(width, 256), aligned_height = ALIGN(height, 64);
	u32 blocks = aligned_width * aligned_height / 256;
	u32 size = ALIGN(blocks * 16, 4096) + blocks * 1024;
	struct drm_rect src = { 0, 0, width << 16, height << 16 };

	assert(n < 4);
	obj[n] = (struct drm_gem_dma_object) { .base.size = size, .dma_addr = addr };
	fb[n] = (struct drm_afbc_framebuffer) {
		.base = {
			.format = &abgr, .obj = { &obj[n].base },
			.modifier = S7D_AFBC_MODIFIER, .flags = DRM_MODE_FB_MODIFIERS,
			.pitches = { aligned_width * 4 }, .width = width, .height = height,
		},
		.block_width = 32, .block_height = 8,
		.aligned_width = aligned_width, .aligned_height = aligned_height,
		.afbc_size = size,
	};
	assert(!s7d_afbc_build_state(&fb[n++], &src, GENMASK_ULL(35, 0), surface, out));
}

static void observe(u32 raw, u32 surfaces)
{
	regs[RAW] = raw;
	regs[STATUS] = 0;
	regs[TOP] |= MANUAL_RESET;
	regs[SURFACES] = surfaces;
}

static void check_plans(const struct s7d_afbc_plan *plan)
{
	struct s7d_afbc_plan p = *plan;

	assert(!s7d_afbc_plan_check(&p));
	p.surface_mask = 0;
	assert(s7d_afbc_plan_check(&p) == -EINVAL);
	p.surface_mask = BIT(0);
	assert(s7d_afbc_plan_check(&p) == -EINVAL);	/* stale absent surface */
	p.surface[1] = (struct s7d_afbc_state) {0};
	assert(!s7d_afbc_plan_check(&p));
	p = *plan;
	p.surface_mask = BIT(2) | BIT(0);
	assert(s7d_afbc_plan_check(&p) == -EINVAL);
	p = *plan;
	p.surface[1] = plan->surface[0];
	assert(s7d_afbc_plan_check(&p) == -EINVAL);	/* S0 state in the S1 slot */
	cases += 5;
}

int main(void)
{
	const struct s7d_afbc_frame first = { 1, 0, true, true, true };
	const struct s7d_afbc_frame second = { 2, 1, true, true, true };
	struct s7d_afbc_observation o = {
		.raw = READOUT | SWAPPED, .top = MANUAL_RESET, .surfaces = 3,
	};
	struct s7d_afbc_plan plan = { .surface_mask = 3 }, next, single;
	struct s7d_afbc_engine e;
	u32 failed_reg;

	build(0, 1920, 1080, 0x10000000, &plan.surface[0]);
	build(1, 640, 360, 0x20000000, &plan.surface[1]);
	check_plans(&plan);
	next = plan;
	build(1, 640, 360, 0x30000000, &next.surface[1]);
	assert(s7d_afbc_plan_same_layout(&plan, &next));
	assert(!s7d_afbc_plan_same_headers(&plan, &next));
	single = plan;
	single.surface_mask = BIT(0);
	single.surface[1] = (struct s7d_afbc_state) {0};
	assert(!s7d_afbc_plan_same_layout(&plan, &single));
	cases += 3;

	/* A surface admission bit in either bank refuses the plan. */
	s7d_afbc_engine_init(&e, &io, NULL);
	regs[0x3a32] = BIT(19);
	assert(s7d_afbc_engine_prepare(&e, &plan, 1) == -EOPNOTSUPP &&
	       e.failed_reg == 0x3a32);
	regs[0x3a32] = 0;
	cases++;

	/* A failed S1 bank readback is an abortable first configuration. */
	s7d_afbc_engine_init(&e, &io, NULL);
	stuck_reg = 0x3a35;
	regs[0x3a35] = 0xdead;
	assert(s7d_afbc_engine_prepare(&e, &plan, 1) == -EIO && e.failed_reg == 0x3a35);
	assert(s7d_afbc_engine_can_abort_prepare(&e));
	regs[READ0_STATUS] = BIT(0);
	regs[SURFACES] = 0;
	assert(!s7d_afbc_engine_abort_prepare(&e, &failed_reg) && e.phase == S7D_AFBC_STOPPED);
	stuck_reg = 0;
	cases += 2;

	/* Both banks, then one combined enable and one direct command. */
	s7d_afbc_engine_init(&e, &io, NULL);
	regs[RAW] = 0;
	assert(!s7d_afbc_engine_prepare(&e, &plan, 1));
	assert(regs[SURFACES] == 3 && regs[0x3a10] == 0x10000000 &&
	       regs[0x3a30] == 0x20000000 && regs[0x3a33] == 768);
	regs[ENCP_EN] = BIT(0);
	regs[0x1a30] = BIT(0);
	assert(s7d_afbc_engine_start_initial(&e) == -EBUSY && e.failed_reg == 0x1a30);
	cases += 2;
	s7d_afbc_engine_init(&e, &io, NULL);
	regs[0x1a30] = 0;
	regs[SURFACES] = 0;
	assert(!s7d_afbc_engine_prepare(&e, &plan, 2));
	commands = 0;
	assert(!s7d_afbc_engine_start_initial(&e) && commands == 1 &&
	       e.phase == S7D_AFBC_RUNNING && e.generation == 2);
	cases++;

	/* Only S1 changes; the plan swaps as a whole under one generation. */
	assert(s7d_afbc_engine_stage(&e, &single, 3) == -EOPNOTSUPP);
	assert(!s7d_afbc_engine_stage(&e, &next, 4));
	assert(!s7d_afbc_engine_rdma_drained(&e, 4));
	assert(s7d_afbc_engine_sample(&e, &first, &o) == 0);
	assert(s7d_afbc_engine_sample(&e, &second, &o) == 1);
	observe(READOUT | SWAPPED, 3);
	assert(!s7d_afbc_engine_restart(&e, &second));
	assert(commands == 2 && e.generation == 4 && !e.pending_valid &&
	       regs[0x3a10] == 0x10000000 && regs[0x3a30] == 0x30000000 && !regs[RAW]);
	cases += 3;

	/* A surface missing from the readback faults the whole plan. */
	o.surfaces = BIT(0);
	assert(s7d_afbc_engine_sample(&e, &first, &o) == -EIO &&
	       e.failed_reg == SURFACES && e.phase == S7D_AFBC_ERROR);
	assert(s7d_afbc_engine_can_recover(&e));
	observe(BIT(2), 3);
	regs[READ0_STATUS] = BIT(0);
	assert(!s7d_afbc_engine_recover(&e, &failed_reg));
	assert(resets == 1 && e.phase == S7D_AFBC_STOPPED && !e.bound_valid &&
	       !regs[SURFACES] && !regs[RAW] && e.accepted_generation == 4);
	cases += 2;

	/* Stop drains every OSD reader of the plan. */
	s7d_afbc_engine_init(&e, &io, NULL);
	assert(!s7d_afbc_engine_prepare(&e, &plan, 5));
	assert(!s7d_afbc_engine_start_initial(&e));
	assert(!s7d_afbc_engine_stop_begin(&e));
	o.surfaces = 3;
	assert(s7d_afbc_engine_stop_sample(&e, &o) == 1);
	observe(READOUT | SWAPPED, 3);
	regs[0x1a4b] = BIT(20);
	assert(s7d_afbc_engine_stop_finish(&e) == -EAGAIN);
	regs[0x1a4b] = 0;
	assert(!s7d_afbc_engine_stop_finish(&e) && e.phase == S7D_AFBC_STOPPED &&
	       !regs[SURFACES]);
	cases += 2;

	printf("PASS: %u AFBC plan/engine cases on a fake register file\n", cases);
	return 0;
}
