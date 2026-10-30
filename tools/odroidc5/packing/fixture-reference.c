// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.
#define WB_NO_MAIN
#include "packing-matrix.c"
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const uint32_t formats[] = { DRM_FORMAT_XRGB8888,
				     DRM_FORMAT_XBGR2101010 };
	for (unsigned int i = 0; i < ARRAY_SIZE(formats); i++) {
		struct wb_buffer b = {};
		b.fb.pixel_format = formats[i];
		b.dumb.pitch = WB_W * 4;
		b.dumb.size = (uint64_t)b.dumb.pitch * (WB_H + 2);
		b.fb.offsets[0] = b.dumb.pitch;
		b.map = malloc(b.dumb.size);
		if (!b.map)
			return 1;
		memset(b.map, 0xa5, b.dumb.size);
		wb_fill_source(&b);
		if (wb_buffer_guard(&b))
			return 1;
		char path[PATH_MAX];
		if (snprintf(path, sizeof(path), "%s/%s.raw", argv[1],
			     wb_format_name(formats[i])) >= (int)sizeof(path))
			return 1;
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
			      0600);
		if (fd < 0)
			return 1;
		if (wb_write(fd, b.map + b.fb.offsets[0],
			     (uint64_t)b.dumb.pitch * WB_H) ||
		    close(fd))
			return 1;
		free(b.map);
	}
	return 0;
}
