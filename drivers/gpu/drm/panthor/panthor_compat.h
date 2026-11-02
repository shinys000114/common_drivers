/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Linux 6.18 helpers missing from the 6.12 kernel this fork builds against. */

#ifndef __PANTHOR_COMPAT_H__
#define __PANTHOR_COMPAT_H__

#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/sizes.h>

#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>

/* 6.18 renamed DRM_GPU_SCHED_STAT_NOMINAL. */
#define DRM_GPU_SCHED_STAT_RESET DRM_GPU_SCHED_STAT_NOMINAL

#ifndef range_overflows
#define range_overflows(start, size, max) ({ \
	typeof(start) start__ = (start); \
	typeof(size) size__ = (size); \
	typeof(max) max__ = (max); \
	(void)(&start__ == &size__); \
	(void)(&start__ == &max__); \
	start__ >= max__ || size__ > max__ - start__; \
})
#endif

static inline void drm_fdinfo_print_size(struct drm_printer *p,
					 const char *prefix,
					 const char *stat,
					 const char *region,
					 u64 sz)
{
	const char *units[] = {"", " KiB", " MiB"};
	unsigned u;

	for (u = 0; u < ARRAY_SIZE(units) - 1; u++) {
		if (sz == 0 || !IS_ALIGNED(sz, SZ_1K))
			break;
		sz = div_u64(sz, SZ_1K);
	}

	drm_printf(p, "%s-%s-%s:\t%llu%s\n",
		   prefix, stat, region, sz, units[u]);
}

#endif
