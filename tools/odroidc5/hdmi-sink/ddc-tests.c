// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
static unsigned sink_cases;
static void sink_init(void)
{
	init(128);
	sink_enabled = true;
	sink_absent = false;
	sink_nack_op = sink_ops = sink_bad_config = sink_transaction_fault = 0;
	memset(sink_scdc, 0, sizeof(sink_scdc));
	sink_scdc[1] = 1;
}
static void sink_released(void)
{
	assert(!d.lock.held && !(map.regs[0x6f8] & 0x80) && !d.fault);
}
static void check_scdc(int expected, bool high)
{
	struct s7d_hdmi h = { .ddc = d };
	struct s7d_hdmi_state state = { .scdc = true, .high_tmds = high };
	h.ddc.adapter.data = &h.ddc;
	int ret = s7d_hdmi_setup_scdc(&h, &state);
	assert(ret == expected);
	assert(!h.ddc.lock.held && !(map.regs[0x6f8] & 0x80) && !h.ddc.fault);
	if (!ret && high) {
		assert(sink_ops == 4 && sink_scdc[2] == 1 && sink_scdc[0x20] == 3);
		/* All four transactions completed and the native delay ran afterward. */
		assert(now >= 9004);
	} else if (!ret) {
		/* Below 340 MHz: one best-effort clear, as dw-hdmi. */
		assert(sink_ops == 1 && (sink_nack_op == 1 || sink_scdc[0x20] == 0));
	}
	sink_cases++;
}
int main(int argc, char **argv)
{
	assert(argc == 2);
	regression_main();
	sink_init();
	FILE *f = fopen(argv[1], "rb");
	assert(f);
	assert(fread(sink_edid, 1, sizeof(sink_edid), f) == sizeof(sink_edid));
	fclose(f);
	u8 actual[256];
	for (unsigned block = 0; block < 2; block++) {
		u8 off = block * 128;
		struct i2c_msg messages[2] = { { 0x50, 0, 1, &off },
					       { 0x50, I2C_M_RD, 128,
						 actual + block * 128 } };
		assert(s7d_ddc_xfer(&d.adapter, messages, 2) == 2);
		sink_released();
	}
	assert(!memcmp(sink_edid, actual, sizeof(actual)));
	sink_cases++;
	sink_init();
	check_scdc(0, true);
	sink_init();
	sink_scdc[0x20] = 3;
	check_scdc(0, false);
	for (unsigned op = 1; op <= 4; op++) {
		sink_init();
		sink_nack_op = op;
		check_scdc(-ENXIO, true);
		assert(sink_ops == op);
	}
	sink_init();
	sink_scdc[0x20] = 3;
	sink_nack_op = 1;
	check_scdc(0, false);
	sink_init();
	sink_absent = true;
	check_scdc(-ENXIO, true);
	sink_init();
	sink_scdc[1] = 0;
	check_scdc(-EINVAL, true);
	sink_init();
	sink_bad_config = 1;
	check_scdc(-EIO, true);
	sink_init();
	sink_bad_config = 2;
	check_scdc(-EIO, true);
	sink_init();
	struct s7d_hdmi h = { .ddc = d };
	h.ddc.adapter.data = &h.ddc;
	struct s7d_hdmi_state state = { .scdc = false, .high_tmds = true };
	assert(s7d_hdmi_setup_scdc(&h, &state) == -EINVAL && !sink_ops);
	sink_cases++;
	state.high_tmds = false;
	assert(!s7d_hdmi_setup_scdc(&h, &state) && !sink_ops);
	sink_cases++;
	printf("PASS: %u synthetic EDID/SCDC scenarios execute native DDC and native SCDC setup; host-only sink model, no real sink success claimed\n",
	       sink_cases);
	return 0;
}
