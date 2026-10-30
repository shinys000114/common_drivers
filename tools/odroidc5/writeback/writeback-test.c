// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#define CP_NO_MAIN
#include "../common/c5-kms-compose.c"
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/wait.h>

static unsigned int wb_width = 1920, wb_height = 720, wb_preset_index;
#define WB_W wb_width
#define WB_H wb_height
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
	uint32_t connector, ctm, gamma[2], original_ctm, original_gamma, alias;
	struct cp_object_state wb_saved;
	struct wb_buffer source, target[2], invalid, previous_source;
	uint32_t previous_mode_blob, capture_format;
	const char *mode_name;
	unsigned int mode_index, smoke;
	struct drm_mode_modeinfo mode;
	uint64_t source_hash;
	const char *boot, *notes, *output;
	unsigned int cases, tests;
	bool changed, pending, probe;
	const char *phase;
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
struct wb_preset {
	const char *name;
	struct drm_mode_modeinfo mode;
};
static const struct wb_preset wb_presets[] = {
	{ "vu12",
	  { .clock = 111750,
	    .hdisplay = 1920,
	    .hsync_start = 2016,
	    .hsync_end = 2208,
	    .htotal = 2496,
	    .vdisplay = 720,
	    .vsync_start = 723,
	    .vsync_end = 733,
	    .vtotal = 748,
	    .vrefresh = 60,
	    .flags = 5,
	    .type = DRM_MODE_TYPE_USERDEF,
	    .name = "1920x720" } },
	{ "1080p60",
	  { .clock = 148500,
	    .hdisplay = 1920,
	    .hsync_start = 2008,
	    .hsync_end = 2052,
	    .htotal = 2200,
	    .vdisplay = 1080,
	    .vsync_start = 1084,
	    .vsync_end = 1089,
	    .vtotal = 1125,
	    .vrefresh = 60,
	    .flags = 5,
	    .type = DRM_MODE_TYPE_USERDEF,
	    .name = "1920x1080" } },
	{ "fhd120",
	  { .clock = 297000,
	    .hdisplay = 1920,
	    .hsync_start = 2008,
	    .hsync_end = 2052,
	    .htotal = 2200,
	    .vdisplay = 1080,
	    .vsync_start = 1084,
	    .vsync_end = 1089,
	    .vtotal = 1125,
	    .vrefresh = 120,
	    .flags = 5,
	    .type = DRM_MODE_TYPE_USERDEF,
	    .name = "1920x1080" } },
	{ "4k30",
	  { .clock = 297000,
	    .hdisplay = 3840,
	    .hsync_start = 4016,
	    .hsync_end = 4104,
	    .htotal = 4400,
	    .vdisplay = 2160,
	    .vsync_start = 2168,
	    .vsync_end = 2178,
	    .vtotal = 2250,
	    .vrefresh = 30,
	    .flags = 5,
	    .type = DRM_MODE_TYPE_USERDEF,
	    .name = "3840x2160" } },
	{ "4k60",
	  { .clock = 594000,
	    .hdisplay = 3840,
	    .hsync_start = 4016,
	    .hsync_end = 4104,
	    .htotal = 4400,
	    .vdisplay = 2160,
	    .vsync_start = 2168,
	    .vsync_end = 2178,
	    .vtotal = 2250,
	    .vrefresh = 60,
	    .flags = 5,
	    .type = DRM_MODE_TYPE_USERDEF,
	    .name = "3840x2160" } },
	{ "dci4k60",
	  { .clock = 594000,
	    .hdisplay = 4096,
	    .hsync_start = 4184,
	    .hsync_end = 4272,
	    .htotal = 4400,
	    .vdisplay = 2160,
	    .vsync_start = 2168,
	    .vsync_end = 2178,
	    .vtotal = 2250,
	    .vrefresh = 60,
	    .flags = 5,
	    .type = DRM_MODE_TYPE_USERDEF,
	    .name = "4096x2160" } }
};
static int wb_preset(const char *name, unsigned int *index)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(wb_presets); i++)
		if (!strcmp(name, wb_presets[i].name)) {
			*index = i;
			return 0;
		}
	errno = EINVAL;
	return -1;
}
static struct drm_mode_modeinfo wb_mode(void)
{
	return wb_presets[wb_preset_index].mode;
}
static void wb_select_mode(struct wb_context *w, unsigned int preset,
			   unsigned int index)
{
	wb_preset_index = preset;
	w->mode = wb_mode();
	w->mode_name = wb_presets[preset].name;
	w->mode_index = index;
	wb_width = w->mode.hdisplay;
	wb_height = w->mode.vdisplay;
	printf("MODE_SELECT index=%u preset=%s size=%ux%u pixel_khz=%u htotal=%u vtotal=%u flags=%u headless_source_mode=1\n",
	       index, w->mode_name, WB_W, WB_H, w->mode.clock, w->mode.htotal,
	       w->mode.vtotal, w->mode.flags);
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
	w->phase = "resource-query";
	if (call(c->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) ||
	    call(c->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &list))
		return -1;
	printf("PROBE_RESOURCES crtcs=%u connectors=%u planes=%u expected=1,2,5\n",
	       res.count_crtcs, res.count_connectors, list.count_planes);
	w->phase = "resource-shape";
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
		w->phase = "connector-query";
		if (call(c->fd, DRM_IOCTL_MODE_GETCONNECTOR, &q) ||
		    q.count_modes > ARRAY_SIZE(modes))
			return -1;
		printf("PROBE_CONNECTOR id=%u type=%u type_id=%u connection=%u modes=%u\n",
		       q.connector_id, q.connector_type, q.connector_type_id,
		       q.connection, q.count_modes);
		for (unsigned int m = 0; m < q.count_modes; m++)
			printf("PROBE_MODE connector=%u index=%u name=%.32s clock=%u h=%u,%u,%u,%u v=%u,%u,%u,%u flags=%u qualified_equal=%u\n",
			       q.connector_id, m, modes[m].name, modes[m].clock,
			       modes[m].hdisplay, modes[m].hsync_start,
			       modes[m].hsync_end, modes[m].htotal,
			       modes[m].vdisplay, modes[m].vsync_start,
			       modes[m].vsync_end, modes[m].vtotal,
			       modes[m].flags,
			       wb_mode_equal(&modes[m], &w->mode));
		w->phase = "connector-type";
		if (q.connector_type == DRM_MODE_CONNECTOR_HDMIA) {
			if (c->ids[CP_CONN]) {
				errno = ENOTSUP;
				return -1;
			}
			c->ids[CP_CONN] = q.connector_id;
			if (clone && !w->probe) {
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
	w->phase = "connector-identity";
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
		bool rgb = false, nv12 = false, nv21 = false;
		w->phase = "plane-query";
		if (call(c->fd, DRM_IOCTL_MODE_GETPLANE, &p) ||
		    property(c->fd, p.plane_id, DRM_MODE_OBJECT_PLANE, "type",
			     &id, &type))
			return -1;
		printf("PROBE_PLANE index=%u id=%u type=%llu possible_crtcs=0x%x formats=%u\n",
		       i, p.plane_id, (unsigned long long)type,
		       p.possible_crtcs, p.count_format_types);
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
		printf("PROBE_PLANE_FORMATS id=%u rgb=%u nv12=%u nv21=%u values=",
		       p.plane_id, rgb, nv12, nv21);
		for (unsigned int f = 0; f < p.count_format_types; f++)
			printf("%s%08x", f ? "," : "", formats[f]);
		puts("");
		w->phase = "plane-classification";
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
	w->phase = "plane-identities";
	if (nv != 2 || !c->ids[CP_OSD1] || !c->ids[CP_OSD2] ||
	    !c->ids[CP_CURSOR]) {
		errno = ENODEV;
		return -1;
	}
	c->ids[CP_VD1] = videos[0];
	c->ids[CP_VD2] = videos[1];
	w->phase = "initial-snapshot";
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
	printf("PROBE_WB_PROPERTIES crtc_present=%u crtc=%llu job_present=%u job=%llu formats_present=%u formats_blob=%llu EDID_present=%u\n",
	       !!attached, (unsigned long long)(attached ? attached->value : 0),
	       !!job, (unsigned long long)(job ? job->value : 0), !!formats,
	       (unsigned long long)(formats ? formats->value : 0), !!edid);
	w->phase = "initial-WB-properties";
	if (!attached || (!w->probe && attached->value) || !job ||
	    (!w->probe && job->value) || !formats || !formats->value || !edid) {
		errno = EBUSY;
		return -1;
	}
	uint32_t fmts[16], format_length = 0;
	w->phase = "WB-format-blob";
	if (wb_read_blob(c->fd, formats->value, fmts, sizeof(fmts),
			 sizeof(fmts[0]), &format_length))
		return -1;
	printf("PROBE_WB_FORMATS bytes=%u requested_capture_format=%08x values=",
	       format_length, w->capture_format);
	for (unsigned int i = 0; i < format_length / 4; i++)
		printf("%s%08x", i ? "," : "", fmts[i]);
	puts("");
	w->phase = "WB-format-support";
	bool supported = false;
	for (unsigned int i = 0; i < format_length / 4; i++)
		supported |= fmts[i] == w->capture_format;
	if (!supported) {
		errno = ENOTSUP;
		return -1;
	}
	if (w->probe) {
		puts("PROBE_COMPLETE no_master=1 no_buffers=1 no_modeset=1 no_capture=1");
		return 0;
	}
	w->phase = "EDID-copy";
	if (edid->value) {
		if (cp_capture_edid(c))
			return -1;
	} else if (clone) {
		errno = ENODEV;
		return -1;
	}
	w->phase = "initial-overlay-ownership";
	for (unsigned int i = CP_OSD2; i < CP_OBJECTS; i++) {
		const struct cp_prop *fb =
			cp_find(&c->saved.objects[i], "FB_ID");
		if (!fb || fb->value) {
			errno = EBUSY;
			return -1;
		}
	}
	w->phase = "primary-blend-enum";
	if (cp_enum(c, CP_OSD1, "pixel blend mode", "None", &c->none[0]))
		return -1;
	printf("DISCOVERY HDMI=%u WB=%u CRTC=%u primary=%u standard_writeback_cap=1 capture_format=0x%08x saved_EDID_bytes=%u initial_WB_detached=1\n",
	       c->ids[CP_CONN], w->connector, c->ids[CP_CRTC], c->ids[CP_OSD1],
	       w->capture_format, c->saved_edid_length);
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
static uint32_t wb_pattern(unsigned int x, unsigned int y)
{
	static const uint32_t palette[] = { 0xff0000, 0x00ff00, 0x0000ff,
					    0xffffff, 0,	0xffff00,
					    0xff00ff, 0x00ffff };
	if (y < WB_H / 3)
		return palette[x / (WB_W / 8)];
	if (y < WB_H * 2 / 3) {
		unsigned int grey = (uint64_t)x * 255 / (WB_W - 1);
		return grey * 0x010101U;
	}
	return ((x / 32) ^ (y / 32)) & 1 ? 0x0000ff : 0xffffff;
}
static void wb_expected(char effect, unsigned int x, unsigned int y,
			unsigned char out[3])
{
	uint32_t p = wb_pattern(x, y);
	out[0] = p >> 16;
	out[1] = p >> 8;
	out[2] = p;
	if (effect == 'T') {
		unsigned char v = out[0];
		out[0] = out[2];
		out[2] = v;
	}
	if (effect == 'O' || effect == 'B') {
		out[0] = effect == 'O' ? 255 : 16;
		out[1] = 64;
		out[2] = effect == 'O' ? 16 : 255;
	}
}
static unsigned int wb_cpp(uint32_t format)
{
	return format == DRM_FORMAT_BGR888 ? 3 : 4;
}
static const char *wb_format_name(uint32_t format)
{
	return format == DRM_FORMAT_BGR888 ? "bgr888" : "xbgr2101010";
}
static unsigned int wb_component_bits(uint32_t format)
{
	return format == DRM_FORMAT_BGR888 ? 8 : 10;
}
static uint16_t wb_lut10(uint16_t value)
{
	/* drm_color_lut_extract(value, 10), before S7D's 12-bit alignment. */
	return ((uint32_t)value * 1023 + 32767) / 65535;
}
static void wb_expected_capture(uint32_t format, char effect, unsigned int x,
				unsigned int y, uint16_t out[3])
{
	unsigned char source[3];
	wb_expected(effect, x, y, source);
	for (unsigned int channel = 0; channel < 3; channel++)
		out[channel] = format == DRM_FORMAT_BGR888 ? source[channel] :
						       source[channel] << 2;
	if (format != DRM_FORMAT_BGR888 && (effect == 'O' || effect == 'B')) {
		out[0] = wb_lut10(effect == 'O' ? 65535 : 4096);
		out[1] = wb_lut10(16384);
		out[2] = wb_lut10(effect == 'O' ? 4096 : 65535);
	}
}
static void wb_unpack(uint32_t format, const unsigned char *pixel,
		      uint16_t out[3])
{
	if (format == DRM_FORMAT_BGR888) {
		for (unsigned int channel = 0; channel < 3; channel++)
			out[channel] = pixel[channel];
		return;
	}
	uint32_t word = (uint32_t)pixel[0] | (uint32_t)pixel[1] << 8 |
			(uint32_t)pixel[2] << 16 | (uint32_t)pixel[3] << 24;
	for (unsigned int channel = 0; channel < 3; channel++)
		out[channel] = (word >> (channel * 10)) & 1023;
}
static int wb_buffer_make(struct wb_context *w, struct wb_buffer *b,
			  uint32_t format, bool guard)
{
	struct drm_mode_map_dumb map = {};
	b->dumb = (struct drm_mode_create_dumb){
		.width = WB_W,
		.height = WB_H + (guard ? 2 : 0),
		.bpp = wb_cpp(format) * 8
	};
	if (call(w->c.fd, DRM_IOCTL_MODE_CREATE_DUMB, &b->dumb))
		return -1;
	if (!b->dumb.size || b->dumb.size > SIZE_MAX ||
	    b->dumb.pitch < WB_W * wb_cpp(format) ||
	    (b->dumb.pitch & 63) ||
	    b->dumb.size < (uint64_t)b->dumb.pitch * b->dumb.height) {
		errno = EPROTO;
		return -1;
	}
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
	b->fb = (struct drm_mode_fb_cmd2){
		.width = WB_W,
		.height = WB_H,
		.pixel_format = format,
		.flags = DRM_MODE_FB_MODIFIERS,
		.handles = { b->dumb.handle },
		.pitches = { b->dumb.pitch },
		.offsets = { guard ? b->dumb.pitch : 0 },
		.modifier = { DRM_FORMAT_MOD_LINEAR }
	};
	if (call(w->c.fd, DRM_IOCTL_MODE_ADDFB2, &b->fb))
		return -1;
	if (!guard) {
		for (unsigned int y = 0; y < WB_H; y++)
			for (unsigned int x = 0; x < WB_W; x++)
				((uint32_t *)(b->map +
					      (uint64_t)y * b->dumb.pitch))[x] =
					0xff000000 | wb_pattern(x, y);
		if (mprotect(b->map, b->dumb.size, PROT_READ))
			return -1;
	}
	return 0;
}
static int wb_gamma_make(struct wb_context *w, uint32_t *id, bool blue)
{
	struct drm_color_lut lut[65];
	for (unsigned int i = 0; i < ARRAY_SIZE(lut); i++)
		lut[i] = (struct drm_color_lut){ .red = blue ? 4096 : 65535,
						 .green = 16384,
						 .blue = blue ? 65535 : 4096 };
	return wb_blob(w->c.fd, lut, sizeof(lut), id);
}
static int wb_build(struct wb_context *w, struct wb_request *r, bool headless,
		    char effect, uint32_t target, int *crtc_fence,
		    int *wb_fence)
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
	    wb_add(r, crtc, "CTM", effect == 'T' ? w->ctm : 0, false) ||
	    wb_add(r, crtc, "GAMMA_LUT",
		   effect == 'O' ? w->gamma[0] :
		   effect == 'B' ? w->gamma[1] :
				   0,
		   false) ||
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
	uint64_t hashes[3] = {
		wb_hash(w->target[0].map, w->target[0].dumb.size),
		wb_hash(w->target[1].map, w->target[1].dumb.size),
		wb_hash(w->invalid.map, w->invalid.dumb.size)
	};
	if (cp_snapshot(&w->c, &before) ||
	    cp_read_object(w->c.fd, w->connector, DRM_MODE_OBJECT_CONNECTOR,
			   &bbefore) ||
	    wb_build(w, &r, headless, effect, target, &cf, &wf))
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
	    hashes[2] != wb_hash(w->invalid.map, w->invalid.dumb.size) ||
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
static int wb_pixels(struct wb_context *w, struct wb_buffer *b,
		     const char *name, char effect)
{
	unsigned int max_error = 0, cpp = wb_cpp(b->fb.pixel_format);
	uint64_t mismatches = 0, total_error = 0;
	uint16_t expected[3], observed[3];
	size_t start = b->fb.offsets[0],
	       end = start + (uint64_t)b->dumb.pitch * WB_H;
	__sync_synchronize();
	if (!wb_bytes(b->map, start, 0xa5) ||
	    !wb_bytes(b->map + end, b->dumb.size - end, 0xa5)) {
		errno = EOVERFLOW;
		return -1;
	}
	for (unsigned int y = 0; y < WB_H; y++) {
		const unsigned char *row =
			b->map + start + (uint64_t)y * b->dumb.pitch;
		for (unsigned int x = 0; x < WB_W; x++) {
			wb_expected_capture(b->fb.pixel_format, effect, x, y, expected);
			wb_unpack(b->fb.pixel_format, row + x * cpp, observed);
			for (unsigned int channel = 0; channel < 3; channel++) {
				unsigned int v = observed[channel], e = expected[channel],
					     delta = v > e ? v - e : e - v;
				max_error = delta > max_error ? delta : max_error;
				total_error += delta;
				mismatches += delta != 0;
			}
		}
		if (!wb_bytes(row + WB_W * cpp, b->dumb.pitch - WB_W * cpp, 0xa5)) {
			errno = EOVERFLOW;
			return -1;
		}
	}
	char path[PATH_MAX];
	const char *extension = b->fb.pixel_format == DRM_FORMAT_BGR888 ?
					"rgb" : "xbgr2101010";
	if (snprintf(path, sizeof(path), "%s/%s.%s", w->output, name,
		     extension) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	for (unsigned int y = 0; y < WB_H; y++)
		if (wb_write(fd, b->map + start + (uint64_t)y * b->dumb.pitch,
			     WB_W * cpp)) {
			int e = errno;
			close(fd);
			errno = e;
			return -1;
		}
	if (close(fd))
		return -1;
	printf("PIXELS case=%s effect=%c pixels=%u component_mismatches=%llu max_error=%u total_error=%llu guard_and_padding_ok=1 payload_fnv1a64=%016" PRIx64
	       " file=%s exact=%u capture_format=%s capture_bits=%u cpp=%u raw_row_bytes=%u buffer_pitch=%u X_bits_ignored=%u\n",
	       name, effect, WB_W * WB_H, (unsigned long long)mismatches,
	       max_error, (unsigned long long)total_error,
	       wb_hash(b->map + start, (uint64_t)b->dumb.pitch * WB_H), path,
	       !mismatches, wb_format_name(b->fb.pixel_format),
	       wb_component_bits(b->fb.pixel_format), cpp, WB_W * cpp,
	       b->dumb.pitch, b->fb.pixel_format == DRM_FORMAT_XBGR2101010);
	if (mismatches) {
		errno = EILSEQ;
		return -1;
	}
	return 0;
}
static int wb_capture(struct wb_context *w, bool headless, char effect,
		      unsigned int slot, const char *name)
{
	struct wb_buffer *target = &w->target[slot];
	struct wb_request r;
	int cf = -1, wf = -1;
	uint32_t seq = 0;
	if (wb_identity(w) ||
	    w->source_hash != wb_hash(w->source.map, w->source.dumb.size)) {
		errno = ESTALE;
		return -1;
	}
	memset(target->map, 0xa5, target->dumb.size);
	__sync_synchronize();
	if (wb_test(w, headless, effect, target->fb.fb_id, 0) ||
	    wb_build(w, &r, headless, effect, target->fb.fb_id, &cf, &wf))
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
	if (wb_pixels(w, target, name, effect) || wb_identity(w) ||
	    w->source_hash != wb_hash(w->source.map, w->source.dumb.size))
		return -1;
	w->cases++;
	printf("CAPTURE_JSON {\"case\":\"%s\",\"route\":\"%s\",\"effect\":\"%c\",\"width\":%u,\"height\":%u,\"pixel_khz\":%u,\"mode_index\":%u,\"target_slot\":%u,\"writeback_fence\":true,\"crtc_fence_event\":true,\"all_pixels_exact\":true,\"guard_rows\":true,\"HDMI_CRTC_ID\":%llu,\"WB_CRTC_ID\":%llu,\"sequence\":%u,\"elapsed_ms\":%.3f,\"source_immutable\":true,\"source_rgb_bits\":8,\"capture_format\":\"%s\",\"capture_component_bits\":%u,\"physical_sink_validated\":false}\n",
	       name, headless ? "headless" : "clone", effect, WB_W, WB_H,
	       w->mode.clock, w->mode_index, slot,
	       (unsigned long long)hdmi->value, (unsigned long long)wc->value,
	       seq, (cp_now() - begin) / 1000000.0,
	       wb_format_name(w->capture_format), wb_component_bits(w->capture_format));
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
static int wb_destroy_buffer(struct wb_context *w, struct wb_buffer *b);
static int wb_mode_buffers(struct wb_context *w)
{
	w->phase = "test-mode-blob";
	if (wb_blob(w->c.fd, &w->mode, sizeof(w->mode), &w->c.mode_blob))
		return -1;
	w->phase = "test-buffers";
	if (wb_buffer_make(w, &w->source, DRM_FORMAT_XRGB8888, false) ||
	    wb_buffer_make(w, &w->target[0], w->capture_format, true) ||
	    wb_buffer_make(w, &w->target[1], w->capture_format, true) ||
	    (!w->smoke &&
	     wb_buffer_make(w, &w->invalid, DRM_FORMAT_XRGB8888, true)))
		return -1;
	w->source_hash = wb_hash(w->source.map, w->source.dumb.size);
	struct drm_mode_fb_cmd2 alias = { .width = WB_W,
					  .height = WB_H,
					  .pixel_format = w->capture_format,
					  .flags = DRM_MODE_FB_MODIFIERS,
					  .handles = { w->source.dumb.handle },
					  .pitches = { w->source.dumb.pitch },
					  .modifier = {
						  DRM_FORMAT_MOD_LINEAR } };
	w->phase = "alias-framebuffer";
	if (!w->smoke) {
		if (call(w->c.fd, DRM_IOCTL_MODE_ADDFB2, &alias))
			return -1;
		w->alias = alias.fb_id;
	}
	printf("SOURCE preset=%s mode_index=%u immutable=1 fnv1a64=%016" PRIx64
	       " source_format=XRGB8888 source_bits=8 target_format=%s target_bits=%u target_guard_rows=2 target_offset=pitch pitch=%u notes=%s boot=%s\n",
	       w->mode_name, w->mode_index, w->source_hash,
	       wb_format_name(w->capture_format), wb_component_bits(w->capture_format),
	       w->target[0].dumb.pitch, w->notes, w->boot);
	return 0;
}
static int wb_next_mode(struct wb_context *w, unsigned int preset,
			unsigned int index)
{
	if (w->pending || w->previous_source.dumb.handle ||
	    w->previous_mode_blob) {
		errno = EBUSY;
		return -1;
	}
	if (w->alias) {
		if (call(w->c.fd, DRM_IOCTL_MODE_RMFB, &w->alias))
			return -1;
		w->alias = 0;
	}
	if (wb_destroy_buffer(w, &w->target[0]) ||
	    wb_destroy_buffer(w, &w->target[1]) ||
	    wb_destroy_buffer(w, &w->invalid))
		return -1;
	w->previous_source = w->source;
	memset(&w->source, 0, sizeof(w->source));
	w->previous_mode_blob = w->c.mode_blob;
	w->c.mode_blob = 0;
	wb_select_mode(w, preset, index);
	return wb_mode_buffers(w);
}
static int wb_finish_transition(struct wb_context *w)
{
	if (w->pending) {
		errno = EBUSY;
		return -1;
	}
	if (wb_destroy_buffer(w, &w->previous_source))
		return -1;
	if (w->previous_mode_blob) {
		struct drm_mode_destroy_blob b = {
			.blob_id = w->previous_mode_blob
		};
		if (call(w->c.fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &b))
			return -1;
		w->previous_mode_blob = 0;
	}
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
	memset(b, 0, sizeof(*b));
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}
static int wb_selftest(void)
{
	unsigned char p[3];
	wb_expected('A', 0, 0, p);
	if (memcmp(p, (unsigned char[]){ 255, 0, 0 }, 3))
		return 1;
	wb_expected('T', 0, 0, p);
	if (memcmp(p, (unsigned char[]){ 0, 0, 255 }, 3))
		return 1;
	wb_expected('O', 1919, 719, p);
	if (memcmp(p, (unsigned char[]){ 255, 64, 16 }, 3))
		return 1;
	wb_expected('B', 1919, 719, p);
	if (memcmp(p, (unsigned char[]){ 16, 64, 255 }, 3))
		return 1;
	for (unsigned int x = 0; x < WB_W; x++) {
		wb_expected('A', x, WB_H / 2, p);
		if (p[0] != p[1] || p[1] != p[2])
			return 1;
	}
	uint16_t expected10[3], observed10[3];
	const char effects[] = "ATOB";
	const uint16_t colors10[][3] = {
		{ 1020, 0, 0 }, { 0, 0, 1020 },
		{ 1023, 256, 64 }, { 64, 256, 1023 }
	};
	for (unsigned int i = 0; i < sizeof(effects) - 1; i++) {
		wb_expected_capture(DRM_FORMAT_XBGR2101010, effects[i], 0, 0,
				    expected10);
		if (memcmp(expected10, colors10[i], sizeof(expected10)))
			return 1;
		for (unsigned int padding = 0; padding < 4; padding++) {
			uint32_t word = expected10[0] | (uint32_t)expected10[1] << 10 |
					(uint32_t)expected10[2] << 20 | padding << 30;
			unsigned char bytes[4];
			for (unsigned int byte = 0; byte < 4; byte++)
				bytes[byte] = word >> (byte * 8);
			wb_unpack(DRM_FORMAT_XBGR2101010, bytes, observed10);
			if (memcmp(observed10, expected10, sizeof(expected10)))
				return 1;
		}
	}
	for (unsigned int value = 0; value < 256; value++) {
		unsigned char bytes[] = { value, value, value };
		wb_unpack(DRM_FORMAT_BGR888, bytes, observed10);
		for (unsigned int channel = 0; channel < 3; channel++)
			if (observed10[channel] != value)
				return 1;
	}
	struct drm_mode_modeinfo m = wb_mode();
	if (m.htotal * m.vtotal != 1867008 || m.flags != 5)
		return 1;
	for (unsigned int i = 0; i < ARRAY_SIZE(wb_presets); i++) {
		unsigned int index = 99;
		if (wb_preset(wb_presets[i].name, &index) || index != i)
			return 1;
		wb_preset_index = i;
		wb_width = wb_presets[i].mode.hdisplay;
		wb_height = wb_presets[i].mode.vdisplay;
		for (unsigned int y = 0; y < WB_H; y += WB_H / 3)
			for (unsigned int x = 0; x < WB_W; x += WB_W / 8) {
				wb_expected('A', x, y, p);
				if (wb_pattern(x, y) > 0xffffff)
					return 1;
			}
	}
	wb_preset_index = 0;
	wb_width = 1920;
	wb_height = 720;
	puts("SELFTEST PASS actual_C_reference=1 modes=6 pattern_channels_gamma=1 capture_formats=2 X2_padding_ignored=1 gamma10_quantization=1 no_DRM_or_board_access=1");
	return 0;
}
int main(int argc, char **argv)
{
	struct wb_context w = { .c = { .fd = -1, .fence = -1, .token = 68000 },
				.capture_format = DRM_FORMAT_BGR888 };
	bool run = false, clone = true, headless = true,
	     restore_required = false, restore_verified = false,
	     resources_released = true, source_ok = false;
	int failure = 0;
	unsigned int modes[16] = { 0 }, mode_count = 1, smoke = 0;
	bool mode_argument = false;
	const char *device = "/dev/dri/card0";
	setbuf(stdout, NULL);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--selftest"))
			return wb_selftest();
		if (!strcmp(argv[i], "--help")) {
			puts("writeback-test --mode vu12|1080p60|fhd120|4k30|4k60|dci4k60 [--mode-sequence vu12,4k60,vu12] [--smoke 1|2] [--capture-format bgr888|xbgr2101010] --run (or --probe) --expected-boot UUID --expected-notes SHA256 --output ABS_NEW_DIR [--route both|clone|headless] [--device /dev/dri/card0]\nRoot must stop compositor and verify original fbcon/inactive ownership. Uses standard DRM writeback only; failed unsignaled jobs retain all resources.");
			return 0;
		}
		if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			if (mode_argument || wb_preset(argv[++i], &modes[0]))
				return 2;
			mode_argument = true;
		} else if (!strcmp(argv[i], "--mode-sequence") &&
			   i + 1 < argc) {
			if (mode_argument)
				return 2;
			char *text = strdup(argv[++i]), *save = NULL, *word;
			if (!text)
				return 1;
			mode_count = 0;
			for (word = strtok_r(text, ",", &save); word;
			     word = strtok_r(NULL, ",", &save)) {
				if (mode_count == ARRAY_SIZE(modes) ||
				    wb_preset(word, &modes[mode_count])) {
					free(text);
					return 2;
				}
				mode_count++;
			}
			free(text);
			if (!mode_count)
				return 2;
			mode_argument = true;
		} else if (!strcmp(argv[i], "--capture-format") && i + 1 < argc) {
			const char *format = argv[++i];
			if (!strcmp(format, "bgr888"))
				w.capture_format = DRM_FORMAT_BGR888;
			else if (!strcmp(format, "xbgr2101010"))
				w.capture_format = DRM_FORMAT_XBGR2101010;
			else
				return 2;
		} else if (!strcmp(argv[i], "--smoke") && i + 1 < argc) {
			if (cp_number(argv[++i], 1, 2, &smoke))
				return 2;
		} else if (!strcmp(argv[i], "--probe"))
			w.probe = true;
		else if (!strcmp(argv[i], "--run"))
			run = true;
		else if (!strcmp(argv[i], "--expected-boot") && i + 1 < argc)
			w.boot = argv[++i];
		else if (!strcmp(argv[i], "--expected-notes") && i + 1 < argc)
			w.notes = argv[++i];
		else if (!strcmp(argv[i], "--output") && i + 1 < argc)
			w.output = argv[++i];
		else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else if (!strcmp(argv[i], "--route") && i + 1 < argc) {
			const char *p = argv[++i];
			clone = strcmp(p, "headless") != 0;
			headless = strcmp(p, "clone") != 0;
			if (strcmp(p, "both") && strcmp(p, "headless") &&
			    strcmp(p, "clone"))
				return 2;
		} else
			return 2;
	}
	if (!(run || w.probe) || !w.boot || strlen(w.boot) != 36 || !w.notes ||
	    strlen(w.notes) != 64 ||
	    (!w.probe && (!w.output || w.output[0] != '/')) || geteuid() != 0)
		return 2;
	for (unsigned int i = 0; i < 64; i++)
		if (!((w.notes[i] >= '0' && w.notes[i] <= '9') ||
		      (w.notes[i] >= 'a' && w.notes[i] <= 'f')))
			return 2;
	if (mode_count > 1 && clone)
		return 2;
	w.smoke = smoke;
	struct sigaction sa = { .sa_handler = wb_signal };
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL) ||
	    sigaction(SIGHUP, &sa, NULL))
		return 1;
	w.phase = "identity-and-output";
	if (wb_identity(&w) || (!w.probe && mkdir(w.output, 0700)))
		goto fail;
	w.phase = "device-open";
	w.c.fd = open(device, O_RDWR | O_CLOEXEC);
	if (w.c.fd < 0)
		goto fail;
	struct drm_version version = {};
	char driver[64] = {};
	version.name = driver;
	version.name_len = sizeof(driver) - 1;
	w.phase = "driver-version-query";
	int version_result = call(w.c.fd, DRM_IOCTL_VERSION, &version),
	    version_error = version_result ? errno : 0;
	printf("PROBE_DRIVER result=%d errno=%d name=%.63s name_length=%zu version=%d.%d.%d expected_name=s7d-drm\n",
	       version_result, version_error, driver, version.name_len,
	       version.version_major, version.version_minor,
	       version.version_patchlevel);
	if (version_result) {
		errno = version_error;
		goto fail;
	}
	w.phase = "driver-name";
	if (version.name_len >= sizeof(driver) || strcmp(driver, "s7d-drm")) {
		errno = ENOTSUP;
		goto fail;
	}
	w.phase = "set-master";
	if (!w.probe) {
		if (call(w.c.fd, DRM_IOCTL_SET_MASTER, NULL))
			goto fail;
		w.c.master = true;
	}
	struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_ATOMIC,
					  .value = 1 };
	w.phase = "atomic-client-cap";
	if (call(w.c.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap))
		goto fail;
	cap.capability = DRM_CLIENT_CAP_WRITEBACK_CONNECTORS;
	w.phase = "writeback-client-cap";
	if (call(w.c.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap))
		goto fail;
	wb_select_mode(&w, modes[0], 0);
	if (wb_discover(&w, clone))
		goto fail;
	if (w.probe)
		goto cleanup;
	w.phase = "original-blob-clones";
	if (wb_clone_property(&w, "CTM", &w.original_ctm) ||
	    wb_clone_property(&w, "GAMMA_LUT", &w.original_gamma))
		goto fail;
	if (w.c.saved.crtc.mode_valid &&
	    wb_blob(w.c.fd, &w.c.saved.crtc.mode, sizeof(w.c.saved.crtc.mode),
		    &w.c.restore_blob))
		goto fail;
	struct drm_color_ctm ctm = { .matrix = { 0, 0, UINT64_C(1) << 32, 0,
						 UINT64_C(1) << 32, 0,
						 UINT64_C(1) << 32, 0, 0 } };
	w.phase = "test-color-blobs";
	if (!smoke && (wb_blob(w.c.fd, &ctm, sizeof(ctm), &w.ctm) ||
		       wb_gamma_make(&w, &w.gamma[0], false) ||
		       wb_gamma_make(&w, &w.gamma[1], true)))
		goto fail;
	if (wb_mode_buffers(&w))
		goto fail;
	for (unsigned int mode_index = 0; mode_index < mode_count;
	     mode_index++) {
		if (mode_index &&
		    wb_next_mode(&w, modes[mode_index], mode_index))
			goto fail;
		w.phase = "initial-TEST_ONLY";
		if (!smoke &&
		    (wb_test(&w, !clone, 'A', w.target[0].fb.fb_id, 0) ||
		     wb_test(&w, !clone, 'A', w.invalid.fb.fb_id, EINVAL) ||
		     wb_test(&w, !clone, 'A', w.alias, EINVAL)))
			goto fail;
		if (clone) {
			const char stages[] = "AATOB";
			unsigned int count = smoke ? smoke : sizeof(stages) - 1;
			for (unsigned int i = 0; i < count; i++) {
				char name[96];
				snprintf(name, sizeof(name),
					 "m%u-%s-clone-%u-%c", mode_index,
					 w.mode_name, i, stages[i]);
				if (cp_stop) {
					errno = EINTR;
					goto fail;
				}
				if (wb_capture(&w, false, stages[i], i % 2,
					       name) ||
				    wb_finish_transition(&w))
					goto fail;
			}
		}
		if (headless) {
			const char stages[] = "AATOB";
			unsigned int count = smoke ? smoke : sizeof(stages) - 1;
			for (unsigned int i = 0; i < count; i++) {
				char name[96];
				snprintf(name, sizeof(name),
					 "m%u-%s-headless-%u-%c", mode_index,
					 w.mode_name, i, stages[i]);
				if (cp_stop) {
					errno = EINTR;
					goto fail;
				}
				if (wb_capture(&w, true, stages[i], (i + 1) % 2,
					       name) ||
				    wb_finish_transition(&w))
					goto fail;
			}
		}
	}
	goto cleanup;
fail:
	failure = errno ? errno : EIO;
	fprintf(stderr,
		"TEST_FAILURE phase=%s errno=%d reason=%s cases=%u changed=%u WB_pending=%u\n",
		w.phase ? w.phase : "unspecified", failure, strerror(failure),
		w.cases, w.changed, w.pending);
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
		if (w.alias && call(w.c.fd, DRM_IOCTL_MODE_RMFB, &w.alias)) {
			resources_released = false;
			if (!failure)
				failure = errno;
		}
		struct wb_buffer *buffers[] = { &w.source, &w.target[0],
						&w.target[1], &w.invalid,
						&w.previous_source };
		for (unsigned int i = 0; i < ARRAY_SIZE(buffers); i++)
			if (wb_destroy_buffer(&w, buffers[i])) {
				resources_released = false;
				if (!failure)
					failure = errno;
			}
		uint32_t blobs[] = {
			w.c.mode_blob,	  w.c.restore_blob,    w.ctm,
			w.gamma[0],	  w.gamma[1],	       w.original_ctm,
			w.original_gamma, w.previous_mode_blob
		};
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
	printf("RESULT_JSON {\"status\":\"%s\",\"captures\":%u,\"TEST_ONLY\":%u,\"source_immutable\":%s,\"standard_writeback\":true,\"probe_only\":%s,\"restore_required\":%s,\"restore_verified\":%s,\"resources_released\":%s,\"capture_format\":\"%s\",\"capture_component_bits\":%u,\"source_rgb_bits\":8,\"physical_sink_validated\":false}\n",
	       failure ? "FAIL" : "PASS", w.cases, w.tests,
	       source_ok ? "true" : "false", w.probe ? "true" : "false",
	       restore_required ? "true" : "false",
	       restore_verified ? "true" : "false",
	       resources_released ? "true" : "false",
	       wb_format_name(w.capture_format), wb_component_bits(w.capture_format));
	return failure ? 1 : 0;
}
