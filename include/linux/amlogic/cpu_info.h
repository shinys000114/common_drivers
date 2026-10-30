/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __CPU_INFO_H_
#define __CPU_INFO_H_

#define CHIPID_LEN 16
void cpuinfo_get_chipid(unsigned char *cid, unsigned int size);
/* Returns -EPROBE_DEFER until firmware has supplied a valid chip ID. */
int meson_cpu_version_read(unsigned int level, unsigned char *value);

#endif
