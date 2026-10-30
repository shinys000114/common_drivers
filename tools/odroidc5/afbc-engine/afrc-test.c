// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* Host checks of the S7D GFCD AFRC modifier, size and buffer rules. */
#include <assert.h>
#include <stdio.h>

#include "s7d-afrc.h"

static unsigned int cases;

static const struct drm_format_info rgba = { DRM_FORMAT_RGBA8888, 1 };

static int build(u32 width, u32 height, u64 modifier, size_t size, u64 addr,
		 struct s7d_afrc_buffer *out)
{
	struct drm_gem_dma_object obj = { .base.size = size, .dma_addr = addr };
	struct drm_framebuffer fb = {
		.format = &rgba, .obj = { &obj.base }, .modifier = modifier,
		.flags = DRM_MODE_FB_MODIFIERS, .pitches = { width * 4 },
		.width = width, .height = height,
	};

	cases++;
	return s7d_afrc_build_buffer(&fb, GENMASK_ULL(35, 0), out);
}

int main(void)
{
	static const u32 sizes[] = { 65536, 98304, 131072 };
	static const u32 aligns[] = { 1024, 512, 2048 };
	struct s7d_afrc_layout layout;
	struct s7d_afrc_buffer buffer = { .cu = 7 };
	u32 cu;

	assert(s7d_afrc_cu(S7D_AFRC_MODIFIER(16)) == S7D_AFRC_CU_16 &&
	       s7d_afrc_cu(S7D_AFRC_MODIFIER(24)) == S7D_AFRC_CU_24 &&
	       s7d_afrc_cu(S7D_AFRC_MODIFIER(32)) == S7D_AFRC_CU_32);
	/* ROT layout, a chroma CU size and AFBC are not GFCD AFRC. */
	assert(s7d_afrc_cu(DRM_FORMAT_MOD_ARM_AFRC(AFRC_FORMAT_MOD_CU_SIZE_32)) == -EINVAL);
	assert(s7d_afrc_cu(S7D_AFRC_MODIFIER(32) |
			   AFRC_FORMAT_MOD_CU_SIZE_P12(AFRC_FORMAT_MOD_CU_SIZE_32)) == -EINVAL);
	assert(s7d_afrc_cu(DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8)) == -EINVAL);
	cases += 6;

	for (cu = 0; cu < 3; cu++) {
		assert(!s7d_afrc_layout(256, 256, cu, &layout));
		assert(layout.size == sizes[cu] && layout.align == aligns[cu]);
		cases++;
	}
	assert(!s7d_afrc_layout(1920, 1088, S7D_AFRC_CU_32, &layout) &&
	       layout.size == 4177920);
	assert(!s7d_afrc_layout(4096, 2160, S7D_AFRC_CU_32, &layout) &&
	       layout.size == 17694720);
	assert(s7d_afrc_layout(1920, 1080, S7D_AFRC_CU_32, &layout) == -EINVAL);
	assert(s7d_afrc_layout(1952, 1088, S7D_AFRC_CU_32, &layout) == -EINVAL);
	assert(s7d_afrc_layout(4160, 1088, S7D_AFRC_CU_32, &layout) == -EINVAL);
	assert(s7d_afrc_layout(256, 2176, S7D_AFRC_CU_32, &layout) == -EINVAL);
	assert(s7d_afrc_layout(0, 16, S7D_AFRC_CU_32, &layout) == -EINVAL);
	assert(s7d_afrc_layout(256, 256, 3, &layout) == -EINVAL);
	cases += 8;

	/* Exact payload at the plane alignment of each CU size. */
	assert(!build(256, 256, S7D_AFRC_MODIFIER(16), 65536, 0x10000400, &buffer) &&
	       buffer.cu == S7D_AFRC_CU_16 && buffer.addr == 0x10000400 &&
	       buffer.last_byte_addr == 0x100103ff);
	assert(!build(256, 256, S7D_AFRC_MODIFIER(24), 98304, 0x10000200, &buffer) &&
	       buffer.cu == S7D_AFRC_CU_24);
	assert(!build(1920, 1088, S7D_AFRC_MODIFIER(32), 4177920, 0x10000800, &buffer) &&
	       buffer.cu == S7D_AFRC_CU_32 && buffer.layout.size == 4177920);
	/* Rejections leave the output unchanged. */
	assert(build(256, 256, S7D_AFRC_MODIFIER(32), 131072, 0x10000400, &buffer) == -ERANGE);
	assert(build(256, 256, S7D_AFRC_MODIFIER(16), 65536, 0x10000200, &buffer) == -ERANGE);
	assert(build(256, 256, S7D_AFRC_MODIFIER(32), 131071, 0x10000000, &buffer) == -EINVAL);
	assert(build(256, 256, S7D_AFRC_MODIFIER(32), 131072, 0xfffff0000ULL, &buffer) == -ERANGE);
	assert(build(1920, 1080, S7D_AFRC_MODIFIER(32), 4177920, 0x10000000, &buffer) == -EINVAL);
	assert(build(256, 256, S7D_AFRC_MODIFIER(32) & ~AFRC_FORMAT_MOD_LAYOUT_SCAN,
		     131072, 0x10000000, &buffer) == -EINVAL);
	assert(buffer.cu == S7D_AFRC_CU_32 && buffer.layout.size == 4177920);
	cases++;

	printf("PASS: %u AFRC layout cases\n", cases);
	return 0;
}
