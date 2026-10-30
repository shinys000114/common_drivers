/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* Minimal kernel/DRM declarations for compiling the AFBC/AFRC helpers on the host. */
#ifndef __S7D_AFBC_SHIM_H
#define __S7D_AFBC_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Kernel values; the libc errno.h would include the shimmed linux/errno.h. */
#define EAGAIN 11
#define EIO 5
#define EBUSY 16
#define EINVAL 22
#define ERANGE 34
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#define EHOSTDOWN 112
#define EALREADY 114
#define ESTALE 116

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;

#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define U32_MAX UINT32_MAX
#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))
#define IS_ALIGNED(x, a) (((x) & ((typeof(x))(a) - 1)) == 0)
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))
#define upper_32_bits(n) ((u32)((u64)(n) >> 32))
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define for_each_if(condition) if (!(condition)) {} else
#define _RET_IP_ ((unsigned long)__builtin_return_address(0))

static inline void *memchr_inv(const void *start, int c, size_t bytes)
{
	const unsigned char *p = start;

	for (; bytes; bytes--, p++)
		if (*p != (unsigned char)c)
			return (void *)p;
	return NULL;
}

static inline u64 ktime_get_mono_fast_ns(void)
{
	return 0;
}

#define fourcc_code(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | \
				 ((u32)(c) << 16) | ((u32)(d) << 24))
#define DRM_FORMAT_ABGR8888 fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_XBGR8888 fourcc_code('X', 'B', '2', '4')
#define DRM_FORMAT_RGBA8888 fourcc_code('R', 'A', '2', '4')
#define DRM_FORMAT_MOD_ARM_AFBC(m) ((0x08ULL << 56) | (m))
#define DRM_FORMAT_MOD_ARM_AFRC(m) ((0x08ULL << 56) | (0x02ULL << 52) | (m))
#define AFRC_FORMAT_MOD_CU_SIZE_16 1ULL
#define AFRC_FORMAT_MOD_CU_SIZE_24 2ULL
#define AFRC_FORMAT_MOD_CU_SIZE_32 3ULL
#define AFRC_FORMAT_MOD_CU_SIZE_P0(cu) (cu)
#define AFRC_FORMAT_MOD_CU_SIZE_P12(cu) ((cu) << 4)
#define AFRC_FORMAT_MOD_LAYOUT_SCAN (1ULL << 8)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 2ULL
#define AFBC_FORMAT_MOD_YTR (1ULL << 4)
#define AFBC_FORMAT_MOD_SPLIT (1ULL << 5)
#define AFBC_FORMAT_MOD_SPARSE (1ULL << 6)
#define AFBC_FORMAT_MOD_TILED (1ULL << 8)
#define AFBC_FORMAT_MOD_SC (1ULL << 9)
#define DRM_MODE_FB_MODIFIERS (1 << 1)

struct drm_rect {
	int x1, y1, x2, y2;
};

struct drm_format_info {
	u32 format;
	u8 num_planes;
};

struct drm_gem_object {
	size_t size;
};

struct drm_gem_dma_object {
	struct drm_gem_object base;
	dma_addr_t dma_addr;
};

#define to_drm_gem_dma_obj(obj) container_of(obj, struct drm_gem_dma_object, base)

struct drm_framebuffer {
	const struct drm_format_info *format;
	struct drm_gem_object *obj[4];
	u64 modifier;
	int flags;
	unsigned int offsets[4], pitches[4];
	unsigned int width, height;
};

struct drm_afbc_framebuffer {
	struct drm_framebuffer base;
	u32 block_width, block_height, aligned_width, aligned_height;
	u32 offset, afbc_size;
};

#endif
