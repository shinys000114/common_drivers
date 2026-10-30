// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* Standard DRM UAPI only: no GPU, libMali, libdrm or vendor ioctl dependency. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PTR(p) ((uint64_t)(uintptr_t)(p))
#define MAX_OBJECTS 32
#define MAX_PROPS 64
static unsigned int test_width = 1920, test_height = 1080;
/* Stable DRM plane "type" property enum (libdrm normally names this value). */
#define PLANE_TYPE_PRIMARY 1

struct buffer {
	struct drm_mode_create_dumb dumb;
	struct drm_mode_fb_cmd2 fb;
	void *map;
};

struct request {
	uint32_t objects[3], counts[3], props[16];
	uint64_t values[16];
	unsigned int used;
};

static int call(int fd, unsigned long cmd, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, cmd, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int property(int fd, uint32_t object, uint32_t type, const char *name,
		    uint32_t *id, uint64_t *value)
{
	uint32_t ids[MAX_PROPS];
	uint64_t values[MAX_PROPS];
	struct drm_mode_obj_get_properties list = {
		.obj_id = object,
		.obj_type = type,
		.count_props = MAX_PROPS,
		.props_ptr = PTR(ids),
		.prop_values_ptr = PTR(values),
	};
	unsigned int i;

	if (call(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list))
		return -1;
	if (list.count_props > MAX_PROPS) {
		errno = EOVERFLOW;
		return -1;
	}
	for (i = 0; i < list.count_props; i++) {
		struct drm_mode_get_property prop = { .prop_id = ids[i] };

		if (call(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop))
			return -1;
		if (!strcmp(prop.name, name)) {
			*id = ids[i];
			*value = values[i];
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

static int add(int fd, struct request *r, unsigned int object, uint32_t type,
	       const char *name, uint64_t value)
{
	uint64_t previous;
	uint32_t id;

	if (r->used == ARRAY_SIZE(r->props)) {
		errno = EOVERFLOW;
		return -1;
	}
	if (property(fd, r->objects[object], type, name, &id, &previous))
		return -1;
	r->props[r->used] = id;
	r->values[r->used++] = value;
	r->counts[object]++;
	return 0;
}

static int prepare_request(int fd, struct request *r, uint32_t blob,
			   uint32_t fb)
{
	static const char *const names[] = {
		"FB_ID", "CRTC_ID", "SRC_X",  "SRC_Y",	"SRC_W",
		"SRC_H", "CRTC_X",  "CRTC_Y", "CRTC_W", "CRTC_H",
	};
	uint64_t values[] = { fb,
			      r->objects[1],
			      0,
			      0,
			      test_width << 16,
			      test_height << 16,
			      0,
			      0,
			      test_width,
			      test_height };
	unsigned int i;

	if (add(fd, r, 0, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID",
		r->objects[1]) ||
	    add(fd, r, 1, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1) ||
	    add(fd, r, 1, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob))
		return -1;
	for (i = 0; i < ARRAY_SIZE(names); i++)
		if (add(fd, r, 2, DRM_MODE_OBJECT_PLANE, names[i], values[i]))
			return -1;
	return 0;
}

static int atomic_commit(int fd, struct request *r, uint32_t flags,
			 uint64_t token)
{
	struct drm_mode_atomic cmd = {
		.flags = flags,
		.count_objs = 3,
		.objs_ptr = PTR(r->objects),
		.count_props_ptr = PTR(r->counts),
		.props_ptr = PTR(r->props),
		.prop_values_ptr = PTR(r->values),
		.user_data = token,
	};

	return call(fd, DRM_IOCTL_MODE_ATOMIC, &cmd);
}

static uint32_t test_format = DRM_FORMAT_XRGB8888;

static int make_buffer(int fd, struct buffer *b, unsigned int index)
{
	static const uint32_t colors[] = {
		0x00ffffff, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000,
	};
	struct drm_mode_map_dumb map = {};
	unsigned int x, y;

	b->dumb = (struct drm_mode_create_dumb){ .width = test_width,
						 .height = test_height,
						 .bpp = 32 };
	if (call(fd, DRM_IOCTL_MODE_CREATE_DUMB, &b->dumb))
		return -1;
	b->fb = (struct drm_mode_fb_cmd2){
		.width = test_width,
		.height = test_height,
		.pixel_format = test_format,
		.handles[0] = b->dumb.handle,
		.pitches[0] = b->dumb.pitch,
	};
	if (call(fd, DRM_IOCTL_MODE_ADDFB2, &b->fb))
		return -1;
	map.handle = b->dumb.handle;
	if (call(fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		return -1;
	b->map = mmap(NULL, b->dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fd, map.offset);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		return -1;
	}
	for (y = 0; y < test_height; y++) {
		uint32_t *line =
			(uint32_t *)((char *)b->map + y * b->dumb.pitch);

		for (x = 0; x < test_width; x++) {
			uint32_t pixel = y < 80 ?
						 (index ? 0x00ffffff : 0) :
						 colors[x * ARRAY_SIZE(colors) /
							test_width];

			if (test_format == DRM_FORMAT_XBGR8888 ||
			    test_format == DRM_FORMAT_BGRX8888)
				pixel = ((pixel & 0xff) << 16) |
					(pixel & 0xff00) |
					((pixel >> 16) & 0xff);
			if (test_format == DRM_FORMAT_RGBX8888 ||
			    test_format == DRM_FORMAT_BGRX8888)
				pixel <<= 8;
			line[x] = pixel;
		}
	}
	return 0;
}

static int wait_flip(int fd, uint64_t token, uint32_t *sequence)
{
	struct pollfd poller = { .fd = fd, .events = POLLIN };
	union {
		uint64_t align;
		char data[4096];
	} events;
	ssize_t len;
	size_t offset = 0;
	int ret;

	do {
		ret = poll(&poller, 1, 3000);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0) {
		if (!ret)
			errno = ETIMEDOUT;
		return -1;
	}
	len = read(fd, events.data, sizeof(events.data));
	if (len < 0)
		return -1;
	while (offset + sizeof(struct drm_event) <= (size_t)len) {
		struct drm_event *event = (void *)(events.data + offset);

		if (event->length < sizeof(*event) ||
		    event->length > (size_t)len - offset)
			break;
		if (event->type == DRM_EVENT_FLIP_COMPLETE &&
		    event->length == sizeof(struct drm_event_vblank)) {
			struct drm_event_vblank *flip = (void *)event;

			if (flip->user_data == token) {
				*sequence = flip->sequence;
				return 0;
			}
		}
		offset += event->length;
	}
	errno = EPROTO;
	return -1;
}

static int check_sink(int fd, uint32_t connector)
{
	struct drm_mode_get_connector info = { .connector_id = connector };
	struct drm_mode_get_blob blob = {};
	uint32_t id;
	uint64_t value;
	unsigned char *edid;
	unsigned int i, capacity, sum = 0;
	int ret = -1;

	if (call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &info))
		return -1;
	if (info.connection != 1 || !info.count_modes) {
		errno = ENODEV;
		return -1;
	}
	if (property(fd, connector, DRM_MODE_OBJECT_CONNECTOR, "EDID", &id,
		     &value))
		return -1;
	blob.blob_id = value;
	if (!value) {
		errno = ENOENT;
		return -1;
	}
	if (call(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob))
		return -1;
	if (!blob.length || blob.length > 32768 || blob.length % 128) {
		errno = EPROTO;
		return -1;
	}
	capacity = blob.length;
	edid = calloc(1, capacity);
	if (!edid)
		return -1;
	blob.data = PTR(edid);
	if (call(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob))
		goto out;
	if (blob.length != capacity) {
		errno = EAGAIN;
		goto out;
	}
	for (i = 0; i < blob.length; i++) {
		sum += edid[i];
		if (i % 128 == 127 && (sum & 0xff)) {
			errno = EPROTO;
			goto out;
		}
	}
	printf("Native connector=%u connected, modes=%u, EDID=%u bytes checksums valid\n",
	       connector, info.count_modes, blob.length);
	ret = 0;
out:
	free(edid);
	return ret;
}

static int discover(int fd, struct request *r)
{
	uint32_t crtcs[MAX_OBJECTS], connectors[MAX_OBJECTS],
		planes[MAX_OBJECTS];
	struct drm_mode_card_res res = {
		.crtc_id_ptr = PTR(crtcs),
		.connector_id_ptr = PTR(connectors),
		.count_crtcs = MAX_OBJECTS,
		.count_connectors = MAX_OBJECTS,
	};
	struct drm_mode_get_plane_res pr = {
		.plane_id_ptr = PTR(planes),
		.count_planes = MAX_OBJECTS,
	};
	unsigned int i;

	if (call(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) ||
	    call(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr))
		return -1;
	if (res.count_crtcs != 1 || res.count_connectors != 1 ||
	    pr.count_planes > MAX_OBJECTS) {
		errno = ENOTSUP;
		return -1;
	}
	r->objects[0] = connectors[0];
	r->objects[1] = crtcs[0];
	for (i = 0; i < pr.count_planes; i++) {
		struct drm_mode_get_plane plane = { .plane_id = planes[i] };
		uint32_t id;
		uint64_t value;

		if (call(fd, DRM_IOCTL_MODE_GETPLANE, &plane) ||
		    property(fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &id,
			     &value))
			return -1;
		if (value == PLANE_TYPE_PRIMARY && (plane.possible_crtcs & 1)) {
			r->objects[2] = planes[i];
			return 0;
		}
	}
	errno = ENODEV;
	return -1;
}

int main(int argc, char **argv)
{
	struct drm_mode_modeinfo mode = {
		.clock = 148500,
		.hdisplay = 1920,
		.hsync_start = 2008,
		.hsync_end = 2052,
		.htotal = 2200,
		.vdisplay = 1080,
		.vsync_start = 1084,
		.vsync_end = 1089,
		.vtotal = 1125,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
		.type = DRM_MODE_TYPE_USERDEF,
		.name = "1920x1080",
	};
	struct drm_mode_create_blob blob = { .data = PTR(&mode),
					     .length = sizeof(mode) };
	struct drm_mode_crtc saved = {}, after = {};
	struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_ATOMIC,
					  .value = 1 };
	struct drm_version version = {};
	struct buffer buffers[2] = {};
	struct request request = {};
	char name[64] = {};
	bool changed = false;
	uint32_t first = 0, last = 0;
	unsigned int i, completed = 0, hold = 0;
	int fd, ret = 1;

	if ((argc != 4 && argc != 6) || strcmp(argv[2], "--run") ||
	    (strcmp(argv[3], "60") && strcmp(argv[3], "59.94") &&
	     strcmp(argv[3], "4k60") && strcmp(argv[3], "4k59.94") &&
	     strcmp(argv[3], "4k30") && strcmp(argv[3], "4k29.97"))) {
		fprintf(stderr,
			"Usage: %s /dev/dri/cardN --run 60|59.94|4k60|4k59.94|4k30|4k29.97 [--hold 1..60]\n",
			argv[0]);
		return 2;
	}
	if (argc == 6) {
		char *end;
		unsigned long value;

		errno = 0;
		value = strtoul(argv[5], &end, 10);
		if (strcmp(argv[4], "--hold") || errno || !*argv[5] || *end ||
		    value < 1 || value > 60)
			return 2;
		hold = value;
	}
	const char *format = getenv("C5_KMS_FORMAT");
	if (format) {
		if (!strcmp(format, "XR24"))
			test_format = DRM_FORMAT_XRGB8888;
		else if (!strcmp(format, "XB24"))
			test_format = DRM_FORMAT_XBGR8888;
		else if (!strcmp(format, "RX24"))
			test_format = DRM_FORMAT_RGBX8888;
		else if (!strcmp(format, "BX24"))
			test_format = DRM_FORMAT_BGRX8888;
		else
			return 2;
	}
	setbuf(stdout, NULL);
	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open DRM");
		return 1;
	}
	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (call(fd, DRM_IOCTL_VERSION, &version) ||
	    version.name_len >= sizeof(name) || strcmp(name, "s7d-drm")) {
		fprintf(stderr,
			"Refusing driver '%s': native s7d-drm required; no modeset issued\n",
			name);
		close(fd);
		return 2;
	}
	if (!strcmp(argv[3], "59.94"))
		mode.clock = 148352;
	if (!strncmp(argv[3], "4k", 2)) {
		test_width = 3840;
		test_height = 2160;
		mode.hdisplay = 3840;
		mode.hsync_start = 4016;
		mode.hsync_end = 4104;
		mode.htotal = 4400;
		mode.vdisplay = 2160;
		mode.vsync_start = 2168;
		mode.vsync_end = 2178;
		mode.vtotal = 2250;
		strcpy(mode.name, "3840x2160");
		if (!strcmp(argv[3], "4k60"))
			mode.clock = 594000;
		if (!strcmp(argv[3], "4k59.94"))
			mode.clock = 593407;
		if (!strcmp(argv[3], "4k30"))
			mode.clock = 297000;
		if (!strcmp(argv[3], "4k29.97"))
			mode.clock = 296703;
	}
	if (call(fd, DRM_IOCTL_SET_MASTER, NULL) ||
	    call(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) ||
	    discover(fd, &request) || check_sink(fd, request.objects[0]))
		goto fail;
	saved.crtc_id = request.objects[1];
	if (call(fd, DRM_IOCTL_MODE_GETCRTC, &saved) ||
	    call(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) ||
	    make_buffer(fd, &buffers[0], 0) ||
	    make_buffer(fd, &buffers[1], 1) ||
	    prepare_request(fd, &request, blob.blob_id, buffers[0].fb.fb_id))
		goto fail;
	if (atomic_commit(fd, &request,
			  DRM_MODE_ATOMIC_TEST_ONLY |
				  DRM_MODE_ATOMIC_ALLOW_MODESET,
			  0))
		goto fail;
	after.crtc_id = saved.crtc_id;
	if (call(fd, DRM_IOCTL_MODE_GETCRTC, &after))
		goto fail;
	if (after.fb_id != saved.fb_id ||
	    after.mode_valid != saved.mode_valid ||
	    memcmp(&after.mode, &saved.mode, sizeof(saved.mode))) {
		errno = EIO;
		goto fail;
	}
	puts("TEST_ONLY accepted; reported CRTC state unchanged (not a register audit)");
	for (i = 0; i < 60; i++) {
		uint32_t sequence;

		/* First plane property is FB_ID; retain all other checked values. */
		request.values[3] = buffers[i & 1].fb.fb_id;
		changed = true;
		if (atomic_commit(fd, &request,
				  DRM_MODE_ATOMIC_ALLOW_MODESET |
					  DRM_MODE_ATOMIC_NONBLOCK |
					  DRM_MODE_PAGE_FLIP_EVENT,
				  i + 1) ||
		    wait_flip(fd, i + 1, &sequence))
			goto fail;
		if (i && (int32_t)(sequence - last) <= 0) {
			errno = EIO;
			goto fail;
		}
		if (!i)
			first = sequence;
		last = sequence;
		completed++;
		if (!i && hold) {
			struct timespec remaining = { .tv_sec = hold };

			printf("HOLD: initial framebuffer for %u seconds; inspect physical bars\n",
			       hold);
			while (nanosleep(&remaining, &remaining)) {
				if (errno != EINTR)
					goto fail;
			}
		}
	}
	printf("PASS: %ux%u clock=%u kHz, %u flips, vblank=%u..%u; inspect physical color bars\n",
	       mode.hdisplay, mode.vdisplay, mode.clock, completed, first,
	       last);
	ret = 0;
	goto cleanup;
fail:
	fprintf(stderr, "FAIL after %u flips: %s\n", completed,
		strerror(errno));
cleanup:
	if (changed) {
		/* Console fb stays owned by fbdev; report if its ID was removed meanwhile. */
		saved.set_connectors_ptr = PTR(&request.objects[0]);
		saved.count_connectors = saved.mode_valid ? 1 : 0;
		if (call(fd, DRM_IOCTL_MODE_SETCRTC, &saved)) {
			perror("restore previous CRTC");
			ret = 1;
		}
	}
	for (i = 0; i < ARRAY_SIZE(buffers); i++) {
		struct buffer *b = &buffers[i];
		struct drm_mode_destroy_dumb destroy = {
			.handle = b->dumb.handle
		};

		if (b->map)
			munmap(b->map, b->dumb.size);
		if (b->fb.fb_id)
			call(fd, DRM_IOCTL_MODE_RMFB, &b->fb.fb_id);
		if (b->dumb.handle)
			call(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	if (blob.blob_id) {
		struct drm_mode_destroy_blob destroy = { .blob_id =
								 blob.blob_id };

		call(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &destroy);
	}
	close(fd);
	return ret;
}
