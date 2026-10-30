// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#define CP_NO_MAIN
#include "../common/c5-kms-compose.c"
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define WB_W 1920U
#define WB_H 720U
#define WB_OBJECTS 8
#define WB_UPDATES 192

struct wb_request {
	uint32_t objects[WB_OBJECTS], counts[WB_OBJECTS], props[WB_UPDATES];
	uint64_t values[WB_UPDATES];
	unsigned int count, used;
};
struct wb_buffer {
	struct drm_mode_create_dumb dumb;
	struct drm_mode_fb_cmd2 fb;
	unsigned char *map;
};
struct wb_context {
	struct cp_context c;
	uint32_t connector, original_ctm, original_gamma, source_format,
		target_format;
	struct cp_object_state wb_saved;
	struct wb_buffer source, target[2];
	struct drm_mode_modeinfo mode;
	uint64_t source_hash;
	const char *boot, *notes, *output;
	unsigned int cases, tests;
	bool changed, pending;
};

static void wb_signal(int signal)
{
	cp_stop = signal;
}
static void wb_retain(struct wb_context *w, const char *reason)
{
	fprintf(stderr,
		"RECOVERY_HOLD pid=%ld reason=%s pending_WB=%u resources_retained=1 root_recovery_required=1\n",
		(long)getpid(), reason, w->pending);
	fflush(NULL);
	for (;;)
		pause();
}
static uint64_t wb_hash(const void *data, size_t length)
{
	const unsigned char *p = data;
	uint64_t h = UINT64_C(14695981039346656037);
	for (size_t i = 0; i < length; i++)
		h = (h ^ p[i]) * UINT64_C(1099511628211);
	return h;
}
static int wb_read_text(const char *path, char *data, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;
	if (fd < 0)
		return -1;
	do
		n = read(fd, data, size - 1);
	while (n < 0 && errno == EINTR);
	int e = n < 0 ? errno : 0;
	if (close(fd) && !e)
		e = errno;
	if (e) {
		errno = e;
		return -1;
	}
	data[n] = 0;
	data[strcspn(data, "\r\n")] = 0;
	return 0;
}
static int wb_identity(struct wb_context *w)
{
	char boot[80], hash[256] = {};
	int fds[2], status;
	pid_t pid;
	ssize_t length = 0;
	if (wb_read_text("/proc/sys/kernel/random/boot_id", boot,
			 sizeof(boot)) ||
	    strcmp(boot, w->boot)) {
		errno = ESTALE;
		return -1;
	}
	if (pipe2(fds, O_CLOEXEC))
		return -1;
	pid = fork();
	if (pid < 0) {
		int e = errno;
		close(fds[0]);
		close(fds[1]);
		errno = e;
		return -1;
	}
	if (!pid) {
		dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		execl("/usr/bin/sha256sum", "sha256sum", "/sys/kernel/notes",
		      (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	while ((size_t)length < sizeof(hash) - 1) {
		ssize_t n =
			read(fds[0], hash + length, sizeof(hash) - 1 - length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			length = -1;
			break;
		}
		if (!n)
			break;
		length += n;
	}
	close(fds[0]);
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (length < 64 || !WIFEXITED(status) || WEXITSTATUS(status) ||
	    strncmp(hash, w->notes, 64) || hash[64] != ' ') {
		errno = ESTALE;
		return -1;
	}
	return 0;
}
static int wb_begin(struct wb_request *r, uint32_t object)
{
	if (r->count == WB_OBJECTS) {
		errno = EOVERFLOW;
		return -1;
	}
	r->objects[r->count] = object;
	r->counts[r->count++] = 0;
	return 0;
}
static int wb_add(struct wb_request *r, const struct cp_object_state *object,
		  const char *name, uint64_t value, bool optional)
{
	const struct cp_prop *p = cp_find(object, name);
	if (!p && optional)
		return 0;
	if (!p || p->flags & DRM_MODE_PROP_IMMUTABLE || !r->count ||
	    r->objects[r->count - 1] != object->id || r->used == WB_UPDATES) {
		errno = EPROTO;
		return -1;
	}
	r->props[r->used] = p->id;
	r->values[r->used++] = value;
	r->counts[r->count - 1]++;
	return 0;
}
static int wb_send(struct wb_context *w, struct wb_request *r, uint32_t flags,
		   uint64_t token)
{
	struct drm_mode_atomic a = { .flags = flags,
				     .count_objs = r->count,
				     .objs_ptr = PTR(r->objects),
				     .count_props_ptr = PTR(r->counts),
				     .props_ptr = PTR(r->props),
				     .prop_values_ptr = PTR(r->values),
				     .user_data = token };
	return call(w->c.fd, DRM_IOCTL_MODE_ATOMIC, &a);
}
static struct drm_mode_modeinfo wb_mode(void)
{
	return (struct drm_mode_modeinfo){ .clock = 111750,
					   .hdisplay = WB_W,
					   .hsync_start = 2016,
					   .hsync_end = 2208,
					   .htotal = 2496,
					   .vdisplay = WB_H,
					   .vsync_start = 723,
					   .vsync_end = 733,
					   .vtotal = 748,
					   .vrefresh = 60,
					   .flags = DRM_MODE_FLAG_PHSYNC |
						    DRM_MODE_FLAG_PVSYNC,
					   .type = DRM_MODE_TYPE_USERDEF,
					   .name = "1920x720" };
}
static bool wb_mode_equal(const struct drm_mode_modeinfo *a,
			  const struct drm_mode_modeinfo *b)
{
	return a->clock == b->clock && a->hdisplay == b->hdisplay &&
	       a->hsync_start == b->hsync_start &&
	       a->hsync_end == b->hsync_end && a->htotal == b->htotal &&
	       a->hskew == b->hskew && a->vdisplay == b->vdisplay &&
	       a->vsync_start == b->vsync_start &&
	       a->vsync_end == b->vsync_end && a->vtotal == b->vtotal &&
	       a->vscan == b->vscan && a->flags == b->flags;
}
static int wb_read_blob(int fd, uint32_t id, void *bytes, uint32_t capacity,
			uint32_t unit, uint32_t *length)
{
	struct drm_mode_get_blob b = { .blob_id = id };
	if (call(fd, DRM_IOCTL_MODE_GETPROPBLOB, &b))
		return -1;
	uint32_t expected = b.length;
	if (!expected || expected > capacity || expected % unit) {
		errno = EOVERFLOW;
		return -1;
	}
	b.data = PTR(bytes);
	if (call(fd, DRM_IOCTL_MODE_GETPROPBLOB, &b))
		return -1;
	if (b.length != expected) {
		errno = EAGAIN;
		return -1;
	}
	*length = expected;
	return 0;
}
static int wb_discover(struct wb_context *w, bool clone)
{
	struct cp_context *c = &w->c;
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
	if (res.count_crtcs != 1 || res.count_connectors != 2 ||
	    list.count_planes != 5) {
		errno = ENOTSUP;
		return -1;
	}
	c->ids[CP_CRTC] = crtcs[0];
	for (unsigned int i = 0; i < res.count_connectors; i++) {
		struct drm_mode_modeinfo modes[128];
		struct drm_mode_get_connector q = {
			.connector_id = connectors[i],
			.count_modes = ARRAY_SIZE(modes),
			.modes_ptr = PTR(modes)
		};
		if (call(c->fd, DRM_IOCTL_MODE_GETCONNECTOR, &q) ||
		    q.count_modes > ARRAY_SIZE(modes))
			return -1;
		if (q.connector_type == DRM_MODE_CONNECTOR_HDMIA) {
			if (c->ids[CP_CONN]) {
				errno = ENOTSUP;
				return -1;
			}
			c->ids[CP_CONN] = q.connector_id;
			if (clone) {
				bool found = false;
				for (unsigned int m = 0; m < q.count_modes; m++)
					found |= wb_mode_equal(&modes[m],
							       &w->mode);
				if (q.connection != 1 || !found) {
					errno = ENODEV;
					return -1;
				}
			}
		} else if (q.connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			if (w->connector) {
				errno = ENOTSUP;
				return -1;
			}
			w->connector = q.connector_id;
		} else {
			errno = ENOTSUP;
			return -1;
		}
	}
	if (!c->ids[CP_CONN] || !w->connector) {
		errno = ENODEV;
		return -1;
	}
	for (unsigned int i = 0; i < list.count_planes; i++) {
		uint32_t formats[64], id;
		uint64_t type;
		struct drm_mode_get_plane p = {
			.plane_id = planes[i],
			.count_format_types = ARRAY_SIZE(formats),
			.format_type_ptr = PTR(formats)
		};
		bool rgb = false, rgb10 = false, nv12 = false, nv21 = false;
		if (call(c->fd, DRM_IOCTL_MODE_GETPLANE, &p) ||
		    property(c->fd, p.plane_id, DRM_MODE_OBJECT_PLANE, "type",
			     &id, &type) ||
		    p.count_format_types > ARRAY_SIZE(formats) ||
		    p.possible_crtcs != 1)
			return -1;
		for (unsigned int f = 0; f < p.count_format_types; f++) {
			rgb |= formats[f] == DRM_FORMAT_XRGB8888;
			rgb10 |= formats[f] == DRM_FORMAT_XBGR2101010;
			nv12 |= formats[f] == DRM_FORMAT_NV12;
			nv21 |= formats[f] == DRM_FORMAT_NV21;
		}
		if (type == 1 &&
		    ((w->source_format == DRM_FORMAT_XRGB8888 && rgb) ||
		     (w->source_format == DRM_FORMAT_XBGR2101010 && rgb10)) &&
		    !nv12)
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
	c->ids[CP_VD1] = videos[0];
	c->ids[CP_VD2] = videos[1];
	if (cp_snapshot(c, &c->saved) ||
	    cp_read_object(c->fd, w->connector, DRM_MODE_OBJECT_CONNECTOR,
			   &w->wb_saved))
		return -1;
	const struct cp_prop *attached = cp_find(&w->wb_saved, "CRTC_ID"),
			     *job = cp_find(&w->wb_saved, "WRITEBACK_FB_ID"),
			     *edid = cp_find(&c->saved.objects[CP_CONN],
					     "EDID"),
			     *formats = cp_find(&w->wb_saved,
						"WRITEBACK_PIXEL_FORMATS");
	if (!attached || attached->value || !job || job->value || !formats ||
	    !formats->value || !edid) {
		errno = EBUSY;
		return -1;
	}
	uint32_t fmts[16], format_length = 0;
	if (wb_read_blob(c->fd, formats->value, fmts, sizeof(fmts),
			 sizeof(fmts[0]), &format_length))
		return -1;
	bool bgr = false;
	for (unsigned int i = 0; i < format_length / 4; i++)
		bgr |= fmts[i] == w->target_format;
	if (!bgr) {
		errno = ENOTSUP;
		return -1;
	}
	if (edid->value) {
		if (cp_capture_edid(c))
			return -1;
	} else if (clone) {
		errno = ENODEV;
		return -1;
	}
	for (unsigned int i = CP_OSD2; i < CP_OBJECTS; i++) {
		const struct cp_prop *fb =
			cp_find(&c->saved.objects[i], "FB_ID");
		if (!fb || fb->value) {
			errno = EBUSY;
			return -1;
		}
	}
	if (cp_enum(c, CP_OSD1, "pixel blend mode", "None", &c->none[0]))
		return -1;
	printf("DISCOVERY HDMI=%u WB=%u CRTC=%u primary=%u standard_writeback_cap=1 source_fourcc=0x%08x target_fourcc=0x%08x saved_EDID_bytes=%u initial_WB_detached=1\n",
	       c->ids[CP_CONN], w->connector, c->ids[CP_CRTC], c->ids[CP_OSD1],
	       w->source_format, w->target_format, c->saved_edid_length);
	return 0;
}
static int wb_blob(int fd, const void *bytes, size_t size, uint32_t *id)
{
	struct drm_mode_create_blob b = { .data = PTR(bytes), .length = size };
	if (call(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &b))
		return -1;
	*id = b.blob_id;
	return 0;
}
static int wb_clone_property(struct wb_context *w, const char *name,
			     uint32_t *owned)
{
	struct cp_object_state *s = &w->c.saved.objects[CP_CRTC];
	const struct cp_prop *p = cp_find(s, name);
	unsigned char bytes[1024];
	if (!p || p->value > UINT32_MAX) {
		errno = EPROTO;
		return -1;
	}
	if (!p->value)
		return 0;
	uint32_t length = 0, original = p->value;
	if (wb_read_blob(w->c.fd, original, bytes, sizeof(bytes), 1, &length) ||
	    wb_blob(w->c.fd, bytes, length, owned))
		return -1;
	for (unsigned int i = 0; i < s->count; i++)
		if (s->props[i].id == p->id)
			s->props[i].value = *owned;
	printf("ORIGINAL_BLOB name=%s original=%u owned_clone=%u bytes=%u\n",
	       name, original, *owned, length);
	return 0;
}
static unsigned int wb_cpp(uint32_t format)
{
	return format == DRM_FORMAT_BGR888 ?
		       3 :
		       (format == DRM_FORMAT_XRGB8888 ||
					format == DRM_FORMAT_XBGR2101010 ?
				4 :
				0);
}
static const char *wb_format_name(uint32_t format)
{
	return format == DRM_FORMAT_XRGB8888 ?
		       "XRGB8888" :
		       (format == DRM_FORMAT_BGR888 ?
				"BGR888" :
				(format == DRM_FORMAT_XBGR2101010 ?
					 "XBGR2101010" :
					 "UNKNOWN"));
}
static uint32_t wb_pattern(uint32_t format, unsigned int x, unsigned int y)
{
	unsigned int bits = format == DRM_FORMAT_XRGB8888 ? 8 : 10,
		     mask = (1U << bits) - 1, v = x & mask, r, g, b;
	if (y < 128) {
		r = v;
		g = 1U << (bits - 4);
		b = 1U << (bits - 1);
	} else if (y < 256) {
		r = 1U << (bits - 4);
		g = v;
		b = 1U << (bits - 1);
	} else if (y < 384) {
		r = 1U << (bits - 4);
		g = 1U << (bits - 1);
		b = v;
	} else if (y < 512) {
		r = g = b = v;
	} else if (y < 640) {
		r = (x + 3 * y) & mask;
		g = (3 * x + 5 * y) & mask;
		b = (7 * x + 11 * y) & mask;
	} else {
		r = x & 1 ? mask : 0;
		g = y & 1 ? mask : 0;
		b = x & 32 ? mask : 0;
	}
	return bits == 8 ? (r << 16) | (g << 8) | b : r | (g << 10) | (b << 20);
}
static int wb_layout(uint32_t format, uint32_t pitch, uint64_t size)
{
	unsigned int cpp = wb_cpp(format);
	if (!cpp || !size || size > SIZE_MAX || pitch < WB_W * cpp ||
	    (pitch & 63) || size < (uint64_t)pitch * (WB_H + 2)) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}
static void wb_fill_source(struct wb_buffer *b)
{
	for (unsigned int y = 0; y < WB_H; y++)
		for (unsigned int x = 0; x < WB_W; x++)
			((uint32_t *)(b->map + b->fb.offsets[0] +
				      (uint64_t)y * b->dumb.pitch))[x] =
				wb_pattern(b->fb.pixel_format, x, y);
}
static int wb_buffer_make(struct wb_context *w, struct wb_buffer *b,
			  uint32_t format, bool guard)
{
	unsigned int cpp = wb_cpp(format);
	if (!cpp || (!guard && cpp != 4)) {
		errno = EINVAL;
		return -1;
	}
	struct drm_mode_map_dumb map = {};
	b->dumb = (struct drm_mode_create_dumb){ .width = WB_W,
						 .height = WB_H + 2,
						 .bpp = cpp * 8 };
	if (call(w->c.fd, DRM_IOCTL_MODE_CREATE_DUMB, &b->dumb))
		return -1;
	if (wb_layout(format, b->dumb.pitch, b->dumb.size))
		return -1;
	map.handle = b->dumb.handle;
	if (call(w->c.fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		return -1;
	b->map = mmap(NULL, b->dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      w->c.fd, map.offset);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		return -1;
	}
	memset(b->map, 0xa5, b->dumb.size);
	b->fb = (struct drm_mode_fb_cmd2){ .width = WB_W,
					   .height = WB_H,
					   .pixel_format = format,
					   .flags = DRM_MODE_FB_MODIFIERS,
					   .handles = { b->dumb.handle },
					   .pitches = { b->dumb.pitch },
					   .offsets = { b->dumb.pitch },
					   .modifier = {
						   DRM_FORMAT_MOD_LINEAR } };
	if (call(w->c.fd, DRM_IOCTL_MODE_ADDFB2, &b->fb))
		return -1;
	if (!guard) {
		wb_fill_source(b);
		if (mprotect(b->map, b->dumb.size, PROT_READ))
			return -1;
	}
	return 0;
}
static int wb_build(struct wb_context *w, struct wb_request *r, bool headless,
		    uint32_t target, int *crtc_fence, int *wb_fence)
{
	struct cp_context *c = &w->c;
	memset(r, 0, sizeof(*r));
	const struct cp_object_state *h = &c->saved.objects[CP_CONN],
				     *crtc = &c->saved.objects[CP_CRTC];
	if (wb_begin(r, c->ids[CP_CONN]) ||
	    wb_add(r, h, "CRTC_ID", headless ? 0 : c->ids[CP_CRTC], false) ||
	    wb_add(r, h, "Colorspace", 0, true) ||
	    wb_add(r, h, "max bpc", 8, true) ||
	    wb_add(r, h, "Broadcast RGB", 0, true) ||
	    wb_add(r, h, "content type", DRM_MODE_CONTENT_TYPE_NO_DATA, true))
		return -1;
	if (wb_begin(r, w->connector) ||
	    wb_add(r, &w->wb_saved, "CRTC_ID", c->ids[CP_CRTC], false) ||
	    wb_add(r, &w->wb_saved, "WRITEBACK_FB_ID", target, false) ||
	    wb_add(r, &w->wb_saved, "WRITEBACK_OUT_FENCE_PTR", PTR(wb_fence),
		   false))
		return -1;
	if (wb_begin(r, c->ids[CP_CRTC]) ||
	    wb_add(r, crtc, "ACTIVE", 1, false) ||
	    wb_add(r, crtc, "MODE_ID", c->mode_blob, false) ||
	    wb_add(r, crtc, "OUT_FENCE_PTR", PTR(crtc_fence), false) ||
	    wb_add(r, crtc, "CTM", 0, false) ||
	    wb_add(r, crtc, "GAMMA_LUT", 0, false) ||
	    wb_add(r, crtc, "VRR_ENABLED", 0, true))
		return -1;
	for (unsigned int i = CP_OSD1; i < CP_OBJECTS; i++) {
		const struct cp_object_state *p = &c->saved.objects[i];
		bool primary = i == CP_OSD1;
		if (wb_begin(r, c->ids[i]) ||
		    wb_add(r, p, "FB_ID", primary ? w->source.fb.fb_id : 0,
			   false) ||
		    wb_add(r, p, "CRTC_ID", primary ? c->ids[CP_CRTC] : 0,
			   false))
			return -1;
		if (primary) {
			const uint64_t values[] = { 0,
						    0,
						    (uint64_t)WB_W << 16,
						    (uint64_t)WB_H << 16,
						    0,
						    0,
						    WB_W,
						    WB_H };
			for (unsigned int j = 2; j < ARRAY_SIZE(cp_geometry);
			     j++)
				if (wb_add(r, p, cp_geometry[j], values[j - 2],
					   false))
					return -1;
			if (wb_add(r, p, "zpos", 0, false) ||
			    wb_add(r, p, "alpha", 65535, false) ||
			    wb_add(r, p, "rotation", DRM_MODE_ROTATE_0,
				   false) ||
			    wb_add(r, p, "pixel blend mode", c->none[0],
				   false) ||
			    wb_add(r, p, "IN_FENCE_FD", (uint64_t)(int64_t)-1,
				   true))
				return -1;
		}
	}
	return 0;
}
static int wb_test(struct wb_context *w, bool headless, char effect,
		   uint32_t target, int expected)
{
	struct cp_state before, after;
	struct cp_object_state bbefore, bafter;
	struct wb_request r;
	int cf = -1, wf = -1;
	uint64_t hashes[2] = {
		wb_hash(w->target[0].map, w->target[0].dumb.size),
		wb_hash(w->target[1].map, w->target[1].dumb.size)
	};
	if (cp_snapshot(&w->c, &before) ||
	    cp_read_object(w->c.fd, w->connector, DRM_MODE_OBJECT_CONNECTOR,
			   &bbefore) ||
	    wb_build(w, &r, headless, target, &cf, &wf))
		return -1;
	int result = wb_send(w, &r,
			     DRM_MODE_ATOMIC_TEST_ONLY |
				     DRM_MODE_ATOMIC_ALLOW_MODESET,
			     0),
	    error = result ? errno : 0;
	if ((expected && (!result || error != expected)) ||
	    (!expected && result) || cf != -1 || wf != -1) {
		errno = EPROTO;
		return -1;
	}
	if (cp_snapshot(&w->c, &after) ||
	    cp_read_object(w->c.fd, w->connector, DRM_MODE_OBJECT_CONNECTOR,
			   &bafter) ||
	    memcmp(&before, &after, sizeof(before)) ||
	    memcmp(&bbefore, &bafter, sizeof(bbefore)) ||
	    hashes[0] != wb_hash(w->target[0].map, w->target[0].dumb.size) ||
	    hashes[1] != wb_hash(w->target[1].map, w->target[1].dumb.size) ||
	    w->source_hash != wb_hash(w->source.map, w->source.dumb.size)) {
		errno = EPROTO;
		return -1;
	}
	w->tests++;
	printf("TEST_ONLY route=%s effect=%c target=%u errno=%d expected=%d state_and_target_unchanged=1 no_fence_allocated=1\n",
	       headless ? "headless" : "clone", effect, target, error,
	       expected);
	return 0;
}
static int wb_wait_fence(struct wb_context *w, int fd)
{
	uint64_t end = cp_now() + UINT64_C(3000000000);
	while (cp_now() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int ret = poll(&p, 1, 100);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			return -1;
		if (!ret)
			continue;
		struct sync_file_info info = {};
		if (p.revents & POLLNVAL ||
		    call(fd, SYNC_IOC_FILE_INFO, &info) || !info.num_fences ||
		    !(fcntl(fd, F_GETFD) & FD_CLOEXEC)) {
			errno = EPROTO;
			return -1;
		}
		if (info.status) {
			w->pending = false;
			if (info.status < 0) {
				errno = -info.status;
				return -1;
			}
			if (info.status != 1) {
				errno = EPROTO;
				return -1;
			}
			return 0;
		}
	}
	errno = ETIMEDOUT;
	return -1;
}
static bool wb_bytes(const unsigned char *p, size_t n, unsigned char value)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != value)
			return false;
	return true;
}
static int wb_write(int fd, const void *data, size_t size)
{
	const unsigned char *p = data;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (!n)
				errno = EIO;
			return -1;
		}
		p += n;
		size -= n;
	}
	return 0;
}
static int wb_buffer_guard(const struct wb_buffer *b)
{
	unsigned int cpp = wb_cpp(b->fb.pixel_format);
	size_t start = b->fb.offsets[0],
	       end = start + (uint64_t)b->dumb.pitch * WB_H;
	if (wb_layout(b->fb.pixel_format, b->dumb.pitch, b->dumb.size) ||
	    start != b->dumb.pitch || end > b->dumb.size) {
		errno = EPROTO;
		return -1;
	}
	if (!wb_bytes(b->map, start, 0xa5) ||
	    !wb_bytes(b->map + end, b->dumb.size - end, 0xa5)) {
		errno = EOVERFLOW;
		return -1;
	}
	for (unsigned int y = 0; y < WB_H; y++)
		if (!wb_bytes(b->map + start + (uint64_t)y * b->dumb.pitch +
				      WB_W * cpp,
			      b->dumb.pitch - WB_W * cpp, 0xa5)) {
			errno = EOVERFLOW;
			return -1;
		}
	return 0;
}
static int wb_source_guard(struct wb_context *w)
{
	struct wb_buffer *s = &w->source;
	if (wb_buffer_guard(s))
		return -1;
	if (w->source_hash != wb_hash(s->map, s->dumb.size)) {
		errno = EOVERFLOW;
		return -1;
	}
	return 0;
}
static int wb_dump(struct wb_context *w, struct wb_buffer *b, const char *name)
{
	size_t start = b->fb.offsets[0];
	__sync_synchronize();
	if (wb_buffer_guard(b))
		return -1;
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/%s.raw", w->output, name) >=
	    (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	if (wb_write(fd, b->map + start, (uint64_t)b->dumb.pitch * WB_H)) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	if (close(fd))
		return -1;
	printf("RAW case=%s width=%u height=%u cpp=%u pitch=%u bytes=%llu offset=%u storage_fourcc=0x%08x storage_name=%s raw_fnv1a64=%016" PRIx64
	       " guards_and_padding=1 precision_qualified=0 no_CPU_swizzle=1 file=%s\n",
	       name, WB_W, WB_H, wb_cpp(b->fb.pixel_format), b->dumb.pitch,
	       (unsigned long long)b->dumb.pitch * WB_H, b->fb.offsets[0],
	       b->fb.pixel_format, wb_format_name(b->fb.pixel_format),
	       wb_hash(b->map + start, (uint64_t)b->dumb.pitch * WB_H), path);
	return 0;
}
static int wb_capture(struct wb_context *w, bool headless, char effect,
		      unsigned int slot, const char *name)
{
	struct wb_buffer *target = &w->target[slot];
	struct wb_request r;
	int cf = -1, wf = -1;
	uint32_t seq = 0;
	if (wb_identity(w) || wb_source_guard(w))
		return -1;
	memset(target->map, 0xa5, target->dumb.size);
	__sync_synchronize();
	if (wb_test(w, headless, effect, target->fb.fb_id, 0) ||
	    wb_build(w, &r, headless, target->fb.fb_id, &cf, &wf))
		return -1;
	uint64_t begin = cp_now(), token = ++w->c.token;
	if (wb_send(w, &r,
		    DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK |
			    DRM_MODE_PAGE_FLIP_EVENT,
		    token))
		return -1;
	w->changed = true;
	w->pending = true;
	if (cf < 0 || wf < 0) {
		errno = EPROTO;
		return -1;
	}
	int result = wb_wait_fence(w, wf);
	int error = result ? errno : 0;
	if (!result && cp_wait_completion(w->c.fd, cf, token, &seq)) {
		result = -1;
		error = errno;
	}
	if (!w->pending) {
		close(wf);
		close(cf);
	}
	if (result) {
		errno = error;
		return -1;
	}
	struct cp_state state;
	struct cp_object_state wbstate;
	if (cp_snapshot(&w->c, &state) ||
	    cp_read_object(w->c.fd, w->connector, DRM_MODE_OBJECT_CONNECTOR,
			   &wbstate))
		return -1;
	const struct cp_prop *hdmi = cp_find(&state.objects[CP_CONN],
					     "CRTC_ID"),
			     *wc = cp_find(&wbstate, "CRTC_ID"),
			     *fb = cp_find(&wbstate, "WRITEBACK_FB_ID");
	if (!hdmi || hdmi->value != (headless ? 0 : w->c.ids[CP_CRTC]) || !wc ||
	    wc->value != w->c.ids[CP_CRTC] || !fb || fb->value ||
	    !state.crtc.mode_valid ||
	    !wb_mode_equal(&state.crtc.mode, &w->mode)) {
		errno = EPROTO;
		return -1;
	}
	if (wb_dump(w, target, name) || wb_identity(w) || wb_source_guard(w))
		return -1;
	w->cases++;
	printf("CAPTURE_JSON {\"case\":\"%s\",\"route\":\"%s\",\"effect\":\"%c\",\"target_slot\":%u,\"writeback_fence\":true,\"crtc_fence_event\":true,\"raw_dump_saved\":true,\"packing_precision_qualified\":false,\"guard_rows\":true,\"HDMI_CRTC_ID\":%llu,\"WB_CRTC_ID\":%llu,\"sequence\":%u,\"elapsed_ms\":%.3f,\"source_immutable\":true,\"physical_sink_validated\":false}\n",
	       name, headless ? "headless" : "clone", effect, slot,
	       (unsigned long long)hdmi->value, (unsigned long long)wc->value,
	       seq, (cp_now() - begin) / 1000000.0);
	return 0;
}
static void wb_mode_diagnostic(const char *side,
			       const struct drm_mode_modeinfo *m)
{
	fprintf(stderr,
		"RESTORE_MODE side=%s name=%.32s clock=%u h=%u,%u,%u,%u skew=%u v=%u,%u,%u,%u scan=%u flags=0x%x type=0x%x vrefresh=%u\n",
		side, m->name, m->clock, m->hdisplay, m->hsync_start,
		m->hsync_end, m->htotal, m->hskew, m->vdisplay, m->vsync_start,
		m->vsync_end, m->vtotal, m->vscan, m->flags, m->type,
		m->vrefresh);
}
static int wb_connector_restored(const struct cp_object_state *saved,
				 const struct cp_object_state *actual)
{
	const struct cp_prop *attachment = cp_find(saved, "CRTC_ID");
	if (!attachment || saved->id != actual->id ||
	    saved->type != actual->type || saved->count != actual->count) {
		errno = EPROTO;
		return -1;
	}
	for (unsigned int i = 0; i < saved->count; i++) {
		const struct cp_prop *before = &saved->props[i],
				     *after = cp_find(actual, before->name);
		if (!after || before->id != after->id ||
		    before->flags != after->flags) {
			fprintf(stderr,
				"RESTORE_CONNECTOR_PROPERTY_SHAPE object=%u name=%s expected_id=%u observed_id=%u\n",
				saved->id, before->name, before->id,
				after ? after->id : 0);
			errno = EPROTO;
			return -1;
		}
		if (!strcmp(before->name, "WRITEBACK_OUT_FENCE_PTR")) {
			if (after->value) {
				errno = EPROTO;
				return -1;
			}
			continue;
		}
		if (!strcmp(before->name, "DPMS")) {
			uint64_t derived = attachment->value ?
						   DRM_MODE_DPMS_ON :
						   DRM_MODE_DPMS_OFF;
			if (after->value != before->value &&
			    after->value != derived) {
				fprintf(stderr,
					"RESTORE_CONNECTOR_DPMS object=%u saved=%llu observed=%llu derived=%llu\n",
					saved->id,
					(unsigned long long)before->value,
					(unsigned long long)after->value,
					(unsigned long long)derived);
				errno = EPROTO;
				return -1;
			}
			if (after->value != before->value)
				printf("RESTORE_CONNECTOR_DERIVED object=%u property=DPMS saved=%llu observed=%llu attachment=%llu accepted_atomic_legacy_value=1\n",
				       saved->id,
				       (unsigned long long)before->value,
				       (unsigned long long)after->value,
				       (unsigned long long)attachment->value);
			continue;
		}
		if (after->value != before->value) {
			fprintf(stderr,
				"RESTORE_CONNECTOR_VALUE object=%u property=%s expected=%llu observed=%llu\n",
				saved->id, before->name,
				(unsigned long long)before->value,
				(unsigned long long)after->value);
			errno = EPROTO;
			return -1;
		}
	}
	return 0;
}
static int wb_restore(struct wb_context *w)
{
	struct cp_context *c = &w->c;
	struct wb_request r = {};
	int fence = -1;
	uint32_t sequence = 0;
	const struct cp_object_state *h = &c->saved.objects[CP_CONN],
				     *crtc = &c->saved.objects[CP_CRTC];
	const struct cp_prop *active = cp_find(crtc, "ACTIVE"),
			     *conn = cp_find(h, "CRTC_ID");
	const char *const hc[] = { "link-status", "Colorspace", "max bpc",
				   "Broadcast RGB", "content type" };
	if (!active || !conn || wb_begin(&r, c->ids[CP_CONN]) ||
	    wb_add(&r, h, "CRTC_ID", conn->value, false))
		return -1;
	for (unsigned int i = 0; i < ARRAY_SIZE(hc); i++) {
		const struct cp_prop *p = cp_find(h, hc[i]);
		if (p && !(p->flags & DRM_MODE_PROP_IMMUTABLE) &&
		    wb_add(&r, h, hc[i], p->value, false))
			return -1;
	}
	if (wb_begin(&r, w->connector) ||
	    wb_add(&r, &w->wb_saved, "CRTC_ID", 0, false) ||
	    wb_add(&r, &w->wb_saved, "WRITEBACK_FB_ID", 0, false))
		return -1;
	if (wb_begin(&r, c->ids[CP_CRTC]) ||
	    wb_add(&r, crtc, "ACTIVE", active->value, false) ||
	    wb_add(&r, crtc, "MODE_ID",
		   c->saved.crtc.mode_valid ? c->restore_blob : 0, false) ||
	    wb_add(&r, crtc, "CTM", cp_find(crtc, "CTM")->value, false) ||
	    wb_add(&r, crtc, "GAMMA_LUT", cp_find(crtc, "GAMMA_LUT")->value,
		   false) ||
	    (active->value &&
	     wb_add(&r, crtc, "OUT_FENCE_PTR", PTR(&fence), false)))
		return -1;
	const char *const extra[] = { "zpos",		"alpha",
				      "rotation",	"pixel blend mode",
				      "COLOR_ENCODING", "COLOR_RANGE" };
	for (unsigned int i = CP_OSD1; i < CP_OBJECTS; i++) {
		const struct cp_object_state *p = &c->saved.objects[i];
		if (wb_begin(&r, c->ids[i]))
			return -1;
		for (unsigned int j = 0; j < ARRAY_SIZE(cp_geometry); j++) {
			const struct cp_prop *v = cp_find(p, cp_geometry[j]);
			if (!v ||
			    wb_add(&r, p, cp_geometry[j], v->value, false))
				return -1;
		}
		for (unsigned int j = 0; j < ARRAY_SIZE(extra); j++) {
			const struct cp_prop *v = cp_find(p, extra[j]);
			if (v && !(v->flags & DRM_MODE_PROP_IMMUTABLE) &&
			    wb_add(&r, p, extra[j], v->value, false))
				return -1;
		}
		if (wb_add(&r, p, "IN_FENCE_FD", (uint64_t)(int64_t)-1, true))
			return -1;
	}
	if (wb_send(w, &r,
		    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
		    0) ||
	    fence != -1)
		return -1;
	if (wb_send(w, &r,
		    DRM_MODE_ATOMIC_ALLOW_MODESET |
			    (active->value ? DRM_MODE_PAGE_FLIP_EVENT : 0),
		    ++c->token))
		return -1;
	if (active->value &&
	    (fence < 0 ||
	     cp_wait_completion(c->fd, fence, c->token, &sequence))) {
		if (fence >= 0)
			close(fence);
		return -1;
	}
	if (fence >= 0)
		close(fence);
	struct cp_state after;
	struct cp_object_state wbafter;
	if (cp_snapshot(c, &after) ||
	    cp_read_object(c->fd, w->connector, DRM_MODE_OBJECT_CONNECTOR,
			   &wbafter))
		return -1;
	for (unsigned int i = 0; i < CP_OBJECTS; i++)
		for (unsigned int j = 0; j < c->saved.objects[i].count; j++) {
			const struct cp_prop
				*saved = &c->saved.objects[i].props[j],
				*actual =
					cp_find(&after.objects[i], saved->name);
			if (!strcmp(saved->name, "MODE_ID") ||
			    !strcmp(saved->name, "OUT_FENCE_PTR") ||
			    !strcmp(saved->name, "IN_FENCE_FD"))
				continue;
			if (i == CP_CONN && !strcmp(saved->name, "DPMS")) {
				const struct cp_prop *attachment = cp_find(
					&c->saved.objects[CP_CONN], "CRTC_ID");
				uint64_t derived =
					attachment && attachment->value ?
						DRM_MODE_DPMS_ON :
						DRM_MODE_DPMS_OFF;
				if (!actual || actual->id != saved->id ||
				    actual->flags != saved->flags ||
				    (actual->value != saved->value &&
				     actual->value != derived)) {
					fprintf(stderr,
						"RESTORE_HDMI_DPMS expected_saved=%llu observed=%llu derived=%llu\n",
						(unsigned long long)saved->value,
						(unsigned long long)(actual ?
									     actual->value :
									     UINT64_MAX),
						(unsigned long long)derived);
					errno = EPROTO;
					return -1;
				}
				if (actual->value != saved->value)
					printf("RESTORE_CONNECTOR_DERIVED object=%u property=DPMS saved=%llu observed=%llu accepted_atomic_legacy_value=1\n",
					       c->ids[CP_CONN],
					       (unsigned long long)saved->value,
					       (unsigned long long)
						       actual->value);
				continue;
			}
			if (i == CP_CONN && !strcmp(saved->name, "EDID")) {
				if (!actual) {
					errno = EPROTO;
					return -1;
				}
				if (c->saved_edid_captured) {
					if (cp_compare_edid(c, actual))
						return -1;
				} else if (actual->value) {
					errno = EPROTO;
					return -1;
				}
				continue;
			}
			if (!actual || actual->id != saved->id ||
			    actual->flags != saved->flags ||
			    actual->value != saved->value) {
				fprintf(stderr,
					"RESTORE_MISMATCH object=%u property=%s\n",
					c->ids[i], saved->name);
				errno = EPROTO;
				return -1;
			}
		}
	if (after.crtc.mode_valid != c->saved.crtc.mode_valid ||
	    (after.crtc.mode_valid &&
	     !wb_mode_equal(&after.crtc.mode, &c->saved.crtc.mode))) {
		wb_mode_diagnostic("expected", &c->saved.crtc.mode);
		wb_mode_diagnostic("observed", &after.crtc.mode);
		errno = EPROTO;
		return -1;
	}
	if (after.crtc.mode_valid &&
	    memcmp(&after.crtc.mode, &c->saved.crtc.mode,
		   sizeof(after.crtc.mode))) {
		wb_mode_diagnostic("expected-metadata", &c->saved.crtc.mode);
		wb_mode_diagnostic("observed-metadata", &after.crtc.mode);
		puts("RESTORE_MODE_METADATA_NOTE timing_semantics_equal=1 name_type_vrefresh_padding_not_hardware_state=1");
	}
	if (wb_connector_restored(&w->wb_saved, &wbafter))
		return -1;
	w->changed = false;
	printf("RESTORE_OK original_mode=1 original_CTM_gamma_clones=1 original_planes=1 WB_detached=1 EDID_content=1\n");
	return 0;
}
static int wb_destroy_buffer(struct wb_context *w, struct wb_buffer *b)
{
	int error = 0;
	if (b->fb.fb_id && call(w->c.fd, DRM_IOCTL_MODE_RMFB, &b->fb.fb_id))
		error = errno;
	if (b->map && munmap(b->map, b->dumb.size) && !error)
		error = errno;
	if (b->dumb.handle) {
		struct drm_mode_destroy_dumb d = { .handle = b->dumb.handle };
		if (call(w->c.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) && !error)
			error = errno;
	}
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}
static int wb_selftest(void)
{
	unsigned int tests = 0;
	const uint32_t source_formats[] = { DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_XBGR2101010 };
	const uint32_t buffer_formats[] = { DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_XBGR2101010,
					    DRM_FORMAT_BGR888 };
	for (unsigned int f = 0; f < ARRAY_SIZE(source_formats); f++) {
		unsigned int bits = f ? 10 : 8, mask = (1U << bits) - 1;
		for (unsigned int c = 0; c < 3; c++)
			for (unsigned int v = 0; v <= mask; v++) {
				uint32_t word = wb_pattern(source_formats[f], v,
							   c * 128);
				unsigned int shift = f ? c * 10 : (2 - c) * 8;
				if (((word >> shift) & mask) != v ||
				    word >> (f ? 30 : 24))
					return 1;
			}
		tests++;
		for (unsigned int y = 512; y < 640; y++)
			for (unsigned int x = 0; x < WB_W; x++) {
				uint32_t word = wb_pattern(source_formats[f], x,
							   y),
					 r = f ? word & 1023 :
						 (word >> 16) & 255,
					 g = (word >> (f ? 10 : 8)) & mask,
					 b = f ? (word >> 20) & 1023 :
						 word & 255;
				if (r != ((x + 3 * y) & mask) ||
				    g != ((3 * x + 5 * y) & mask) ||
				    b != ((7 * x + 11 * y) & mask))
					return 1;
			}
		tests++;
		for (unsigned int y = 640; y < WB_H; y++)
			for (unsigned int x = 0; x < WB_W; x++) {
				uint32_t word = wb_pattern(source_formats[f], x,
							   y),
					 r = f ? word & 1023 :
						 (word >> 16) & 255,
					 g = (word >> (f ? 10 : 8)) & mask,
					 b = f ? (word >> 20) & 1023 :
						 word & 255;
				if (r != (x & 1 ? mask : 0) ||
				    g != (y & 1 ? mask : 0) ||
				    b != (x & 32 ? mask : 0))
					return 1;
			}
		tests++;
	}
	for (unsigned int f = 0; f < ARRAY_SIZE(buffer_formats); f++)
		for (unsigned int pad = 0; pad < 2; pad++) {
			struct wb_buffer b = {};
			unsigned int cpp = wb_cpp(buffer_formats[f]);
			b.dumb.pitch = (WB_W * cpp + 63) / 64 * 64 + pad * 64;
			b.dumb.size = (uint64_t)b.dumb.pitch * (WB_H + 2);
			b.fb.pixel_format = buffer_formats[f];
			b.fb.offsets[0] = b.dumb.pitch;
			b.map = malloc(b.dumb.size);
			if (!b.map)
				return 1;
			memset(b.map, 0xa5, b.dumb.size);
			if (f < 2)
				wb_fill_source(&b);
			else
				for (unsigned int y = 0; y < WB_H; y++)
					memset(b.map + b.fb.offsets[0] +
						       (uint64_t)y *
							       b.dumb.pitch,
					       0x5a, WB_W * cpp);
			if (wb_buffer_guard(&b))
				return 1;
			tests++;
			size_t guards[] = {
				0, b.fb.offsets[0] - 1,
				b.fb.offsets[0] + (uint64_t)b.dumb.pitch * WB_H,
				b.dumb.size - 1
			};
			for (unsigned int i = 0; i < ARRAY_SIZE(guards); i++) {
				b.map[guards[i]] = 0;
				if (!wb_buffer_guard(&b))
					return 1;
				b.map[guards[i]] = 0xa5;
				tests++;
			}
			if (pad) {
				size_t pos = b.fb.offsets[0] + WB_W * cpp;
				b.map[pos] = 0;
				if (!wb_buffer_guard(&b))
					return 1;
				b.map[pos] = 0xa5;
				tests++;
			}
			if (wb_layout(buffer_formats[f], b.dumb.pitch,
				      b.dumb.size - 1) != -1)
				return 1;
			tests++;
			if (wb_layout(buffer_formats[f], b.dumb.pitch - 1,
				      b.dumb.size) != -1)
				return 1;
			tests++;
			free(b.map);
		}
	struct drm_mode_modeinfo m = wb_mode();
	if (m.htotal * m.vtotal != 1867008 || m.flags != 5)
		return 1;
	printf("SELFTEST PASS fixture_depths=8,10 coordinate_and_parity=1 guard_layout_cases=%u no_DRM_or_board_access=1\n",
	       tests);
	return 0;
}
#ifndef WB_NO_MAIN
int main(int argc, char **argv)
{
	struct wb_context w = { .c = { .fd = -1, .fence = -1, .token = 68000 } };
	bool run = false, clone = false, headless = true, source_set = false,
	     target_set = false, restore_required = false,
	     restore_verified = false, resources_released = true,
	     source_ok = false;
	int failure = 0;
	unsigned int captures = 2;
	const char *device = "/dev/dri/card0";
	setbuf(stdout, NULL);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--selftest"))
			return wb_selftest();
		if (!strcmp(argv[i], "--help")) {
			puts("packing-matrix --run --source rgb8|rgb10 --target rgb8|rgb10 --expected-boot UUID --expected-notes SHA256 --output ABS_NEW_DIR [--route clone|headless] [--captures 1|2] [--device /dev/dri/card0]\nRoot must stop compositor and verify original fbcon/inactive ownership. Raw source/target packing diagnostic; NULL CTM/gamma, RGB8 wire request, raw output makes no precision claim. Failed unsignaled jobs retain all resources.");
			return 0;
		}
		if (!strcmp(argv[i], "--run"))
			run = true;
		else if (!strcmp(argv[i], "--expected-boot") && i + 1 < argc)
			w.boot = argv[++i];
		else if (!strcmp(argv[i], "--expected-notes") && i + 1 < argc)
			w.notes = argv[++i];
		else if (!strcmp(argv[i], "--output") && i + 1 < argc)
			w.output = argv[++i];
		else if (!strcmp(argv[i], "--source") && i + 1 < argc) {
			const char *p = argv[++i];
			if (strcmp(p, "rgb8") && strcmp(p, "rgb10"))
				return 2;
			w.source_format = !strcmp(p, "rgb8") ?
						  DRM_FORMAT_XRGB8888 :
						  DRM_FORMAT_XBGR2101010;
			source_set = true;
		} else if (!strcmp(argv[i], "--target") && i + 1 < argc) {
			const char *p = argv[++i];
			if (strcmp(p, "rgb8") && strcmp(p, "rgb10"))
				return 2;
			w.target_format = !strcmp(p, "rgb8") ?
						  DRM_FORMAT_BGR888 :
						  DRM_FORMAT_XBGR2101010;
			target_set = true;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else if (!strcmp(argv[i], "--route") && i + 1 < argc) {
			const char *p = argv[++i];
			clone = !strcmp(p, "clone");
			headless = !strcmp(p, "headless");
			if (!clone && !headless)
				return 2;
		} else if (!strcmp(argv[i], "--captures") && i + 1 < argc) {
			const char *p = argv[++i];
			if (strcmp(p, "1") && strcmp(p, "2"))
				return 2;
			captures = *p - '0';
		} else
			return 2;
	}
	if (!run || !source_set || !target_set || !w.boot ||
	    strlen(w.boot) != 36 || !w.notes || strlen(w.notes) != 64 ||
	    !w.output || w.output[0] != '/' || geteuid() != 0)
		return 2;
	for (unsigned int i = 0; i < 64; i++)
		if (!((w.notes[i] >= '0' && w.notes[i] <= '9') ||
		      (w.notes[i] >= 'a' && w.notes[i] <= 'f')))
			return 2;
	struct sigaction sa = { .sa_handler = wb_signal };
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL) ||
	    sigaction(SIGHUP, &sa, NULL))
		return 1;
	if (wb_identity(&w) || mkdir(w.output, 0700))
		goto fail;
	w.c.fd = open(device, O_RDWR | O_CLOEXEC);
	if (w.c.fd < 0)
		goto fail;
	struct drm_version version = {};
	char driver[64] = {};
	version.name = driver;
	version.name_len = sizeof(driver) - 1;
	if (call(w.c.fd, DRM_IOCTL_VERSION, &version) ||
	    version.name_len >= sizeof(driver) || strcmp(driver, "s7d-drm")) {
		errno = ENOTSUP;
		goto fail;
	}
	if (call(w.c.fd, DRM_IOCTL_SET_MASTER, NULL))
		goto fail;
	w.c.master = true;
	struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_ATOMIC,
					  .value = 1 };
	if (call(w.c.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap))
		goto fail;
	cap.capability = DRM_CLIENT_CAP_WRITEBACK_CONNECTORS;
	if (call(w.c.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap))
		goto fail;
	w.mode = wb_mode();
	if (wb_discover(&w, clone))
		goto fail;
	if (wb_clone_property(&w, "CTM", &w.original_ctm) ||
	    wb_clone_property(&w, "GAMMA_LUT", &w.original_gamma))
		goto fail;
	if (w.c.saved.crtc.mode_valid &&
	    wb_blob(w.c.fd, &w.c.saved.crtc.mode, sizeof(w.c.saved.crtc.mode),
		    &w.c.restore_blob))
		goto fail;
	if (wb_blob(w.c.fd, &w.mode, sizeof(w.mode), &w.c.mode_blob))
		goto fail;
	if (wb_buffer_make(&w, &w.source, w.source_format, false) ||
	    wb_buffer_make(&w, &w.target[0], w.target_format, true) ||
	    wb_buffer_make(&w, &w.target[1], w.target_format, true))
		goto fail;
	w.source_hash = wb_hash(w.source.map, w.source.dumb.size);
	if (wb_source_guard(&w) ||
	    wb_dump(&w, &w.source, wb_format_name(w.source_format)))
		goto fail;
	printf("SOURCE immutable=1 fnv1a64=%016" PRIx64
	       " source_format=%s target_format=%s source_and_target_guard_rows=2 source_unused_bits_zero=1 parity_region_y=640..719 CTM_NULL=1 GAMMA_NULL=1 RGB8_wire_request=1 no_CPU_swizzle=1 notes=%s boot=%s\n",
	       w.source_hash, wb_format_name(w.source_format),
	       wb_format_name(w.target_format), w.notes, w.boot);
	for (unsigned int i = 0; i < captures; i++) {
		char name[64];
		snprintf(name, sizeof(name), "%s-%u",
			 headless ? "headless" : "clone", i);
		if (cp_stop) {
			errno = EINTR;
			goto fail;
		}
		if (wb_capture(&w, headless, 'A', i % 2, name))
			goto fail;
	}
	goto cleanup;
fail:
	failure = errno ? errno : EIO;
	fprintf(stderr,
		"TEST_FAILURE errno=%d reason=%s cases=%u changed=%u WB_pending=%u\n",
		failure, strerror(failure), w.cases, w.changed, w.pending);
cleanup:
	if (w.pending)
		wb_retain(&w, "unretired-writeback");
	restore_required = w.changed;
	if (w.changed && wb_restore(&w))
		wb_retain(&w, "restore-incomplete");
	restore_verified = !w.changed;
	source_ok = w.source.map &&
		    w.source_hash == wb_hash(w.source.map, w.source.dumb.size);
	if (w.c.fd >= 0) {
		struct wb_buffer *buffers[] = { &w.source, &w.target[0],
						&w.target[1] };
		for (unsigned int i = 0; i < ARRAY_SIZE(buffers); i++)
			if (wb_destroy_buffer(&w, buffers[i])) {
				resources_released = false;
				if (!failure)
					failure = errno;
			}
		uint32_t blobs[] = { w.c.mode_blob, w.c.restore_blob,
				     w.original_ctm, w.original_gamma };
		for (unsigned int i = 0; i < ARRAY_SIZE(blobs); i++)
			if (blobs[i]) {
				struct drm_mode_destroy_blob d = {
					.blob_id = blobs[i]
				};
				if (call(w.c.fd, DRM_IOCTL_MODE_DESTROYPROPBLOB,
					 &d)) {
					resources_released = false;
					if (!failure)
						failure = errno;
				}
			}
		if (w.c.master && call(w.c.fd, DRM_IOCTL_DROP_MASTER, NULL)) {
			resources_released = false;
			if (!failure)
				failure = errno;
		}
		if (close(w.c.fd)) {
			resources_released = false;
			if (!failure)
				failure = errno;
		}
	}
	if (!failure && wb_identity(&w))
		failure = errno;
	printf("RESULT_JSON {\"status\":\"%s\",\"captures\":%u,\"TEST_ONLY\":%u,\"source_immutable\":%s,\"standard_writeback\":true,\"restore_required\":%s,\"restore_verified\":%s,\"resources_released\":%s,\"physical_sink_validated\":false,\"packing_precision_qualified\":false}\n",
	       failure ? "FAIL" : "PASS", w.cases, w.tests,
	       source_ok ? "true" : "false",
	       restore_required ? "true" : "false",
	       restore_verified ? "true" : "false",
	       resources_released ? "true" : "false");
	return failure ? 1 : 0;
}

#endif
