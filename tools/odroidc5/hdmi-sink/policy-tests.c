// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
static unsigned cases;
static const struct drm_display_mode modes[] = {
	{ 25175, 640, 656, 752, 800, 480, 490, 492, 525, 10, 0, 0, 1 },
	{ 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 5, 0, 0, 16 },
	{ 297000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 5, 0, 0, 63 },
	{ 241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, 9, 0, 0, 0 },
	{ 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 5, 0, 0, 95 },
	{ 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 5, 0, 0, 97 },
	{ 593407, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 5, 0, 0, 97 },
	{ 594000, 4096, 4184, 4272, 4400, 2160, 2168, 2178, 2250, 5, 0, 0,
	  102 },
};
static const char *enc_names[] = { "RGB8",	 "601-Y4448",  "709-Y4448",
				   "RGB10",	 "RGB12",      "601-Y44410",
				   "709-Y44410", "601-Y44412", "709-Y44412",
				   "601-Y4228",	 "709-Y4228",  "601-Y4208",
				   "709-Y4208",	 "601-Y42010", "709-Y42010",
				   "601-Y42012", "709-Y42012" };
/* Strict fixture envelope, not a replacement for the kernel EDID parser.
 * Actual HDMI/HF/HDR/Y420 DB parser bodies are compiled above from the tree.
 * No CTA iterator allocation/lifetime or mode-list code is being tested here. */
static struct drm_connector load_sink(const char *dir, const char *name)
{
	u8 e[256];
	char file[1024];
	struct drm_connector c = { 0 };
	assert(snprintf(file, sizeof(file), "%s/%s.bin", dir, name) <
	       (int)sizeof(file));
	FILE *f = fopen(file, "rb");
	assert(f);
	assert(fread(e, 1, sizeof(e), f) == sizeof(e));
	assert(fgetc(f) == EOF);
	fclose(f);
	assert(!memcmp(e, "\000\377\377\377\377\377\377\000", 8));
	for (unsigned b = 0; b < 2; b++) {
		unsigned sum = 0;
		for (unsigned i = 0; i < 128; i++)
			sum += e[b * 128 + i];
		assert(!(sum & 255));
	}
	assert(e[126] == 1 && e[128] == 2 && e[129] == 3 && e[130] >= 4 &&
	       e[130] < 127);
	c.display_info.color_formats = DRM_COLOR_FORMAT_RGB444;
	if (e[131] & 0x20)
		c.display_info.color_formats |= DRM_COLOR_FORMAT_YCBCR444;
	if (e[131] & 0x10)
		c.display_info.color_formats |= DRM_COLOR_FORMAT_YCBCR422;
	for (unsigned i = 132; i < 128U + e[130];) {
		const u8 *p = e + i;
		unsigned len = p[0] & 31, tag = p[0] >> 5;
		assert(len && i + 1 + len <= 128U + e[130]);
		if (tag == 3) {
			assert(len >= 3);
			if (p[1] == 3 && p[2] == 12 && p[3] == 0) {
				assert(len >= 7);
				drm_parse_hdmi_vsdb_video(&c, p);
			} else if (p[1] == 0xd8 && p[2] == 0x5d &&
				   p[3] == 0xc4) {
				assert(len >= 7);
				drm_parse_hdmi_forum_scds(&c, p);
			} else
				assert(!"Unknown vendor block in strict fixture");
		} else if (tag == 7) {
			if (p[1] == 6) {
				assert(len == 3);
				drm_parse_hdr_metadata_block(&c, p);
			}
			if (p[1] == 14) {
				assert(len >= 2);
				parse_cta_y420vdb(&c, (const struct cea_db *)p);
			}
		}
		i += len + 1;
	}
	assert(c.display_info.is_hdmi);
	printf("SINK %-25s max_tmds_khz=%d dc_rgb=0x%02x dc_y444=0x%02x formats=0x%x scdc=%u scramble=%u hdr_eotf=0x%x\n",
	       name, c.display_info.max_tmds_clock,
	       c.display_info.edid_hdmi_rgb444_dc_modes,
	       c.display_info.edid_hdmi_ycbcr444_dc_modes,
	       c.display_info.color_formats, c.display_info.hdmi.scdc.supported,
	       c.display_info.hdmi.scdc.scrambling.supported,
	       c.hdr_sink_metadata.hdmi_type1.eotf);
	return c;
}
/* hdr: 0 without HDR_OUTPUT_METADATA, else the static type 1 EOTF plus one. */
#define SDR_MD 1
#define PQ 3
#define HLG 4
static void check(const char *name, struct drm_connector *c,
		  struct drm_display_mode m, unsigned colorspace, unsigned bpc,
		  unsigned range, unsigned hdr, enum drm_mode_status mode_expected,
		  int expected_encoding)
{
	struct hdr_output_metadata metadata = {
		.metadata_type = HDMI_STATIC_METADATA_TYPE1,
		.hdmi_metadata_type1 = { .eotf = hdr ? hdr - 1 : 0 },
	};
	struct drm_property_blob blob = { &metadata, sizeof(metadata) };
	struct drm_connector_state conn = { .connector = c,
					    .colorspace = colorspace,
					    .max_bpc = bpc,
					    .max_requested_bpc = bpc,
					    .hdmi = { range },
					    .hdr_output_metadata =
						    hdr ? &blob : NULL };
	bool bt2020 = colorspace == DRM_MODE_COLORIMETRY_BT2020_RGB ||
		colorspace == DRM_MODE_COLORIMETRY_BT2020_YCC;
	struct s7d_hdmi_output_state output = { 0 };
	struct s7d_hdmi_state bridge = { 0 };
	struct s7d_crtc_state crtc = { 0 };
	memset(&last_hdr, 0xff, sizeof(last_hdr));
	enum drm_mode_status valid =
		s7d_hdmi_mode_valid(NULL, &c->display_info, &m);
	int select = s7d_hdmi_select_output(&conn, &m, &output);
	bridge.base.input_bus_cfg.format = select ? MEDIA_BUS_FMT_RGB888_1X24 :
						    output.bus_format;
	bridge.base.output_bus_cfg.format = MEDIA_BUS_FMT_FIXED;
	crtc.base.active = true;
	crtc.base.adjusted_mode = m;
	int result =
		s7d_hdmi_atomic_check(NULL, &bridge.base, &crtc.base, &conn);
	if (valid != mode_expected ||
	    (expected_encoding < 0 ? result == 0 : result != 0)) {
		fprintf(stderr,
			"%s failed: mode %d expected %d; select %d; check %d; encoding %d expected %d\n",
			name, valid, mode_expected, select, result,
			output.encoding, expected_encoding);
		abort();
	}
	if (expected_encoding >= 0) {
		assert(!select && bridge.valid &&
		       bridge.output.encoding == (unsigned)expected_encoding);
		assert(s7d_hdmi_output_same(&bridge.output, &crtc.output));
		assert(!s7d_hdmi_output_check(&bridge.output,
					      (unsigned long)m.clock * 1000,
					      24000000, NULL));
		if (s7d_hdmi_output_is_y420(&bridge.output)) {
			assert(last_avi.colorspace == HDMI_COLORSPACE_YUV420 &&
			       bridge.output.pixel_divider == 2 &&
			       bridge.output.map_post == 4 &&
			       (bridge.output.fmt & 3) == 2);
			assert(colorspace || last_avi.colorimetry ==
			       (s7d_hdmi_y420_709(bridge.output.encoding) ?
				HDMI_COLORIMETRY_ITU_709 : HDMI_COLORIMETRY_ITU_601));
		}
		/* BT.2020 requests carry extended colorimetry on every encoding. */
		assert(bridge.output.bt2020 == bt2020);
		assert(!bt2020 || (last_avi.colorimetry == HDMI_COLORIMETRY_EXTENDED &&
				   last_avi.extended_colorimetry ==
				   HDMI_EXTENDED_COLORIMETRY_BT2020));
		/* DVI sinks get no InfoFrames; HDMI sinks get the metadata as given. */
		assert(hdr && c->display_info.is_hdmi ?
		       bridge.hdr_len == sizeof(bridge.hdr) && last_hdr.eotf == hdr - 1 :
		       !bridge.hdr_len);
		struct s7d_encp_state encp;
		assert(s7d_encp_build_state(&m, &encp) == MODE_OK);
		assert(encp.regs[7].value - encp.regs[6].value ==
		       (u32)m.hdisplay);
		assert(encp.regs[9].value - encp.regs[8].value ==
		       (u32)m.vdisplay);
		printf("PASS %-33s %4dx%4d@%3d %s bpc=%u TMDS=%.6fMHz high=%u limited=%u bt2020=%u hdr=%u\n",
		       name, m.hdisplay, m.vdisplay, drm_mode_vrefresh(&m),
		       enc_names[bridge.output.encoding], bridge.output.bpc,
		       (double)bridge.output.rate.tmds_numerator /
			       bridge.output.rate.tmds_denominator / 1e6,
		       bridge.output.high_tmds, bridge.output.rgb_limited,
		       bridge.output.bt2020, bridge.hdr_len ? last_hdr.eotf : 0);
	} else {
		assert(!bridge.valid);
		printf("PASS %-33s rejected: mode=%d atomic=%d\n", name, valid,
		       result);
	}
	cases++;
}
#define TEST(n, c, m, cs, b, r, h, e) check(n, c, m, cs, b, r, h, MODE_OK, e)
/* Limited RGB keeps its range in the gamma LUT, so a LUT leaves the plan unchanged. */
static void check_gamma_range(struct drm_connector *c, struct drm_display_mode m)
{
	static struct drm_property_blob lut;
	struct drm_connector_state conn = { .connector = c, .max_bpc = 8,
					    .max_requested_bpc = 8 };
	struct s7d_hdmi_state bridge = { 0 }, plain = { 0 };
	struct s7d_crtc_state crtc = { 0 };

	bridge.base.input_bus_cfg.format = MEDIA_BUS_FMT_RGB888_1X24;
	bridge.base.output_bus_cfg.format = MEDIA_BUS_FMT_FIXED;
	plain.base = bridge.base;
	crtc.base.active = true;
	crtc.base.adjusted_mode = m;
	assert(!s7d_hdmi_atomic_check(NULL, &plain.base, &crtc.base, &conn));
	assert(plain.output.rgb_limited && !plain.output.post2.enabled);
	crtc.base.gamma_lut = &lut;
	assert(!s7d_hdmi_atomic_check(NULL, &bridge.base, &crtc.base, &conn));
	assert(s7d_hdmi_output_same(&bridge.output, &plain.output));
	assert(!s7d_hdmi_output_check(&bridge.output, (unsigned long)m.clock * 1000,
				      24000000, NULL));
	printf("PASS %-33s limited=%u POST2=%u plan unchanged by LUT\n",
	       "CTA limited RGB with gamma LUT", plain.output.rgb_limited,
	       plain.output.post2.enabled);
	cases++;
}
static s16 coefficient(u32 word, bool high)
{
	s16 value = (high ? word >> 16 : word) & 0x1fff;

	return value & BIT(12) ? value - (s16)BIT(13) : value;
}
/*
 * Every YCbCr encoding converts in the HDMI formatter after the RGB-domain
 * POST2 and gamma stages; BT.2020 NCL Q10 limited rows keep the BT.709 Y gain
 * and give zero-sum chroma rows.
 */
static void check_bt2020_matrix(void)
{
	struct s7d_hdmi_output_state y8, y10, y709;
	unsigned e, i;
	s16 f[9];

	for (e = S7D_HDMI_OUTPUT_RGB8; e <= S7D_HDMI_OUTPUT_709_Y42012; e++) {
		bool rgb = e == S7D_HDMI_OUTPUT_RGB8 || e == S7D_HDMI_OUTPUT_RGB10 ||
			e == S7D_HDMI_OUTPUT_RGB12;

		assert(!s7d_hdmi_output_build(e, 148500000, 24000000, &y8));
		assert(!y8.post2.enabled && y8.map_pre == 1 && !!(y8.fmt & BIT(31)) == !rgb);
	}
	assert(!s7d_hdmi_output_build(S7D_HDMI_OUTPUT_709_Y4448, 148500000, 24000000, &y8));
	assert(!s7d_hdmi_output_build(S7D_HDMI_OUTPUT_709_Y44410, 148500000, 24000000, &y10));
	y709 = y10;
	assert(!memcmp(y8.matrix, y10.matrix, sizeof(y8.matrix)));
	assert(!s7d_hdmi_output_apply_bt2020(&y8, true) && y8.bt2020);
	assert(!s7d_hdmi_output_apply_bt2020(&y10, true) && y10.bt2020);
	assert(!memcmp(y8.matrix, y10.matrix, sizeof(y8.matrix)));
	assert(!s7d_hdmi_output_same(&y10, &y709));
	assert(s7d_hdmi_output_apply_bt2020(&y709, false) == 0 && !y709.bt2020);
	/* Pairs high/low; the ninth coefficient is alone in the low half. */
	for (i = 0; i < 9; i++)
		f[i] = coefficient(y10.matrix[2 + i / 2], i < 8 && !(i & 1));
	/* Rows Cr/Y/Cb. */
	assert(f[3] + f[4] + f[5] == 879 && f[6] + f[7] + f[8] == 0 && f[0] + f[1] + f[2] == 0);
	assert(f[3] == 231 && f[4] == 596 && f[5] == 52 && f[8] == 450 && f[0] == 450);
	assert(!memcmp(y10.matrix + 7, (u32[]){ 0x02000040, 0x00000200 }, 8));
	assert(s7d_hdmi_output_apply_bt2020(&(struct s7d_hdmi_output_state){
		.encoding = S7D_HDMI_OUTPUT_601_Y44410 }, true) == -EINVAL);
	printf("PASS %-33s Y=%d,%d,%d Cb=%d,%d,%d Cr=%d,%d,%d\n", "Formatter CSC, BT.2020 NCL",
	       f[3], f[4], f[5], f[6], f[7], f[8], f[0], f[1], f[2]);
	cases++;
}
static int claim_all(const enum s7d_display_fetch *fetch)
{
	struct s7d_display_resources r = {0};
	unsigned int i;
	int ret;

	for (i = 0; i < S7D_INPUT_COUNT; i++) {
		if (fetch[i] == (enum s7d_display_fetch)-1)
			continue;
		ret = s7d_display_claim(&r, i, fetch[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static void check_compressed_resources(void)
{
	const enum s7d_display_fetch none = (enum s7d_display_fetch)-1;
	const struct {
		const char *name;
		enum s7d_display_fetch fetch[S7D_INPUT_COUNT];
		int ret;
	} t[] = {
		{ "Mali S0+S1", { S7D_FETCH_AFBC, S7D_FETCH_AFBC, none, none }, 0 },
		{ "GFCD OSD1+Mali S1", { S7D_FETCH_GFCD, S7D_FETCH_AFBC, none, none }, 0 },
		{ "GFCD on OSD2", { S7D_FETCH_LINEAR, S7D_FETCH_GFCD, none, none }, -EINVAL },
		{ "GFCD twice", { S7D_FETCH_GFCD, S7D_FETCH_GFCD, none, none }, -EINVAL },
		{ "Mali S0+VD1", { S7D_FETCH_AFBC, none, S7D_FETCH_LINEAR, none }, 0 },
		{ "Mali+VD2", { S7D_FETCH_AFBC, none, none, S7D_FETCH_LINEAR }, -EINVAL },
		{ "GFCD+VD1", { S7D_FETCH_GFCD, none, S7D_FETCH_LINEAR, none }, -EINVAL },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t); i++) {
		int ret = claim_all(t[i].fetch);

		if (ret != t[i].ret) {
			fprintf(stderr, "%s: claim %d, expected %d\n", t[i].name, ret, t[i].ret);
			abort();
		}
		cases++;
	}
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	struct drm_connector h14 = load_sink(argv[1], "hdmi14-4k30");
	struct drm_connector h20 = load_sink(argv[1], "hdmi20-4k-rgb444");
	struct drm_connector y422 = load_sink(argv[1], "hdmi20-4k-y422");
	struct drm_connector hdr = load_sink(argv[1], "hdmi20-4k-hdr");
	struct drm_connector y420 = load_sink(argv[1], "hdmi20-4k60-y420-only");
	assert(h14.display_info.max_tmds_clock == 300000 &&
	       !h14.display_info.hdmi.scdc.supported);
	assert(h20.display_info.max_tmds_clock == 600000 &&
	       h20.display_info.bpc == 12);
	assert(h20.display_info.edid_hdmi_rgb444_dc_modes == 0x30 &&
	       h20.display_info.edid_hdmi_ycbcr444_dc_modes == 0x30);
	assert(!(y422.display_info.color_formats & DRM_COLOR_FORMAT_YCBCR444));
	assert(hdr.hdr_sink_metadata.hdmi_type1.eotf == 0x0d &&
	       hdr.hdr_sink_metadata.hdmi_type1.metadata_type == 1);
	TEST("1080p RGB12", &h20, modes[1], 0, 12, 1, 0, S7D_HDMI_OUTPUT_RGB12);
	TEST("1080p RGB10", &h20, modes[1], 0, 10, 1, 0, S7D_HDMI_OUTPUT_RGB10);
	TEST("1080p RGB8", &h20, modes[1], 0, 8, 1, 0, S7D_HDMI_OUTPUT_RGB8);
	TEST("1080p120 RGB12", &h20, modes[2], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB12);
	TEST("1440p60 RGB12", &h20, modes[3], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB12);
	TEST("4K30 RGB12", &h20, modes[4], 0, 12, 1, 0, S7D_HDMI_OUTPUT_RGB12);
	TEST("4K30 RGB10", &h20, modes[4], 0, 10, 1, 0, S7D_HDMI_OUTPUT_RGB10);
	TEST("4K60 RGB max12 falls to8", &h20, modes[5], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("4K59.94 RGB max12 falls to8", &h20, modes[6], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("4096x2160p60 RGB8", &h20, modes[7], 0, 8, 1, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("CTA Auto limited RGB12", &h20, modes[4], 0, 12, 0, 0,
	     S7D_HDMI_OUTPUT_RGB12);
	TEST("Limited RGB10", &h20, modes[1], 0, 10, 2, 0,
	     S7D_HDMI_OUTPUT_RGB10);
	check_gamma_range(&h20, modes[1]);
	TEST("1440p Auto full RGB12", &h20, modes[3], 0, 12, 0, 0,
	     S7D_HDMI_OUTPUT_RGB12);
	TEST("VGA forbids deep RGB", &h20, modes[0], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("4K30 709 Y44412", &h20, modes[4], 2, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y44412);
	TEST("4K30 709 Y44410", &h20, modes[4], 2, 10, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y44410);
	TEST("4K30 601 Y44412", &h20, modes[4], 1, 12, 0, 0,
	     S7D_HDMI_OUTPUT_601_Y44412);
	TEST("4K60 Y444 max12 falls to8", &h20, modes[5], 2, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4448);
	TEST("4K60 709 Y422 request12 selects8", &y422, modes[5], 2, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4228);
	TEST("4K60 601 Y422 request12 selects8", &y422, modes[5], 1, 12, 0, 0,
	     S7D_HDMI_OUTPUT_601_Y4228);
	TEST("Y422 request10 selects8", &y422, modes[4], 2, 10, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4228);
	TEST("VGA Y422 request12 selects8", &y422, modes[0], 2, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4228);
	TEST("Y422 sink RGB remains8", &y422, modes[4], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("300MHz sink 4K30 RGB fallback", &h14, modes[4], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("300MHz sink 4K30 Y444 fallback", &h14, modes[4], 2, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4448);
	check("300MHz sink rejects4K60", &h14, modes[5], 0, 12, 1, 0,
	      MODE_CLOCK_HIGH, -1);
	TEST("HDR EDID allows SDR RGB12", &hdr, modes[4], 0, 12, 1, 0,
	     S7D_HDMI_OUTPUT_RGB12);
	check_bt2020_matrix();
	check_compressed_resources();
	TEST("HDR10 4K30 BT2020 full RGB10", &hdr, modes[4], DRM_MODE_COLORIMETRY_BT2020_RGB,
	     10, 1, PQ, S7D_HDMI_OUTPUT_RGB10);
	TEST("HDR10 4K30 BT2020 Auto RGB12", &hdr, modes[4], DRM_MODE_COLORIMETRY_BT2020_RGB,
	     12, 0, PQ, S7D_HDMI_OUTPUT_RGB12);
	TEST("HDR10 4K30 BT2020 Y44412", &hdr, modes[4], DRM_MODE_COLORIMETRY_BT2020_YCC,
	     12, 0, PQ, S7D_HDMI_OUTPUT_709_Y44412);
	TEST("HLG 1080p BT2020 Y44410", &hdr, modes[1], DRM_MODE_COLORIMETRY_BT2020_YCC,
	     10, 0, HLG, S7D_HDMI_OUTPUT_709_Y44410);
	TEST("HDR10 4K60 BT2020 Y4448", &hdr, modes[5], DRM_MODE_COLORIMETRY_BT2020_YCC,
	     12, 0, PQ, S7D_HDMI_OUTPUT_709_Y4448);
	TEST("HDR10 4K60 RGB without 420 stays8", &hdr, modes[5],
	     DRM_MODE_COLORIMETRY_BT2020_RGB, 12, 0, PQ, S7D_HDMI_OUTPUT_RGB8);
	TEST("SDR EOTF metadata on BT709 RGB", &hdr, modes[4], 0, 12, 1, SDR_MD,
	     S7D_HDMI_OUTPUT_RGB12);
	TEST("BT2020 RGB without metadata", &hdr, modes[1], DRM_MODE_COLORIMETRY_BT2020_RGB,
	     8, 1, 0, S7D_HDMI_OUTPUT_RGB8);
	TEST("HDR on SDR-only sink still sent", &h20, modes[4], DRM_MODE_COLORIMETRY_BT2020_RGB,
	     10, 1, PQ, S7D_HDMI_OUTPUT_RGB10);
	struct drm_connector pq = hdr;
	pq.hdr_sink_metadata.hdmi_type1.eotf = BIT(HDMI_EOTF_TRADITIONAL_GAMMA_SDR) |
		BIT(HDMI_EOTF_SMPTE_ST2084);
	TEST("HLG on PQ-only sink still sent", &pq, modes[4], DRM_MODE_COLORIMETRY_BT2020_RGB,
	     10, 1, HLG, S7D_HDMI_OUTPUT_RGB10);
	struct drm_connector hdr420 = pq;
	hdr420.display_info.hdmi.y420_cmdb_modes[97 / (8 * sizeof(long))] |=
		1UL << (97 % (8 * sizeof(long)));
	hdr420.display_info.hdmi.y420_dc_modes = DRM_EDID_YCBCR420_DC_36 |
						 DRM_EDID_YCBCR420_DC_30;
	TEST("HDR10 4K60 RGB prefers Y42012", &hdr420, modes[5],
	     DRM_MODE_COLORIMETRY_BT2020_RGB, 12, 0, PQ, S7D_HDMI_OUTPUT_709_Y42012);
	TEST("HDR10 4K60 max10 Y42010", &hdr420, modes[5],
	     DRM_MODE_COLORIMETRY_BT2020_YCC, 10, 0, PQ, S7D_HDMI_OUTPUT_709_Y42010);
	TEST("HDR10 4K60 max8 stays RGB8", &hdr420, modes[5],
	     DRM_MODE_COLORIMETRY_BT2020_RGB, 8, 0, PQ, S7D_HDMI_OUTPUT_RGB8);
	TEST("SDR 4K60 420-also stays RGB8", &hdr420, modes[5], 0, 12, 0, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("SDR-EOTF metadata 420-also stays RGB8", &hdr420, modes[5], 0, 12, 0, SDR_MD,
	     S7D_HDMI_OUTPUT_RGB8);
	TEST("420-only VIC97 RGB intent Y4208", &y420, modes[5], 0, 8, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4208);
	TEST("420-only VIC97 601 intent", &y420, modes[5], 1, 8, 0, 0,
	     S7D_HDMI_OUTPUT_601_Y4208);
	TEST("420-only no DC420 max12 stays8", &y420, modes[5], 0, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4208);
	struct drm_connector y420dc = y420;
	y420dc.display_info.hdmi.y420_dc_modes = DRM_EDID_YCBCR420_DC_36 |
						 DRM_EDID_YCBCR420_DC_30;
	TEST("420-only DC36 max12 Y42012", &y420dc, modes[5], 0, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y42012);
	TEST("420-only DC30 max10 Y42010", &y420dc, modes[5], 0, 10, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y42010);
	struct drm_connector y420also = h14;
	y420also.display_info.hdmi.y420_cmdb_modes[97 / (8 * sizeof(long))] |=
		1UL << (97 % (8 * sizeof(long)));
	TEST("300MHz 420-also 4K60 falls to Y4208", &y420also, modes[5], 0, 12, 0, 0,
	     S7D_HDMI_OUTPUT_709_Y4208);
	TEST("300MHz 420-also 4K30 keeps RGB", &y420also, modes[4], 0, 12, 0, 0,
	     S7D_HDMI_OUTPUT_RGB8);
	struct drm_connector hdrdvi = hdr;
	hdrdvi.display_info.is_hdmi = false;
	check("HDR metadata on DVI ignored", &hdrdvi, modes[1], 0, 8, 0, PQ, MODE_OK,
	      S7D_HDMI_OUTPUT_RGB8);
	struct drm_connector y420dvi = y420;
	y420dvi.display_info.is_hdmi = false;
	check("DVI 420-only rejected", &y420dvi, modes[5], 0, 8, 0, 0,
	      MODE_NO_420, -1);
	struct drm_connector missing = h20;
	missing.display_info.hdmi.scdc.supported = false;
	check("No SCDC rejects4K60", &missing, modes[5], 0, 8, 1, 0,
	      MODE_CLOCK_HIGH, -1);
	missing = h20;
	missing.display_info.hdmi.scdc.scrambling.supported = false;
	check("No scramble rejects4K60", &missing, modes[5], 0, 8, 1, 0,
	      MODE_CLOCK_HIGH, -1);
	missing = h20;
	missing.display_info.color_formats = DRM_COLOR_FORMAT_RGB444;
	TEST("RGB-only rejectsYCC intent", &missing, modes[1], 2, 8, 0, 0, -1);
	struct drm_display_mode bad = modes[5];
	bad.hdisplay = 4097;
	bad.hsync_start = 4200;
	check("Width4097 rejected", &h20, bad, 0, 8, 1, 0, MODE_CLOCK_HIGH, -1);
	bad = modes[5];
	bad.vdisplay = 2161;
	check("Height2161 rejected", &h20, bad, 0, 8, 1, 0, MODE_CLOCK_HIGH,
	      -1);
	bad = modes[5];
	bad.clock = 594001;
	check("Pixel594001 rejected", &h20, bad, 0, 8, 1, 0, MODE_CLOCK_HIGH,
	      -1);
	bad = modes[3];
	bad.clock *= 2;
	check("1440p120 rejected", &h20, bad, 0, 8, 1, 0, MODE_VSYNC, -1);
	bad = modes[1];
	bad.flags |= DRM_MODE_FLAG_INTERLACE;
	check("Interlace rejected", &h20, bad, 0, 8, 1, 0, MODE_NO_INTERLACE,
	      -1);
	printf("PASS: %u production-C policy/timing cases; no MMIO, board, link, packet or capture validation\n",
	       cases);
	return 0;
}
