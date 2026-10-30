// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
/* Local board harness. Standard DRM UAPI; no raw MMIO or private ioctl. */
#define main reused_native_smoke_main
#include "c5-kms-base.c"
#undef main
#include <limits.h>
#include <signal.h>
#include <linux/sync_file.h>

#define CP_INPUTS 4
#define CP_OBJECTS 7
#define CP_STAGES 64
#define CP_MAX_UPDATES 512
#define CP_DEFAULT_FLIPS 4
#define CP_EDID_MAX_BYTES 32768

/* Native s7d-drv.c creation order: OSD1, cursor, OSD2, VD1, VD2. */
enum cp_object {
	CP_CONN,
	CP_CRTC,
	CP_OSD1,
	CP_OSD2,
	CP_VD1,
	CP_VD2,
	CP_CURSOR
};
static const char *const cp_names[] = { "connector", "CRTC", "OSD1",  "OSD2",
					"VD1",	     "VD2",  "cursor" };
static const char *const cp_geometry[] = { "FB_ID",  "CRTC_ID", "SRC_X",
					   "SRC_Y",  "SRC_W",	"SRC_H",
					   "CRTC_X", "CRTC_Y",	"CRTC_W",
					   "CRTC_H" };
static volatile sig_atomic_t cp_stop, cp_release;
static unsigned int cp_width = 1024, cp_height = 600, cp_factor = 1;

struct cp_prop {
	uint32_t id, flags;
	uint64_t value;
	char name[DRM_PROP_NAME_LEN];
};
struct cp_object_state {
	uint32_t id, type, count;
	struct cp_prop props[MAX_PROPS];
};
struct cp_state {
	struct drm_mode_crtc crtc;
	struct cp_object_state objects[CP_OBJECTS];
};
struct cp_req {
	uint32_t objects[CP_OBJECTS], counts[CP_OBJECTS], props[128];
	uint64_t values[128];
	unsigned int count, used;
};
struct cp_image {
	struct drm_mode_create_dumb dumb[2];
	struct drm_mode_fb_cmd2 fb;
	void *map[2];
	unsigned int objects;
};
struct cp_layer {
	uint32_t fb, w, h, dw, dh;
	int x, y;
	uint64_t z, alpha, rotation;
};
struct cp_scene {
	const char *name;
	struct cp_layer layer[CP_INPUTS];
	bool modeset;
};
struct cp_context {
	int fd, fence;
	uint32_t ids[CP_OBJECTS], mode_blob, restore_blob, sequence, first;
	uint64_t token, none[2], encoding[2], range[2];
	unsigned int tests, commits;
	bool master, changed, pending;
	struct cp_state saved;
	/* Owned byte copy; driver may replace/drop its EDID blob after probing. */
	unsigned char saved_edid[CP_EDID_MAX_BYTES];
	uint32_t saved_edid_length;
	uint64_t saved_edid_blob_id;
	bool saved_edid_captured;
};
struct cp_crc {
	char control[PATH_MAX], data[PATH_MAX], original[64];
	bool changed;
};

static void cp_signal(int signal)
{
	if (signal == SIGUSR1)
		cp_release = 1;
	else
		cp_stop = 1;
}

static uint64_t cp_now(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static int cp_number(const char *text, unsigned int min, unsigned int max,
		     unsigned int *out)
{
	char *end;
	unsigned long n;
	errno = 0;
	n = strtoul(text, &end, 10);
	if (errno || !*text || *end || n < min || n > max) {
		errno = EINVAL;
		return -1;
	}
	*out = n;
	return 0;
}

static int cp_read_object(int fd, uint32_t id, uint32_t type,
			  struct cp_object_state *out)
{
	uint32_t ids[MAX_PROPS];
	uint64_t values[MAX_PROPS];
	struct drm_mode_obj_get_properties list = { .obj_id = id,
						    .obj_type = type,
						    .count_props = MAX_PROPS,
						    .props_ptr = PTR(ids),
						    .prop_values_ptr =
							    PTR(values) };
	memset(out, 0, sizeof(*out));
	out->id = id;
	out->type = type;
	if (call(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list))
		return -1;
	if (list.count_props > MAX_PROPS) {
		errno = EOVERFLOW;
		return -1;
	}
	out->count = list.count_props;
	for (unsigned int i = 0; i < list.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = ids[i] };
		if (call(fd, DRM_IOCTL_MODE_GETPROPERTY, &p))
			return -1;
		out->props[i].id = ids[i];
		out->props[i].value = values[i];
		out->props[i].flags = p.flags;
		memcpy(out->props[i].name, p.name, sizeof(p.name));
		out->props[i].name[sizeof(out->props[i].name) - 1] = 0;
	}
	return 0;
}

static const struct cp_prop *cp_find(const struct cp_object_state *obj,
				     const char *name)
{
	for (unsigned int i = 0; i < obj->count; i++)
		if (!strcmp(obj->props[i].name, name))
			return &obj->props[i];
	errno = ENOENT;
	return NULL;
}

static int cp_snapshot(struct cp_context *c, struct cp_state *out)
{
	memset(out, 0, sizeof(*out));
	out->crtc.crtc_id = c->ids[CP_CRTC];
	if (call(c->fd, DRM_IOCTL_MODE_GETCRTC, &out->crtc))
		return -1;
	for (unsigned int i = 0; i < CP_OBJECTS; i++)
		if (cp_read_object(c->fd, c->ids[i],
				   i == CP_CONN ? DRM_MODE_OBJECT_CONNECTOR :
				   i == CP_CRTC ? DRM_MODE_OBJECT_CRTC :
						  DRM_MODE_OBJECT_PLANE,
				   &out->objects[i]))
			return -1;
	return 0;
}

static int cp_read_edid_blob(struct cp_context *c, uint64_t id,
			     unsigned char *bytes, uint32_t *length,
			     const char *phase)
{
	struct drm_mode_get_blob blob = {};
	uint32_t capacity;
	if (!id || id > UINT32_MAX) {
		fprintf(stderr,
			"EDID_READ_ERROR phase=%s stage=blob-id observed=%llu expected=1..UINT32_MAX\n",
			phase, (unsigned long long)id);
		errno = EPROTO;
		return -1;
	}
	blob.blob_id = id;
	if (call(c->fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob)) {
		int error = errno;
		fprintf(stderr,
			"EDID_READ_ERROR phase=%s stage=query blob_id=%llu errno=%d\n",
			phase, (unsigned long long)id, error);
		errno = error;
		return -1;
	}
	capacity = blob.length;
	if (!capacity || capacity > CP_EDID_MAX_BYTES || capacity % 128) {
		fprintf(stderr,
			"EDID_READ_ERROR phase=%s stage=length blob_id=%llu observed_length=%u expected=128-byte-blocks-up-to-%u\n",
			phase, (unsigned long long)id, capacity,
			CP_EDID_MAX_BYTES);
		errno = EPROTO;
		return -1;
	}
	blob.data = PTR(bytes);
	if (call(c->fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob)) {
		int error = errno;
		fprintf(stderr,
			"EDID_READ_ERROR phase=%s stage=read blob_id=%llu errno=%d\n",
			phase, (unsigned long long)id, error);
		errno = error;
		return -1;
	}
	if (blob.length != capacity) {
		fprintf(stderr,
			"EDID_READ_ERROR phase=%s stage=changed-length blob_id=%llu expected_length=%u observed_length=%u\n",
			phase, (unsigned long long)id, capacity, blob.length);
		errno = EAGAIN;
		return -1;
	}
	*length = capacity;
	return 0;
}

static int cp_capture_edid(struct cp_context *c)
{
	const struct cp_prop *p = cp_find(&c->saved.objects[CP_CONN], "EDID");
	c->saved_edid_captured = false;
	c->saved_edid_length = 0;
	c->saved_edid_blob_id = 0;
	if (!p || !(p->flags & DRM_MODE_PROP_BLOB)) {
		fprintf(stderr,
			"EDID_CAPTURE_ERROR present=%u observed_flags=0x%x expected_blob_property=1\n",
			!!p, p ? p->flags : 0);
		errno = EPROTO;
		return -1;
	}
	/* Capture before GETCONNECTOR/check_sink can refresh the driver-owned ID. */
	if (cp_read_edid_blob(c, p->value, c->saved_edid, &c->saved_edid_length,
			      "setup"))
		return -1;
	c->saved_edid_blob_id = p->value;
	c->saved_edid_captured = true;
	printf("EDID_CAPTURE blob_id=%llu length=%u owner=userspace-byte-copy original_blob_lifetime_not_required=1\n",
	       (unsigned long long)c->saved_edid_blob_id, c->saved_edid_length);
	return 0;
}

static int cp_compare_edid(struct cp_context *c, const struct cp_prop *actual)
{
	unsigned char observed[CP_EDID_MAX_BYTES];
	uint32_t length = 0;
	if (!c->saved_edid_captured || !c->saved_edid_length) {
		fputs("RESTORE_EDID_ERROR stage=saved-metadata expected=captured-byte-copy observed=missing\n",
		      stderr);
		errno = EPROTO;
		return -1;
	}
	/* Always read content, including when an ID has been recycled unchanged. */
	if (cp_read_edid_blob(c, actual->value, observed, &length, "restore"))
		return -1;
	if (length != c->saved_edid_length) {
		fprintf(stderr,
			"RESTORE_EDID_ERROR stage=content-length expected_blob_id=%llu observed_blob_id=%llu expected_length=%u observed_length=%u\n",
			(unsigned long long)c->saved_edid_blob_id,
			(unsigned long long)actual->value, c->saved_edid_length,
			length);
		errno = EPROTO;
		return -1;
	}
	for (unsigned int i = 0; i < length; i++)
		if (observed[i] != c->saved_edid[i]) {
			fprintf(stderr,
				"RESTORE_EDID_ERROR stage=content-byte expected_blob_id=%llu observed_blob_id=%llu offset=%u expected=0x%02x observed=0x%02x length=%u\n",
				(unsigned long long)c->saved_edid_blob_id,
				(unsigned long long)actual->value, i,
				c->saved_edid[i], observed[i], length);
			errno = EPROTO;
			return -1;
		}
	printf("RESTORE_EDID expected_blob_id=%llu observed_blob_id=%llu length=%u content_equal=1 numeric_id_changed=%u\n",
	       (unsigned long long)c->saved_edid_blob_id,
	       (unsigned long long)actual->value, length,
	       actual->value != c->saved_edid_blob_id);
	return 0;
}

static void cp_print_snapshot(const struct cp_state *s)
{
	printf("SNAPSHOT crtc=%u active_mode=%u fb=%u mode=%ux%u clock_khz=%u\n",
	       s->crtc.crtc_id, s->crtc.mode_valid, s->crtc.fb_id,
	       s->crtc.mode.hdisplay, s->crtc.mode.vdisplay,
	       s->crtc.mode.clock);
	for (unsigned int i = 0; i < CP_OBJECTS; i++)
		for (unsigned int j = 0; j < s->objects[i].count; j++) {
			const struct cp_prop *p = &s->objects[i].props[j];
			printf("SNAPSHOT_PROPERTY object=%s id=%u prop=%u name=\"%s\" value=%llu flags=0x%x\n",
			       cp_names[i], s->objects[i].id, p->id, p->name,
			       (unsigned long long)p->value, p->flags);
		}
}

static int cp_enum(struct cp_context *c, unsigned int obj, const char *prop,
		   const char *name, uint64_t *value)
{
	const struct cp_prop *p = cp_find(&c->saved.objects[obj], prop);
	struct drm_mode_property_enum enums[32];
	struct drm_mode_get_property query = { .count_enum_blobs =
						       ARRAY_SIZE(enums),
					       .enum_blob_ptr = PTR(enums) };
	if (!p)
		return -1;
	query.prop_id = p->id;
	if (call(c->fd, DRM_IOCTL_MODE_GETPROPERTY, &query))
		return -1;
	if (!(query.flags & DRM_MODE_PROP_ENUM) ||
	    query.count_enum_blobs > ARRAY_SIZE(enums)) {
		errno = EPROTO;
		return -1;
	}
	for (unsigned int i = 0; i < query.count_enum_blobs; i++)
		if (!strcmp(enums[i].name, name)) {
			*value = enums[i].value;
			return 0;
		}
	errno = ENOENT;
	return -1;
}

static int cp_discover(struct cp_context *c, uint32_t vd1, uint32_t vd2)
{
	uint32_t crtcs[MAX_OBJECTS], connectors[MAX_OBJECTS],
		planes[MAX_OBJECTS], videos[2];
	unsigned int nv = 0;
	struct drm_mode_card_res res = { .crtc_id_ptr = PTR(crtcs),
					 .connector_id_ptr = PTR(connectors),
					 .count_crtcs = MAX_OBJECTS,
					 .count_connectors = MAX_OBJECTS };
	struct drm_mode_get_plane_res list = { .plane_id_ptr = PTR(planes),
					       .count_planes = MAX_OBJECTS };
	if (call(c->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) ||
	    call(c->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &list))
		return -1;
	if (res.count_crtcs != 1 || res.count_connectors != 1 ||
	    list.count_planes != 5) {
		errno = ENOTSUP;
		return -1;
	}
	c->ids[CP_CONN] = connectors[0];
	c->ids[CP_CRTC] = crtcs[0];
	for (unsigned int i = 0; i < list.count_planes; i++) {
		uint32_t formats[64], id;
		uint64_t type, z;
		struct drm_mode_get_plane p = {
			.plane_id = planes[i],
			.count_format_types = ARRAY_SIZE(formats),
			.format_type_ptr = PTR(formats)
		};
		bool rgb = false, nv12 = false, nv21 = false;
		if (call(c->fd, DRM_IOCTL_MODE_GETPLANE, &p) ||
		    property(c->fd, p.plane_id, DRM_MODE_OBJECT_PLANE, "type",
			     &id, &type) ||
		    property(c->fd, p.plane_id, DRM_MODE_OBJECT_PLANE, "zpos",
			     &id, &z))
			return -1;
		if (p.count_format_types > ARRAY_SIZE(formats) ||
		    p.possible_crtcs != 1) {
			errno = ENOTSUP;
			return -1;
		}
		for (unsigned int f = 0; f < p.count_format_types; f++) {
			rgb |= formats[f] == DRM_FORMAT_XRGB8888;
			nv12 |= formats[f] == DRM_FORMAT_NV12;
			nv21 |= formats[f] == DRM_FORMAT_NV21;
		}
		printf("PLANE_ENUM index=%u id=%u type=%llu saved_zpos=%llu RGB=%u NV12=%u NV21=%u\n",
		       i, p.plane_id, (unsigned long long)type,
		       (unsigned long long)z, rgb, nv12, nv21);
		if (type == 1 && rgb && !nv12)
			c->ids[CP_OSD1] = p.plane_id;
		else if (type == 2 && !nv12)
			c->ids[CP_CURSOR] = p.plane_id;
		else if (type == 0 && rgb && !nv12)
			c->ids[CP_OSD2] = p.plane_id;
		else if (type == 0 && nv12 && nv21 && !rgb && nv < 2)
			videos[nv++] = p.plane_id;
		else {
			errno = ENOTSUP;
			return -1;
		}
	}
	if (nv != 2 || !c->ids[CP_OSD1] || !c->ids[CP_OSD2] ||
	    !c->ids[CP_CURSOR]) {
		errno = ENODEV;
		return -1;
	}
	if (vd1 || vd2) {
		if (!vd1 || !vd2 || vd1 == vd2 ||
		    !((vd1 == videos[0] && vd2 == videos[1]) ||
		      (vd1 == videos[1] && vd2 == videos[0]))) {
			errno = EINVAL;
			return -1;
		}
		c->ids[CP_VD1] = vd1;
		c->ids[CP_VD2] = vd2;
	} else {
		/* Native DRM plane-list order follows registration, never mutable zpos. */
		c->ids[CP_VD1] = videos[0];
		c->ids[CP_VD2] = videos[1];
	}
	printf("DISCOVERY connector=%u crtc=%u OSD1=%u OSD2=%u VD1=%u VD2=%u cursor=%u video_mapping=%s\n",
	       c->ids[0], c->ids[1], c->ids[2], c->ids[3], c->ids[4], c->ids[5],
	       c->ids[6], vd1 ? "explicit" : "native-registration-order");
	if (cp_snapshot(c, &c->saved))
		return -1;
	return cp_capture_edid(c);
}

static int cp_obj(struct cp_context *c, struct cp_req *r, unsigned int index)
{
	if (index >= CP_OBJECTS || r->count >= CP_OBJECTS) {
		errno = EOVERFLOW;
		return -1;
	}
	r->objects[r->count] = c->ids[index];
	return r->count++;
}

static int cp_add(struct cp_context *c, struct cp_req *r, unsigned int obj,
		  const char *name, uint64_t value)
{
	const struct cp_prop *p;
	if (!r->count || obj >= CP_OBJECTS ||
	    r->objects[r->count - 1] != c->ids[obj] ||
	    r->used >= ARRAY_SIZE(r->props)) {
		errno = EOVERFLOW;
		return -1;
	}
	p = cp_find(&c->saved.objects[obj], name);
	if (!p || (p->flags & DRM_MODE_PROP_IMMUTABLE)) {
		errno = p ? ENOTSUP : ENOENT;
		return -1;
	}
	r->props[r->used] = p->id;
	r->values[r->used++] = value;
	r->counts[r->count - 1]++;
	return 0;
}

static int cp_send(struct cp_context *c, struct cp_req *r, uint32_t flags,
		   uint64_t token)
{
	struct drm_mode_atomic a = { .count_objs = r->count,
				     .objs_ptr = PTR(r->objects),
				     .count_props_ptr = PTR(r->counts),
				     .props_ptr = PTR(r->props),
				     .prop_values_ptr = PTR(r->values),
				     .flags = flags,
				     .user_data = token };
	return call(c->fd, DRM_IOCTL_MODE_ATOMIC, &a);
}

static int cp_build(struct cp_context *c, const struct cp_scene *s, bool mode,
		    int *fence, struct cp_req *r)
{
	memset(r, 0, sizeof(*r));
	if (mode && (cp_obj(c, r, CP_CONN) < 0 ||
		     cp_add(c, r, CP_CONN, "CRTC_ID", c->ids[CP_CRTC])))
		return -1;
	if (cp_obj(c, r, CP_CRTC) < 0 || cp_add(c, r, CP_CRTC, "ACTIVE", 1) ||
	    (mode && cp_add(c, r, CP_CRTC, "MODE_ID", c->mode_blob)) ||
	    cp_add(c, r, CP_CRTC, "OUT_FENCE_PTR", PTR(fence)))
		return -1;
	for (unsigned int i = 0; i < CP_INPUTS; i++) {
		const struct cp_layer *p = &s->layer[i];
		unsigned int obj = CP_OSD1 + i;
		uint64_t values[] = { p->fb,
				      p->fb ? c->ids[CP_CRTC] : 0,
				      0,
				      0,
				      (uint64_t)p->w << 16,
				      (uint64_t)p->h << 16,
				      (uint64_t)(int64_t)p->x,
				      (uint64_t)(int64_t)p->y,
				      p->dw,
				      p->dh };
		if (cp_obj(c, r, obj) < 0)
			return -1;
		for (unsigned int j = 0;
		     j < (p->fb ? ARRAY_SIZE(cp_geometry) : 2); j++)
			if (cp_add(c, r, obj, cp_geometry[j], values[j]))
				return -1;
		if (cp_add(c, r, obj, "zpos", p->z) ||
		    cp_add(c, r, obj, "alpha", p->alpha) ||
		    cp_add(c, r, obj, "rotation", p->rotation))
			return -1;
		if (i < 2) {
			if (cp_add(c, r, obj, "pixel blend mode", c->none[i]))
				return -1;
		} else if (cp_add(c, r, obj, "COLOR_ENCODING",
				  c->encoding[i - 2]) ||
			   cp_add(c, r, obj, "COLOR_RANGE", c->range[i - 2]) ||
			   cp_add(c, r, obj, "IN_FENCE_FD",
				  (uint64_t)(int64_t)(p->fb ? c->fence : -1)))
			return -1;
	}
	/* Cursor shares OSD2; it must be explicitly disabled for the four-input scene. */
	if (cp_obj(c, r, CP_CURSOR) < 0 ||
	    cp_add(c, r, CP_CURSOR, "FB_ID", 0) ||
	    cp_add(c, r, CP_CURSOR, "CRTC_ID", 0))
		return -1;
	return 0;
}

static int cp_test(struct cp_context *c, const struct cp_scene *s, bool mode,
		   int expected)
{
	struct cp_req r;
	struct cp_state before, after;
	int fence = -1, error, result;
	if (cp_snapshot(c, &before) || cp_build(c, s, mode, &fence, &r))
		return -1;
	result = cp_send(
		c, &r,
		DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
	error = result ? errno : 0;
	if ((expected && (!result || error != expected)) ||
	    (!expected && result)) {
		fprintf(stderr, "TEST_ONLY scene=%s errno=%d expected=%d\n",
			s->name, error, expected);
		errno = EPROTO;
		return -1;
	}
	if (fence != -1) {
		close(fence);
		errno = EPROTO;
		return -1;
	}
	if (cp_snapshot(c, &after))
		return -1;
	if (memcmp(&before, &after, sizeof(before))) {
		errno = EPROTO;
		return -1;
	}
	c->tests++;
	printf("TEST_ONLY scene=%s accepted=%u errno=%d reported_state_unchanged=1\n",
	       s->name, !result, error);
	return 0;
}

static int cp_fence_status(int fd, int *status)
{
	struct sync_file_info info = {};
	int flags;
	if (call(fd, SYNC_IOC_FILE_INFO, &info))
		return -1;
	*status = info.status;
	flags = fcntl(fd, F_GETFD);
	if (flags < 0)
		return -1;
	if (!info.num_fences || !(flags & FD_CLOEXEC)) {
		fprintf(stderr,
			"FENCE_PROTOCOL fd=%d stage=metadata expected_num_fences=>0 observed_num_fences=%u expected_CLOEXEC=1 observed_fd_flags=0x%x status=%d\n",
			fd, info.num_fences, flags, info.status);
		errno = EPROTO;
		return -1;
	}
	if (info.status < 0) {
		errno = info.status >= -4095 ? -info.status : EIO;
		return -1;
	}
	if (info.status != 0 && info.status != 1) {
		fprintf(stderr,
			"FENCE_PROTOCOL fd=%d stage=status expected_nonnegative_status=0-or-1 observed_status=%d num_fences=%u\n",
			fd, info.status, info.num_fences);
		errno = EPROTO;
		return -1;
	}
	return info.status;
}

static int cp_wait_completion(int fd, int fence, uint64_t token,
			      uint32_t *sequence)
{
	uint64_t deadline = cp_now() + 3000000000ULL;
	bool event_done = false, fence_done = false;
	while (!event_done || !fence_done) {
		struct pollfd pollers[] = {
			{ .fd = event_done ? -1 : fd, .events = POLLIN },
			{ .fd = fence_done ? -1 : fence, .events = POLLIN }
		};
		uint64_t now = cp_now();
		int ret;
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		int remaining =
			(int)((deadline - now + 999999ULL) / 1000000ULL);
		ret = poll(pollers, ARRAY_SIZE(pollers), remaining);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0) {
			if (!ret)
				errno = ETIMEDOUT;
			return -1;
		}
		/* Error fences can arrive without a flip event. Observe them first. */
		if (pollers[1].revents) {
			int status = 0;
			if (pollers[1].revents & POLLNVAL) {
				errno = EBADF;
				return -1;
			}
			ret = cp_fence_status(fence, &status);
			if (ret < 0) {
				int error = errno;
				fprintf(stderr,
					"FENCE_ERROR token=%llu status=%d errno=%d event_observed=%u\n",
					(unsigned long long)token, status,
					error, event_done);
				errno = error;
				return -1;
			}
			if (ret == 1)
				fence_done = true;
			else if (!(pollers[1].revents & POLLIN)) {
				errno = EIO;
				return -1;
			}
		}
		if (pollers[0].revents) {
			union {
				uint64_t align;
				char data[4096];
			} events;
			size_t offset = 0;
			ssize_t length;
			if (!(pollers[0].revents & POLLIN)) {
				errno = EIO;
				return -1;
			}
			length = read(fd, events.data, sizeof(events.data));
			if (length < 0 && errno == EINTR)
				continue;
			if (length <= 0) {
				if (!length)
					errno = ENODATA;
				return -1;
			}
			while (offset + sizeof(struct drm_event) <=
			       (size_t)length) {
				struct drm_event *event =
					(void *)(events.data + offset);
				if (event->length < sizeof(*event) ||
				    event->length > (size_t)length - offset) {
					fprintf(stderr,
						"EVENT_PROTOCOL token=%llu stage=header offset=%zu type=%u observed_event_length=%u expected_min=%zu expected_max=%zu read_length=%zd\n",
						(unsigned long long)token,
						offset, event->type,
						event->length, sizeof(*event),
						(size_t)length - offset,
						length);
					errno = EPROTO;
					return -1;
				}
				if (event->type == DRM_EVENT_FLIP_COMPLETE &&
				    event->length ==
					    sizeof(struct drm_event_vblank)) {
					struct drm_event_vblank *flip =
						(void *)event;
					if (flip->user_data == token) {
						*sequence = flip->sequence;
						event_done = true;
					}
				}
				offset += event->length;
			}
			if (offset != (size_t)length) {
				fprintf(stderr,
					"EVENT_PROTOCOL token=%llu stage=trailing-bytes parsed_bytes=%zu observed_read_length=%zd expected_trailing_bytes=0 observed_trailing_bytes=%zu\n",
					(unsigned long long)token, offset,
					length, (size_t)length - offset);
				errno = EPROTO;
				return -1;
			}
		}
	}
	return 0;
}

static int cp_verify_scene(struct cp_context *c, const struct cp_scene *s)
{
	for (unsigned int i = 0; i < CP_INPUTS; i++) {
		struct cp_object_state state;
		const struct cp_layer *p = &s->layer[i];
		const struct cp_prop *prop;
		const char *const names[] = { "FB_ID",	"CRTC_ID", "SRC_W",
					      "SRC_H",	"CRTC_X",  "CRTC_Y",
					      "CRTC_W", "CRTC_H",  "zpos",
					      "alpha",	"rotation" };
		uint64_t values[] = { p->fb,
				      p->fb ? c->ids[CP_CRTC] : 0,
				      (uint64_t)p->w << 16,
				      (uint64_t)p->h << 16,
				      (uint64_t)(int64_t)p->x,
				      (uint64_t)(int64_t)p->y,
				      p->dw,
				      p->dh,
				      p->z,
				      p->alpha,
				      p->rotation };
		if (cp_read_object(c->fd, c->ids[CP_OSD1 + i],
				   DRM_MODE_OBJECT_PLANE, &state))
			return -1;
		for (unsigned int j = 0; j < ARRAY_SIZE(names); j++) {
			if (!p->fb && j > 1)
				continue;
			prop = cp_find(&state, names[j]);
			if (!prop || prop->value != values[j]) {
				errno = EPROTO;
				return -1;
			}
		}
	}
	struct drm_mode_get_plane cursor = { .plane_id = c->ids[CP_CURSOR] };
	if (call(c->fd, DRM_IOCTL_MODE_GETPLANE, &cursor))
		return -1;
	if (cursor.fb_id || cursor.crtc_id) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static int cp_apply(struct cp_context *c, const struct cp_scene *s, bool mode,
		    bool allow_modeset)
{
	struct cp_req r;
	uint32_t sequence;
	int fence = -1;
	uint64_t start = cp_now();
	if (cp_build(c, s, mode, &fence, &r))
		return -1;
	if (cp_send(c, &r,
		    DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT |
			    (allow_modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0),
		    ++c->token))
		goto fail;
	c->changed = true;
	c->pending = true;
	if (fence < 0) {
		errno = EPROTO;
		goto fail;
	}
	if (cp_wait_completion(c->fd, fence, c->token, &sequence))
		goto fail;
	c->pending = false;
	if (c->commits && (int32_t)(sequence - c->sequence) <= 0) {
		errno = EPROTO;
		goto fail;
	}
	if (cp_verify_scene(c, s))
		goto fail;
	if (c->fence >= 0)
		close(c->fence);
	c->fence = fence;
	if (!c->commits)
		c->first = sequence;
	c->sequence = sequence;
	c->commits++;
	printf("PRESENT scene=%s token=%llu sequence=%u fence_ok=1 plane_state_ok=1 allow_modeset=%u elapsed_ms=%.3f\n",
	       s->name, (unsigned long long)c->token, sequence, allow_modeset,
	       (cp_now() - start) / 1000000.0);
	return 0;
fail: {
	int error = errno;
	if (fence >= 0)
		close(fence);
	errno = error;
}
	return -1;
}

static uint32_t cp_pattern(unsigned int x, unsigned int y, unsigned int w,
			   unsigned int h, unsigned int layer,
			   unsigned int variant)
{
	static const uint32_t colors[4][4] = {
		{ 0x204060, 0x507020, 0x602040, 0x203060 },
		{ 0xe04020, 0x20b040, 0x2040e0, 0xe0c020 },
		{ 0xc02020, 0x20c060, 0x2020c0, 0xc0b020 },
		{ 0xe08020, 0x20b0c0, 0xa020e0, 0x60c020 }
	};
	if (x < 6 || y < 6 || x + 6 >= w || y + 6 >= h)
		return 0xffffff;
	if (x < w / 8 && y < h / 4)
		return variant ? 0x00ffff : 0xffffff;
	if (x > w * 3 / 4 && y > h * 3 / 4)
		return variant ? 0xffff00 : 0x000000;
	if (y > h / 2 - 3 && y < h / 2 + 3)
		return 0;
	unsigned int q = (x >= w / 2) + 2 * (y >= h / 2);
	uint32_t value = colors[layer][q];
	if ((x / 16 + y / 16) % 2)
		value = ((value & 0xfefefe) >> 1);
	return variant ? value ^ 0x1c1c1c : value;
}

static int64_t cp_round(int64_t value, int64_t div)
{
	return value < 0 ? -((-value + div / 2) / div) :
			   (value + div / 2) / div;
}
static uint8_t cp_byte(int64_t value)
{
	return value < 0 ? 0 : value > 255 ? 255 : value;
}
static void cp_yuv(uint32_t rgb, uint8_t yuv[3])
{
	int64_t r = rgb >> 16, g = (rgb >> 8) & 255, b = rgb & 255;
	int64_t y = 2126 * r + 7152 * g + 722 * b;
	yuv[0] = cp_byte(16 + cp_round(219 * y, 255 * 10000));
	yuv[1] = cp_byte(128 +
			 cp_round(112 * (10000 * b - y), 255 * (10000 - 722)));
	yuv[2] = cp_byte(128 +
			 cp_round(112 * (10000 * r - y), 255 * (10000 - 2126)));
}

static int cp_make_image(struct cp_context *c, struct cp_image *image,
			 unsigned int layer, unsigned int variant)
{
	bool video = layer >= 2;
	unsigned int w = layer == 0 ? cp_width :
			 layer == 1 ? 256 * cp_factor :
				      512 * cp_factor;
	unsigned int h = layer == 0 ? cp_height :
			 layer == 1 ? 160 * cp_factor :
				      288 * cp_factor;
	image->objects = video ? 2 : 1;
	for (unsigned int i = 0; i < image->objects; i++) {
		struct drm_mode_map_dumb map = {};
		image->dumb[i] = (struct drm_mode_create_dumb){
			.width = w, .height = h >> i, .bpp = video ? 8 : 32
		};
		if (call(c->fd, DRM_IOCTL_MODE_CREATE_DUMB, &image->dumb[i]))
			return -1;
		if (image->dumb[i].pitch < w * (video ? 1 : 4) ||
		    image->dumb[i].size > SIZE_MAX ||
		    (uint64_t)image->dumb[i].pitch * (h >> i) >
			    image->dumb[i].size) {
			errno = EPROTO;
			return -1;
		}
		map.handle = image->dumb[i].handle;
		if (call(c->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
			return -1;
		image->map[i] = mmap(NULL, image->dumb[i].size,
				     PROT_READ | PROT_WRITE, MAP_SHARED, c->fd,
				     map.offset);
		if (image->map[i] == MAP_FAILED) {
			image->map[i] = NULL;
			return -1;
		}
		memset(image->map[i], 0, image->dumb[i].size);
	}
	image->fb = (struct drm_mode_fb_cmd2){
		.width = w,
		.height = h,
		.pixel_format = layer < 2  ? DRM_FORMAT_XRGB8888 :
				layer == 2 ? DRM_FORMAT_NV12 :
					     DRM_FORMAT_NV21,
		.flags = DRM_MODE_FB_MODIFIERS,
		.handles = { image->dumb[0].handle,
			     video ? image->dumb[1].handle : 0 },
		.pitches = { image->dumb[0].pitch,
			     video ? image->dumb[1].pitch : 0 },
		.modifier = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR }
	};
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++) {
			uint32_t rgb = cp_pattern(x, y, w, h, layer, variant);
			if (!video)
				((uint32_t *)((char *)image->map[0] +
					      y * image->dumb[0].pitch))[x] =
					0xff000000 | rgb;
			else {
				uint8_t yuv[3];
				cp_yuv(rgb, yuv);
				((unsigned char *)image
					 ->map[0])[y * image->dumb[0].pitch + x] =
					yuv[0];
				if (!(x & 1) && !(y & 1)) {
					unsigned char *uv =
						(unsigned char *)image->map[1] +
						y / 2 * image->dumb[1].pitch +
						x;
					uv[0] = yuv[layer == 3 ? 2 : 1];
					uv[1] = yuv[layer == 3 ? 1 : 2];
				}
			}
		}
	if (call(c->fd, DRM_IOCTL_MODE_ADDFB2, &image->fb))
		return -1;
	printf("BUFFER layer=%s variant=%u fb=%u fourcc=0x%08x size=%ux%u modifier=LINEAR pitches=%u/%u separate_objects=%u immutable=1\n",
	       cp_names[CP_OSD1 + layer], variant, image->fb.fb_id,
	       image->fb.pixel_format, w, h, image->fb.pitches[0],
	       image->fb.pitches[1], image->objects);
	return 0;
}

static void cp_baseline(struct cp_scene *s,
			struct cp_image images[CP_INPUTS][2])
{
	unsigned int f = cp_factor, w = 512 * f, h = 288 * f;
	*s = (struct cp_scene){ .name = "four-linear-baseline",
				.modeset = true,
				.layer = { { .w = cp_width,
					     .h = cp_height,
					     .dw = cp_width,
					     .dh = cp_height,
					     .z = 0 },
					   { .w = 256 * f,
					     .h = 160 * f,
					     .dw = 256 * f,
					     .dh = 160 * f,
					     .x = (cp_width / 3) & ~1U,
					     .y = (cp_height / 3) & ~1U,
					     .z = 3 },
					   { .w = w,
					     .h = h,
					     .dw = w,
					     .dh = h,
					     .x = 80 * f,
					     .y = 50 * f,
					     .z = 1 },
					   { .w = w,
					     .h = h,
					     .dw = w,
					     .dh = h,
					     .x = cp_width - w - 16 * f,
					     .y = cp_height - h - 16 * f,
					     .z = 2 } } };
	for (unsigned int i = 0; i < CP_INPUTS; i++) {
		s->layer[i].fb = images[i][0].fb.fb_id;
		s->layer[i].alpha = 65535;
		s->layer[i].rotation = DRM_MODE_ROTATE_0;
	}
}

static unsigned int cp_stages(struct cp_scene stages[CP_STAGES],
			      const struct cp_scene *base)
{
	static const unsigned int orders[6][3] = { { 1, 2, 3 }, { 1, 3, 2 },
						   { 2, 1, 3 }, { 2, 3, 1 },
						   { 3, 1, 2 }, { 3, 2, 1 } };
	static const char *const order_names[6] = {
		"z-order-OSD2-VD1-VD2", "z-order-OSD2-VD2-VD1",
		"z-order-VD1-OSD2-VD2", "z-order-VD1-VD2-OSD2",
		"z-order-VD2-OSD2-VD1", "z-order-VD2-VD1-OSD2"
	};
	static const char *const reflected[4][3] = {
		{ "OSD1-reflect-X", "OSD1-reflect-Y", "OSD1-reflect-XY" },
		{ "OSD2-reflect-X", "OSD2-reflect-Y", "OSD2-reflect-XY" },
		{ "VD1-reflect-X", "VD1-reflect-Y", "VD1-reflect-XY" },
		{ "VD2-reflect-X", "VD2-reflect-Y", "VD2-reflect-XY" }
	};
	static const uint64_t reflections[] = { DRM_MODE_REFLECT_X,
						DRM_MODE_REFLECT_Y,
						DRM_MODE_REFLECT_X |
							DRM_MODE_REFLECT_Y };
	static const char *const alpha_names[2][5] = {
		{ "VD1-alpha-0", "VD1-alpha-16384", "VD1-alpha-32768",
		  "VD1-alpha-49152", "VD1-alpha-65535" },
		{ "VD2-alpha-0", "VD2-alpha-16384", "VD2-alpha-32768",
		  "VD2-alpha-49152", "VD2-alpha-65535" }
	};
	static const uint64_t alphas[] = { 0, 16384, 32768, 49152, 65535 };
	static const struct {
		const char *name;
		unsigned int w, h;
	} scale[] = { { "VD2-enlarge", 768, 432 },
		      { "VD2-reduce-less-than2", 384, 216 },
		      { "VD2-preHV-2to1", 256, 144 },
		      { "VD2-preH-only", 256, 288 },
		      { "VD2-vertical-only", 512, 144 },
		      { "VD2-preHV-residual2", 128, 72 },
		      { "VD2-preH-mixed", 256, 192 },
		      { "VD2-asymmetric-no-pre", 320, 108 },
		      { "VD2-return-identity", 512, 288 } };
	unsigned int n = 0;
	stages[n++] = *base;
	for (unsigned int i = 0; i < ARRAY_SIZE(orders); i++) {
		stages[n] = *base;
		stages[n].name = order_names[i];
		for (unsigned int j = 0; j < 3; j++)
			stages[n].layer[orders[i][j]].z = j + 1;
		n++;
	}
	for (unsigned int i = 0; i < 2; i++)
		for (unsigned int j = 0; j < ARRAY_SIZE(alphas); j++) {
			stages[n] = *base;
			stages[n].name = alpha_names[i][j];
			stages[n++].layer[i + 2].alpha = alphas[j];
		}
	for (unsigned int i = 0; i < CP_INPUTS; i++)
		for (unsigned int j = 0; j < ARRAY_SIZE(reflections); j++) {
			stages[n] = *base;
			stages[n].name = reflected[i][j];
			stages[n++].layer[i].rotation |= reflections[j];
		}
	for (unsigned int i = 0; i < ARRAY_SIZE(scale); i++) {
		stages[n] = *base;
		stages[n].name = scale[i].name;
		stages[n].layer[3].dw = scale[i].w * cp_factor;
		stages[n].layer[3].dh = scale[i].h * cp_factor;
		stages[n].layer[3].x =
			(cp_width - stages[n].layer[3].dw - 16 * cp_factor) &
			~1U;
		stages[n].layer[3].y =
			(cp_height - stages[n].layer[3].dh - 16 * cp_factor) &
			~1U;
		n++;
	}
	stages[n] = *base;
	stages[n].name = "final-four-linear-baseline";
	n++;
	return n;
}

static int cp_write_source(const char *path, const char *source)
{
	char data[80];
	int fd, length, error = 0;
	ssize_t n;
	length = snprintf(data, sizeof(data), "%s\n", source);
	if (length <= 0 || length >= (int)sizeof(data)) {
		errno = EINVAL;
		return -1;
	}
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		n = write(fd, data, length);
	} while (n < 0 && errno == EINTR);
	if (n != length)
		error = n < 0 ? errno : EIO;
	if (close(fd) && !error)
		error = errno;
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}

static int cp_crc_init(struct cp_crc *crc, const char *directory)
{
	char data[512], *line, *save;
	int fd, error = 0;
	ssize_t n;
	memset(crc, 0, sizeof(*crc));
	if (snprintf(crc->control, sizeof(crc->control), "%s/control",
		     directory) >= (int)sizeof(crc->control) ||
	    snprintf(crc->data, sizeof(crc->data), "%s/data", directory) >=
		    (int)sizeof(crc->data)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = open(crc->control, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		n = read(fd, data, sizeof(data) - 1);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		error = errno;
	if (close(fd) && !error)
		error = errno;
	if (error) {
		errno = error;
		return -1;
	}
	data[n] = 0;
	for (line = strtok_r(data, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		size_t len = strlen(line);
		if (!len || line[len - 1] != '*')
			continue;
		line[--len] = 0;
		if ((strcmp(line, "auto") && strcmp(line, "vpp")) ||
		    len >= sizeof(crc->original)) {
			errno = EPROTO;
			return -1;
		}
		strcpy(crc->original, line);
	}
	if (!crc->original[0]) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static int cp_crc_parse(char *line, size_t length, uint32_t *value)
{
	char *save, *frame, *crc, *end;
	unsigned long n;
	/* drm_debugfs_crc.c returns its terminating NUL as part of LINE_LEN. */
	if (length && line[length - 1] == 0)
		length--;
	if (!length || length >= 128 || memchr(line, '\0', length)) {
		errno = EPROTO;
		return -1;
	}
	line[length] = 0;
	frame = strtok_r(line, " \t\n", &save);
	crc = strtok_r(NULL, " \t\n", &save);
	if (!frame || !crc || strtok_r(NULL, " \t\n", &save)) {
		errno = EPROTO;
		return -1;
	}
	if (strcmp(frame, "XXXXXXXXXX")) {
		errno = 0;
		n = strtoul(frame, &end, 16);
		if (errno || !*frame || *end || n > UINT32_MAX) {
			errno = EPROTO;
			return -1;
		}
	}
	errno = 0;
	n = strtoul(crc, &end, 16);
	if (errno || !*crc || *end || n > UINT32_MAX) {
		errno = EPROTO;
		return -1;
	}
	*value = n;
	return 0;
}

static int cp_crc_window(struct cp_crc *crc, const struct cp_scene *s,
			 unsigned int samples)
{
	uint64_t deadline = cp_now() + 3000000000ULL;
	uint32_t first = 0, previous = 0;
	unsigned int count = 0, transitions = 0;
	int fd = -1, error = 0;
	if (cp_write_source(crc->control, "vpp"))
		return -1;
	crc->changed = true;
	fd = open(crc->data, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	while (count < samples + 3) {
		char data[128];
		ssize_t n;
		uint32_t value;
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int result;
		if (cp_stop) {
			error = EINTR;
			break;
		}
		if (cp_now() >= deadline) {
			error = ETIMEDOUT;
			break;
		}
		result = poll(&p, 1, 100);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}
		if (!result)
			continue;
		if (!(p.revents & POLLIN)) {
			error = EIO;
			break;
		}
		n = read(fd, data, sizeof(data) - 1);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0) {
			error = n ? errno : ENODATA;
			break;
		}
		if (cp_crc_parse(data, n, &value)) {
			error = errno;
			break;
		}
		if (count >= 3) {
			if (count == 3)
				first = value;
			else
				transitions += value != previous;
			printf("CRC_SAMPLE scene=%s reader_index=%u value=0x%08x read_ns=%llu hardware_frame=unavailable\n",
			       s->name, count - 3, value,
			       (unsigned long long)cp_now());
			previous = value;
		}
		count++;
	}
	/* Stop capture before another modeset, long hold or restoration. */
	if (close(fd) && !error)
		error = errno;
	printf("CRC_WINDOW scene=%s samples=%u first=0x%08x transitions=%u stable=%u capture_closed=1 pixel_correctness=unclassified\n",
	       s->name, count > 3 ? count - 3 : 0, first, transitions,
	       count == samples + 3 && !transitions);
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}

static int cp_hold(const char *scene)
{
	sigset_t block, previous;
	/* Clear release under a blocked SIGUSR1 so only the next release ends this hold. */
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	if (sigprocmask(SIG_BLOCK, &block, &previous))
		return -1;
	cp_release = 0;
	printf("HOLD_RELEASE scene=%s pid=%ld signal=SIGUSR1 state=waiting no_timeout=1\n",
	       scene, (long)getpid());
	while (!cp_stop && !cp_release)
		sigsuspend(&previous);
	if (sigprocmask(SIG_SETMASK, &previous, NULL))
		return -1;
	if (cp_stop) {
		errno = EINTR;
		return -1;
	}
	puts("HOLD_RELEASE state=released");
	return 0;
}

static void cp_restore_mode_diagnostics(const struct cp_state *expected,
					const struct cp_state *observed)
{
	const struct cp_state *states[] = { expected, observed };
	const char *const sides[] = { "expected", "observed" };
	for (unsigned int i = 0; i < ARRAY_SIZE(states); i++) {
		const struct drm_mode_modeinfo *m = &states[i]->crtc.mode;
		fprintf(stderr,
			"RESTORE_MODE side=%s valid=%u name=\"%.32s\" clock=%u hdisplay=%u hsync_start=%u hsync_end=%u htotal=%u hskew=%u vdisplay=%u vsync_start=%u vsync_end=%u vtotal=%u vscan=%u vrefresh=%u flags=0x%x type=0x%x\n",
			sides[i], states[i]->crtc.mode_valid, m->name, m->clock,
			m->hdisplay, m->hsync_start, m->hsync_end, m->htotal,
			m->hskew, m->vdisplay, m->vsync_start, m->vsync_end,
			m->vtotal, m->vscan, m->vrefresh, m->flags, m->type);
	}
	const unsigned char *a = (const void *)&expected->crtc.mode,
			    *b = (const void *)&observed->crtc.mode;
	for (size_t i = 0; i < sizeof(expected->crtc.mode); i++)
		if (a[i] != b[i])
			fprintf(stderr,
				"RESTORE_MODE_BYTE offset=%zu expected=0x%02x observed=0x%02x\n",
				i, a[i], b[i]);
}

static int cp_restore(struct cp_context *c)
{
	struct cp_req r = {};
	struct cp_state after;
	uint32_t sequence;
	const char *phase = "build";
	bool mismatch = false;
	int validation_error = 0;
	const struct cp_prop *active =
		cp_find(&c->saved.objects[CP_CRTC], "ACTIVE");
	const struct cp_prop *conn =
		cp_find(&c->saved.objects[CP_CONN], "CRTC_ID");
	const char *const extras[] = { "zpos",		 "alpha",
				       "rotation",	 "pixel blend mode",
				       "COLOR_ENCODING", "COLOR_RANGE" };
	int fence = -1, result = -1;
	if (!active || !conn)
		return -1;
	if (cp_obj(c, &r, CP_CONN) < 0 ||
	    cp_add(c, &r, CP_CONN, "CRTC_ID", conn->value))
		return -1;
	/* Native fault reporting may have changed this writable connector property. */
	const struct cp_prop *link =
		cp_find(&c->saved.objects[CP_CONN], "link-status");
	if (link && !(link->flags & DRM_MODE_PROP_IMMUTABLE) &&
	    cp_add(c, &r, CP_CONN, "link-status", link->value))
		return -1;
	/* Local HDMI harness also restores the connector color intent atomically. */
	const struct cp_prop *colorspace =
		cp_find(&c->saved.objects[CP_CONN], "Colorspace");
	if (colorspace &&
	    cp_add(c, &r, CP_CONN, "Colorspace", colorspace->value))
		return -1;
	const struct cp_prop *max_bpc =
		cp_find(&c->saved.objects[CP_CONN], "max bpc");
	if (max_bpc && cp_add(c, &r, CP_CONN, "max bpc", max_bpc->value))
		return -1;
	/* Replay the captured connector scalar with the saved link state. */
	const struct cp_prop *content =
		cp_find(&c->saved.objects[CP_CONN], "content type");
	if (content && cp_add(c, &r, CP_CONN, "content type", content->value))
		return -1;
	if (cp_obj(c, &r, CP_CRTC) < 0 ||
	    cp_add(c, &r, CP_CRTC, "ACTIVE", active->value) ||
	    cp_add(c, &r, CP_CRTC, "MODE_ID",
		   c->saved.crtc.mode_valid ? c->restore_blob : 0) ||
	    (active->value &&
	     cp_add(c, &r, CP_CRTC, "OUT_FENCE_PTR", PTR(&fence))))
		return -1;
	/* This focused harness requires original CTM=NULL before any display commit. */
	const struct cp_prop *ctm = cp_find(&c->saved.objects[CP_CRTC], "CTM");
	if (ctm && cp_add(c, &r, CP_CRTC, "CTM", ctm->value))
		return -1;
	const struct cp_prop *gamma =
		cp_find(&c->saved.objects[CP_CRTC], "GAMMA_LUT");
	if (gamma && cp_add(c, &r, CP_CRTC, "GAMMA_LUT", gamma->value))
		return -1;
	for (unsigned int i = CP_OSD1; i < CP_OBJECTS; i++) {
		if (cp_obj(c, &r, i) < 0)
			return -1;
		for (unsigned int j = 0; j < ARRAY_SIZE(cp_geometry); j++) {
			const struct cp_prop *p =
				cp_find(&c->saved.objects[i], cp_geometry[j]);
			if (!p || cp_add(c, &r, i, cp_geometry[j], p->value))
				return -1;
		}
		for (unsigned int j = 0; j < ARRAY_SIZE(extras); j++) {
			const struct cp_prop *p =
				cp_find(&c->saved.objects[i], extras[j]);
			if (p && !(p->flags & DRM_MODE_PROP_IMMUTABLE) &&
			    cp_add(c, &r, i, extras[j], p->value))
				return -1;
		}
		if (cp_find(&c->saved.objects[i], "IN_FENCE_FD") &&
		    cp_add(c, &r, i, "IN_FENCE_FD", (uint64_t)(int64_t)-1))
			return -1;
	}
	phase = "test-only";
	if (cp_send(c, &r,
		    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
		    0))
		goto out;
	if (fence != -1) {
		fprintf(stderr,
			"RESTORE_PROTOCOL phase=test-only-out-fence expected_fd=-1 observed_fd=%d\n",
			fence);
		errno = EPROTO;
		goto out;
	}
	phase = "submit";
	if (cp_send(c, &r,
		    DRM_MODE_ATOMIC_ALLOW_MODESET |
			    (active->value ? DRM_MODE_PAGE_FLIP_EVENT : 0),
		    ++c->token))
		goto out;
	phase = "completion";
	if (active->value &&
	    (fence < 0 ||
	     cp_wait_completion(c->fd, fence, c->token, &sequence))) {
		int error = errno;
		fprintf(stderr,
			"RESTORE_COMPLETION token=%llu expected_fence_fd=>=0 observed_fence_fd=%d expected_active=%llu errno=%d\n",
			(unsigned long long)c->token, fence,
			(unsigned long long)active->value, error);
		errno = error;
		goto out;
	}
	c->pending = false;
	phase = "snapshot-after-completion";
	if (cp_snapshot(c, &after))
		goto out;
	phase = "compare-properties";
	for (unsigned int i = 0; i < CP_OBJECTS; i++)
		for (unsigned int j = 0; j < c->saved.objects[i].count; j++) {
			const struct cp_prop *p = &c->saved.objects[i].props[j],
					     *actual;
			if (!strcmp(p->name, "MODE_ID") ||
			    !strcmp(p->name, "OUT_FENCE_PTR") ||
			    !strcmp(p->name, "IN_FENCE_FD"))
				continue;
			actual = cp_find(&after.objects[i], p->name);
			if (i == CP_CONN && !strcmp(p->name, "EDID") &&
			    actual && actual->id == p->id &&
			    actual->flags == p->flags) {
				if (cp_compare_edid(c, actual)) {
					if (!validation_error)
						validation_error = errno;
					mismatch = true;
				}
				continue;
			}
			if (!actual || actual->id != p->id ||
			    actual->flags != p->flags ||
			    actual->value != p->value) {
				fprintf(stderr,
					"RESTORE_PROPERTY_MISMATCH object=%s object_id=%u name=\"%s\" expected_prop_id=%u observed_present=%u observed_prop_id=%u expected_flags=0x%x observed_flags=0x%x expected_value=%llu observed_value=%llu expected_hex=0x%llx observed_hex=0x%llx raw_blob_id_comparison=%u immutable=%u\n",
					cp_names[i], c->ids[i], p->name, p->id,
					!!actual, actual ? actual->id : 0,
					p->flags, actual ? actual->flags : 0,
					(unsigned long long)p->value,
					(unsigned long long)(actual ?
								     actual->value :
								     0),
					(unsigned long long)p->value,
					(unsigned long long)(actual ?
								     actual->value :
								     0),
					!!(p->flags & DRM_MODE_PROP_BLOB),
					!!(p->flags & DRM_MODE_PROP_IMMUTABLE));
				mismatch = true;
			}
		}
	phase = "compare-mode-and-properties";
	if (after.crtc.mode_valid != c->saved.crtc.mode_valid ||
	    (after.crtc.mode_valid &&
	     memcmp(&after.crtc.mode, &c->saved.crtc.mode,
		    sizeof(after.crtc.mode)))) {
		cp_restore_mode_diagnostics(&c->saved, &after);
		mismatch = true;
	}
	if (mismatch) {
		errno = validation_error ? validation_error : EPROTO;
		goto out;
	}
	result = 0;
out: {
	int error = result ? errno : 0;
	if (fence >= 0 && close(fence) && !error) {
		error = errno;
		result = -1;
	}
	if (error) {
		fprintf(stderr,
			"RESTORE_STAGE phase=%s token=%llu errno=%d pending_commit=%u fence_fd=%d\n",
			phase, (unsigned long long)c->token, error, c->pending,
			fence);
		errno = error;
	}
}
	return result;
}

static int cp_disable_for_recovery(struct cp_context *c)
{
	struct cp_req r = {};
	struct cp_state before, after;
	uint32_t sequence;
	int fence = -1, result = -1;
	/* Detach every plane and connector as well as ACTIVE=0/MODE_ID=0. Keep
	 * all original/test FB owners and maps alive throughout this transaction.
	 * Native atomic_disable signals its event/fence only after a confirmed
	 * stop; a stop error must retain the existing CLOSEFB preservation path.
	 */
	if (cp_obj(c, &r, CP_CONN) < 0 ||
	    cp_add(c, &r, CP_CONN, "CRTC_ID", 0) ||
	    cp_obj(c, &r, CP_CRTC) < 0 || cp_add(c, &r, CP_CRTC, "ACTIVE", 0) ||
	    cp_add(c, &r, CP_CRTC, "MODE_ID", 0) ||
	    cp_add(c, &r, CP_CRTC, "CTM", 0) ||
	    cp_add(c, &r, CP_CRTC, "GAMMA_LUT", 0) ||
	    cp_add(c, &r, CP_CRTC, "OUT_FENCE_PTR", PTR(&fence)))
		return -1;
	for (unsigned int i = CP_OSD1; i < CP_OBJECTS; i++) {
		if (cp_obj(c, &r, i) < 0 || cp_add(c, &r, i, "FB_ID", 0) ||
		    cp_add(c, &r, i, "CRTC_ID", 0))
			return -1;
		if (cp_find(&c->saved.objects[i], "IN_FENCE_FD") &&
		    cp_add(c, &r, i, "IN_FENCE_FD", (uint64_t)(int64_t)-1))
			return -1;
	}
	if (cp_snapshot(c, &before) ||
	    cp_send(c, &r,
		    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
		    0))
		goto out;
	if (fence != -1) {
		errno = EPROTO;
		goto out;
	}
	if (cp_snapshot(c, &after))
		goto out;
	if (memcmp(&before, &after, sizeof(before))) {
		errno = EPROTO;
		goto out;
	}
	printf("RECOVERY_DISABLE token=%llu phase=submit FB_owners_retained=1\n",
	       (unsigned long long)(c->token + 1));
	/* A blocking off commit lets core wait for pending commit dependencies. */
	if (cp_send(c, &r,
		    DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT,
		    ++c->token))
		goto out;
	c->pending = true;
	if (fence < 0) {
		errno = EPROTO;
		goto out;
	}
	if (cp_wait_completion(c->fd, fence, c->token, &sequence) ||
	    cp_snapshot(c, &after))
		goto out;
	const char *const crtc_props[] = { "ACTIVE", "MODE_ID", "CTM",
					   "GAMMA_LUT" };
	for (unsigned int i = 0; i < ARRAY_SIZE(crtc_props); i++) {
		const struct cp_prop *p =
			cp_find(&after.objects[CP_CRTC], crtc_props[i]);
		if (!p || p->value) {
			errno = EPROTO;
			goto out;
		}
	}
	const struct cp_prop *conn =
		cp_find(&after.objects[CP_CONN], "CRTC_ID");
	if (!conn || conn->value || after.crtc.mode_valid || after.crtc.fb_id) {
		errno = EPROTO;
		goto out;
	}
	for (unsigned int i = CP_OSD1; i < CP_OBJECTS; i++) {
		const struct cp_prop *fb = cp_find(&after.objects[i], "FB_ID"),
				     *crtc = cp_find(&after.objects[i],
						     "CRTC_ID");
		if (!fb || !crtc || fb->value || crtc->value) {
			errno = EPROTO;
			goto out;
		}
	}
	c->pending = false;
	printf("RECOVERY_DISABLE token=%llu phase=complete fence_ok=1 event_ok=1 all_planes_off=1 active=0 mode_id=0\n",
	       (unsigned long long)c->token);
	result = 0;
out: {
	int error = result ? errno : 0;
	if (fence >= 0 && close(fence) && !error) {
		error = errno;
		result = -1;
	}
	if (error)
		errno = error;
}
	return result;
}

static int cp_restore_with_recovery(struct cp_context *c,
				    bool *restore_had_error)
{
	int error;
	*restore_had_error = false;
	if (!cp_restore(c))
		return 0;
	error = errno;
	*restore_had_error = true;
	fprintf(stderr, "RESTORE_ERROR errno=%d error=%s\n", error,
		strerror(error));
	if (error != EIO) {
		errno = error;
		return -1;
	}
	puts("RECOVERY_BEGIN reason=restore-EIO attempts=1 original_test_failure_preserved=1");
	if (cp_disable_for_recovery(c)) {
		error = errno;
		fprintf(stderr,
			"RECOVERY_END stage=disable result=failed errno=%d CLOSEFB_preservation_required=1\n",
			error);
		errno = error;
		return -1;
	}
	if (cp_restore(c)) {
		error = errno;
		fprintf(stderr,
			"RECOVERY_END stage=restore result=failed errno=%d CLOSEFB_preservation_required=1\n",
			error);
		errno = error;
		return -1;
	}
	puts("RECOVERY_END stage=restore result=recovered original_test_failure_preserved=1");
	return 0;
}

static bool cp_own_fb(struct cp_image images[CP_INPUTS][2], uint32_t fb)
{
	if (!fb)
		return false;
	for (unsigned int i = 0; i < CP_INPUTS; i++)
		for (unsigned int j = 0; j < 2; j++)
			if (images[i][j].fb.fb_id == fb)
				return true;
	return false;
}

static int cp_all_retired(struct cp_context *c,
			  struct cp_image images[CP_INPUTS][2])
{
	for (unsigned int i = CP_OSD1; i < CP_OBJECTS; i++) {
		struct drm_mode_get_plane p = { .plane_id = c->ids[i] };
		if (call(c->fd, DRM_IOCTL_MODE_GETPLANE, &p))
			return -1;
		if (cp_own_fb(images, p.fb_id)) {
			errno = EBUSY;
			return -1;
		}
	}
	return 0;
}

static int cp_release_image(struct cp_context *c, struct cp_image *image,
			    bool preserve)
{
	int result = 0;
	/* Adapted from native-kms/c5-kms-video.c: failed cleanup uses CLOSEFB. */
	if (image->fb.fb_id) {
		if (preserve) {
			struct drm_mode_closefb close = {
				.fb_id = image->fb.fb_id
			};
			if (call(c->fd, DRM_IOCTL_MODE_CLOSEFB, &close)) {
				/* Keep owner FD and metadata until external recovery retires it. */
				return -1;
			}
		} else if (call(c->fd, DRM_IOCTL_MODE_RMFB, &image->fb.fb_id))
			result = -1;
	}
	for (unsigned int i = 0; i < image->objects; i++) {
		struct drm_gem_close close = { .handle =
						       image->dumb[i].handle };
		if (image->map[i] && munmap(image->map[i], image->dumb[i].size))
			result = -1;
		if (close.handle && call(c->fd, DRM_IOCTL_GEM_CLOSE, &close))
			result = -1;
	}
	memset(image, 0, sizeof(*image));
	return result;
}

static bool cp_images_owned(struct cp_image images[CP_INPUTS][2])
{
	for (unsigned int i = 0; i < CP_INPUTS; i++)
		for (unsigned int j = 0; j < 2; j++)
			if (images[i][j].fb.fb_id)
				return true;
	return false;
}

static void cp_recovery_hold(struct cp_context *c,
			     struct cp_image images[CP_INPUTS][2])
{
	/* Only reached after both restoration and CLOSEFB fail. Give desktop recovery
	 * ownership of DRM master while retaining the FB-owning file until safe.
	 * SIGINT/SIGTERM cannot turn this error path into an active RMFB on exit.
	 */
	if (c->master && !call(c->fd, DRM_IOCTL_DROP_MASTER, NULL))
		c->master = false;
	printf("CLEANUP_RECOVERY_HOLD pid=%ld reason=CLOSEFB-failed original_DRM_fd_retained=1 "
	       "action=restart-desktop-or-retire-planes-externally no_timeout=1\n",
	       (long)getpid());
	while (cp_images_owned(images)) {
		bool inactive = !cp_all_retired(c, images);
		for (unsigned int i = 0; i < CP_INPUTS; i++)
			for (unsigned int j = 0; j < 2; j++)
				if (images[i][j].fb.fb_id)
					cp_release_image(c, &images[i][j],
							 !inactive);
		if (cp_images_owned(images))
			poll(NULL, 0, 250);
	}
	puts("CLEANUP_RECOVERY_HOLD state=released retained_FB_owners_closed_safely=1");
}

static int cp_selftest(void)
{
	struct cp_scene base, stages[CP_STAGES];
	struct cp_image images[CP_INPUTS][2] = {};
	unsigned int count;
	for (unsigned int i = 0; i < CP_INPUTS; i++)
		for (unsigned int j = 0; j < 2; j++)
			images[i][j].fb.fb_id = 100 + i * 2 + j;
	cp_baseline(&base, images);
	count = cp_stages(stages, &base);
	if (count > CP_STAGES || count != 39) {
		errno = EPROTO;
		return -1;
	}
	for (unsigned int i = 0; i < count; i++) {
		bool z[5] = {};
		unsigned int bottom = 0;
		for (unsigned int j = 0; j < CP_INPUTS; j++) {
			const struct cp_layer *p = &stages[i].layer[j];
			if (!p->fb || !p->w || !p->h || !p->dw || !p->dh ||
			    ((p->w | p->h | p->dw | p->dh) & 1) || p->x < 0 ||
			    p->y < 0 || (unsigned int)p->x + p->dw > cp_width ||
			    (unsigned int)p->y + p->dh > cp_height ||
			    p->z > 4 || z[p->z] || p->alpha > 65535 ||
			    !(p->rotation & DRM_MODE_ROTATE_0) ||
			    (p->rotation &
			     ~(DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X |
			       DRM_MODE_REFLECT_Y))) {
				errno = EPROTO;
				return -1;
			}
			z[p->z] = true;
			if (p->z < stages[i].layer[bottom].z)
				bottom = j;
		}
		if (stages[i].layer[bottom].alpha != 65535) {
			errno = EPROTO;
			return -1;
		}
	}
	uint8_t yuv[3];
	cp_yuv(0, yuv);
	if (yuv[0] != 16 || yuv[1] != 128 || yuv[2] != 128) {
		errno = EPROTO;
		return -1;
	}
	cp_yuv(0xffffff, yuv);
	if (yuv[0] != 235 || yuv[1] != 128 || yuv[2] != 128) {
		errno = EPROTO;
		return -1;
	}
	for (unsigned int i = 0; i < 4; i++) {
		uint32_t a = cp_pattern(128, 64, 1024, 576, i, 0),
			 b = cp_pattern(895, 64, 1024, 576, i, 0),
			 c = cp_pattern(128, 511, 1024, 576, i, 0);
		if (a == b || a == c ||
		    a == cp_pattern(128, 64, 1024, 576, i, 1)) {
			errno = EPROTO;
			return -1;
		}
	}
	char valid[] = "XXXXXXXXXX 0x12345678\n",
	     with_nul[] = "XXXXXXXXXX 0x12345678\n",
	     invalid[] = "XXXXXXXXXX 1 2\n";
	uint32_t value;
	if (cp_crc_parse(valid, strlen(valid), &value) || value != 0x12345678 ||
	    cp_crc_parse(with_nul, sizeof(with_nul), &value) ||
	    value != 0x12345678 ||
	    !cp_crc_parse(invalid, strlen(invalid), &value)) {
		errno = EPROTO;
		return -1;
	}
	printf("PASS HOST: %u finite scenes; bounds, four-input opaque bottom, unique zpos, immutable asymmetric patterns, BT709-limited endpoints and CRC parser. No board accessed.\n",
	       count);
	return 0;
}

#ifndef CP_NO_MAIN
int main(int argc, char **argv)
{
	struct cp_context c = { .fd = -1, .fence = -1, .token = 20000 };
	struct cp_image images[CP_INPUTS][2] = {};
	struct cp_scene base, stages[CP_STAGES], current;
	struct cp_crc crc = {};
	struct drm_mode_modeinfo mode = { .clock = 148500,
					  .hdisplay = 1920,
					  .hsync_start = 2008,
					  .hsync_end = 2052,
					  .htotal = 2200,
					  .vdisplay = 1080,
					  .vsync_start = 1084,
					  .vsync_end = 1089,
					  .vtotal = 1125,
					  .flags = DRM_MODE_FLAG_PHSYNC |
						   DRM_MODE_FLAG_PVSYNC,
					  .type = DRM_MODE_TYPE_USERDEF,
					  .name = "1920x1080" };
	struct drm_mode_create_blob blob = { .data = PTR(&mode),
					     .length = sizeof(mode) };
	struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_ATOMIC,
					  .value = 1 };
	struct drm_version version = {};
	char driver[64] = {};
	const char *crc_dir = NULL, *hold_scene = NULL, *only_scene = NULL,
		   *phase = "arguments";
	unsigned int flips = CP_DEFAULT_FLIPS, vd1 = 0, vd2 = 0, samples = 8,
		     completed = 0, count = 0;
	bool restored = false, retired = true, test_only = false, run = false;
	int result = 1, saved_errno = 0;
	setbuf(stdout, NULL);
	if (argc == 2 && !strcmp(argv[1], "--selftest")) {
		if (cp_selftest()) {
			perror("selftest");
			return 1;
		}
		return 0;
	}
	if (argc < 3)
		goto usage;
	for (int i = 2; i < argc; i++) {
		const char *option = argv[i], *value;
		if (!strcmp(option, "--run")) {
			run = true;
			continue;
		}
		if (!strcmp(option, "--test-only")) {
			test_only = true;
			continue;
		}
		if (i + 1 >= argc)
			goto usage;
		value = argv[++i];
		if (!strcmp(option, "--flips")) {
			if (cp_number(value, 2, CP_MAX_UPDATES, &flips))
				goto usage;
		} else if (!strcmp(option, "--plane-vd1")) {
			if (cp_number(value, 1, UINT_MAX, &vd1))
				goto usage;
		} else if (!strcmp(option, "--plane-vd2")) {
			if (cp_number(value, 1, UINT_MAX, &vd2))
				goto usage;
		} else if (!strcmp(option, "--crc"))
			crc_dir = value;
		else if (!strcmp(option, "--crc-samples")) {
			if (cp_number(value, 4, 32, &samples))
				goto usage;
		} else if (!strcmp(option, "--hold-scene"))
			hold_scene = value;
		else if (!strcmp(option, "--scene"))
			only_scene = value;
		else
			goto usage;
	}
	if (run == test_only || (!!vd1 != !!vd2) ||
	    (test_only && (crc_dir || hold_scene)))
		goto usage;
	/* Validate scene labels before opening any DRM node. */
	cp_baseline(&base, images);
	count = cp_stages(stages, &base);
	bool found_only = !only_scene, found_hold = !hold_scene;
	for (unsigned int i = 0; i < count; i++) {
		found_only |= only_scene && !strcmp(only_scene, stages[i].name);
		found_hold |= hold_scene && !strcmp(hold_scene, stages[i].name);
	}
	if (!found_only || !found_hold ||
	    (only_scene && hold_scene && strcmp(only_scene, hold_scene)))
		goto usage;
	struct sigaction action = { .sa_handler = cp_signal };
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL) ||
	    sigaction(SIGUSR1, &action, NULL)) {
		perror("signals");
		return 1;
	}
	phase = "open native DRM";
	c.fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (c.fd < 0)
		goto fail;
	version.name = driver;
	version.name_len = sizeof(driver) - 1;
	if (call(c.fd, DRM_IOCTL_VERSION, &version) ||
	    version.name_len >= sizeof(driver) || strcmp(driver, "s7d-drm")) {
		fprintf(stderr,
			"Refusing driver '%s': native s7d-drm required; no modeset issued\n",
			driver);
		result = 2;
		goto cleanup;
	}
	phase = "master/discovery";
	if (call(c.fd, DRM_IOCTL_SET_MASTER, NULL))
		goto fail;
	c.master = true;
	if (call(c.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) ||
	    cp_discover(&c, vd1, vd2) || check_sink(c.fd, c.ids[CP_CONN]))
		goto fail;
	cp_print_snapshot(&c.saved);
	/* Baseline owners must keep their saved framebuffer IDs alive until restoration. */
	phase = "plane properties";
	for (unsigned int i = 0; i < 2; i++)
		if (cp_enum(&c, CP_OSD1 + i, "pixel blend mode", "None",
			    &c.none[i]) ||
		    cp_enum(&c, CP_VD1 + i, "COLOR_ENCODING",
			    "ITU-R BT.709 YCbCr", &c.encoding[i]) ||
		    cp_enum(&c, CP_VD1 + i, "COLOR_RANGE",
			    "YCbCr limited range", &c.range[i]))
			goto fail;
	phase = "select current or preferred mode";
	if (c.saved.crtc.mode_valid)
		mode = c.saved.crtc.mode;
	else {
		struct drm_mode_modeinfo modes[64];
		struct drm_mode_get_connector connector = {
			.connector_id = c.ids[CP_CONN],
			.count_modes = ARRAY_SIZE(modes),
			.modes_ptr = PTR(modes)
		};
		if (call(c.fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector))
			goto fail;
		if (!connector.count_modes ||
		    connector.count_modes > ARRAY_SIZE(modes)) {
			errno = EOVERFLOW;
			goto fail;
		}
		mode = modes[0];
		for (unsigned int i = 0; i < connector.count_modes; i++)
			if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
				mode = modes[i];
				break;
			}
	}
	cp_width = mode.hdisplay;
	cp_height = mode.vdisplay;
	if (cp_width < 1024 || cp_height < 600 || cp_width > 1920 ||
	    cp_height > 1080 || ((cp_width | cp_height) & 1)) {
		errno = ENOTSUP;
		goto fail;
	}
	cp_factor = cp_width >= 1920 && cp_height >= 1080 ? 2 : 1;
	printf("OUTPUT mode=%s size=%ux%u clock_khz=%u source=%s source_factor=%u\n",
	       mode.name, cp_width, cp_height, mode.clock,
	       c.saved.crtc.mode_valid ? "saved-current-mode" :
					 "connector-preferred-mode",
	       cp_factor);
	phase = "restore mode copy";
	if (c.saved.crtc.mode_valid) {
		struct drm_mode_create_blob old = {
			.data = PTR(&c.saved.crtc.mode),
			.length = sizeof(c.saved.crtc.mode)
		};
		if (call(c.fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &old))
			goto fail;
		c.restore_blob = old.blob_id;
	}
	if (crc_dir) {
		phase = "CRC source snapshot";
		if (cp_crc_init(&crc, crc_dir))
			goto fail;
	}
	phase = "prepare immutable buffers";
	if (call(c.fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob))
		goto fail;
	c.mode_blob = blob.blob_id;
	for (unsigned int i = 0; i < CP_INPUTS; i++)
		for (unsigned int j = 0; j < 2; j++)
			if (cp_make_image(&c, &images[i][j], i, j))
				goto fail;
	cp_baseline(&base, images);
	count = cp_stages(stages, &base);
	current = base;
	/* Automatic tests never claim monitor presentation or optical correctness. */
	printf("MATRIX stages=%u flips_per_stage=%u output=%ux%u clock_khz=%u physical_inputs=OSD1+OSD2+VD1+VD2 test_only=%u\n",
	       count, flips, cp_width, cp_height, mode.clock, test_only);
	for (unsigned int i = 0; i < count; i++) {
		if (only_scene && strcmp(only_scene, stages[i].name))
			continue;
		phase = stages[i].name;
		current = stages[i];
		if (cp_stop) {
			errno = EINTR;
			goto fail;
		}
		printf("STAGE_BEGIN index=%u scene=%s VD2_src=%ux%u VD2_dst=%ux%u bottom=OSD1-opaque\n",
		       i, current.name, current.layer[3].w, current.layer[3].h,
		       current.layer[3].dw, current.layer[3].dh);
		if (cp_test(&c, &current, !c.changed, 0))
			goto fail;
		if (!test_only) {
			for (unsigned int j = 0; j < flips; j++) {
				for (unsigned int k = 0; k < CP_INPUTS; k++)
					current.layer[k].fb =
						images[k][j & 1].fb.fb_id;
				/* First commit per scene admits stopped reconfiguration; later flips must stay live. */
				if (cp_apply(&c, &current, !c.changed, j == 0))
					goto fail;
				if (cp_stop) {
					errno = EINTR;
					goto fail;
				}
			}
			if (crc_dir && cp_crc_window(&crc, &current, samples))
				goto fail;
			if (hold_scene && !strcmp(hold_scene, current.name) &&
			    cp_hold(current.name))
				goto fail;
		}
		completed++;
		printf("STAGE_END scene=%s api=pass visual=unclassified\n",
		       current.name);
	}
	phase = "negative TEST_ONLY";
	current = base;
	current.name = "reject-translucent-four-input-bottom";
	current.layer[0].alpha = 32768;
	if (cp_test(&c, &current, !c.changed, EINVAL))
		goto fail;
	current = base;
	current.name = "reject-odd-VD2-output";
	current.layer[3].dw = 511;
	if (cp_test(&c, &current, !c.changed, EINVAL))
		goto fail;
	printf("PASS API: scenes=%u TEST_ONLY=%u completed_commits=%u events=%u fences=%u vblank=%u..%u hardware_pixels=unclassified\n",
	       completed, c.tests, c.commits, c.commits, c.commits, c.first,
	       c.sequence);
	result = 0;
	goto cleanup;
fail:
	saved_errno = errno;
	fprintf(stderr,
		"FAIL phase=%s token=%llu completed_scenes=%u commits=%u errno=%d error=%s\n",
		phase, (unsigned long long)c.token, completed, c.commits,
		saved_errno, strerror(saved_errno));
cleanup:
	if (crc.changed && cp_write_source(crc.control, crc.original)) {
		perror("restore CRC source");
		result = 1;
	}
	if (c.changed) {
		retired = false;
		bool restore_had_error;
		if (cp_restore_with_recovery(&c, &restore_had_error)) {
			perror("atomic restore snapshot/recovery");
			result = 1;
		} else
			restored = true;
		/* Recovery is cleanup, never success for an originally failed test. */
		if (restore_had_error)
			result = 1;
		if (restored && !cp_all_retired(&c, images))
			retired = true;
		else {
			if (restored)
				perror("verify test framebuffers retired");
			result = 1;
		}
	}
	if (c.fd >= 0) {
		for (unsigned int i = 0; i < CP_INPUTS; i++)
			for (unsigned int j = 0; j < 2; j++)
				if (cp_release_image(&c, &images[i][j],
						     !retired)) {
					perror("release framebuffer safely");
					result = 1;
				}
		if (cp_images_owned(images))
			cp_recovery_hold(&c, images);
		uint32_t blobs[] = { c.mode_blob, c.restore_blob };
		for (unsigned int i = 0; i < ARRAY_SIZE(blobs); i++)
			if (blobs[i]) {
				struct drm_mode_destroy_blob b = {
					.blob_id = blobs[i]
				};
				if (call(c.fd, DRM_IOCTL_MODE_DESTROYPROPBLOB,
					 &b)) {
					perror("destroy mode blob");
					result = 1;
				}
			}
		if (c.fence >= 0)
			close(c.fence);
		if (c.master && call(c.fd, DRM_IOCTL_DROP_MASTER, NULL)) {
			perror("drop DRM master");
			result = 1;
		}
		close(c.fd);
	}
	printf("CLEANUP changed=%u snapshot_restored=%u test_buffers_retired=%u preserve_active_with_CLOSEFB=%u pending_commit=%u result=%d\n",
	       c.changed, restored, retired, !retired, c.pending, result);
	return result;
usage:
	fprintf(stderr,
		"Usage: %s /dev/dri/cardN --run|--test-only [--flips 2..512] [--plane-vd1 ID --plane-vd2 ID]\n"
		"       [--scene NAME] [--hold-scene NAME] [--crc /sys/kernel/debug/dri/N/crtc-0/crc] [--crc-samples 4..32]\n"
		"       %s --selftest\n",
		argv[0], argv[0]);
	return 2;
}

#endif /* CP_NO_MAIN */
