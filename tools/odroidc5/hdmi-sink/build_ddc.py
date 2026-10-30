# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Small adapter for the established C5 DDC fault harness."""
import subprocess
from pathlib import Path

def run(here, root, out, source, function, cc):
    src = root / 'common_drivers/drivers/drm/s7d'
    pre = source(here / 'fixtures/ddc-preamble.c')
    pre = pre.replace('#include <string.h>', '#include <string.h>\n#include <stdlib.h>\n#include <sys/types.h>')
    pre = pre.replace('static int regmap_read(', '''static bool sink_enabled, sink_absent;
static u8 sink_edid[256], sink_scdc[256];
static unsigned sink_offset, sink_address, sink_ops, sink_nack_op, sink_bad_config, sink_transaction_fault;
static int regmap_read(''')
    pre = pre.replace('*val = status_fault;', '*val = status_fault | sink_transaction_fault;')
    pre = pre.replace('*val = cursor++ & 0xff;', '''if (!sink_enabled) *val = cursor++ & 0xff;
        else { assert(sink_offset + cursor < 256);
            *val = sink_address == 0x50 ? sink_edid[sink_offset + cursor] : sink_scdc[sink_offset + cursor]; cursor++; }''')
    needle = 'transfer_len = m->regs[6] | (m->regs[7] << 8);'
    assert needle in pre
    pre = pre.replace(needle, needle + '''
            if (sink_enabled) {
                sink_address = m->regs[3] >> 1;
                sink_offset = m->regs[4] * 256 + m->regs[5];
                assert(sink_address == 0x50 || sink_address == 0x54);
                supplied = transfer_len; ++sink_ops;
                sink_transaction_fault = (sink_absent || sink_ops == sink_nack_op) ? 0x20 : 0;
                if (cmd == 6 && !sink_transaction_fault) {
                    assert(sink_address == 0x54 && sink_offset < 256);
                    sink_scdc[sink_offset] = m->regs[10];
                    if (sink_offset == 0x20) sink_scdc[sink_offset] ^= sink_bad_config;
                }
            }''')
    # Recovery must stop the transaction even if the synthetic sink NACKed it.
    pre = pre.replace('++recoveries;', '++recoveries; sink_transaction_fault = 0;')
    ddc = source(src / 's7d-hdmi-ddc.c')
    ddc = ddc[ddc.index('#define DDC_ADDR'):ddc.index('static u32 s7d_ddc_functionality')]
    io = source(src / 's7d-hdmi-io.c')
    code = pre + io[io.index('#define S7D_HDMI_READ'):io.index('static const struct regmap_bus')]
    code += io[io.index('int s7d_hdmi_update_checked('):] + ddc
    tests = source(here / 'fixtures/ddc-regression.c').replace('int main(void)', 'static int regression_main(void)')
    tests = tests.rstrip()[:-1] + 'return 0;\n}\n'
    code += tests
    code += '''
#define SCDC_I2C_SLAVE_ADDRESS 0x54
#define SCDC_SINK_VERSION 1
#define SCDC_SOURCE_VERSION 2
#define SCDC_TMDS_CONFIG 0x20
#define SCDC_SCRAMBLING_ENABLE 1
#define SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 2
#define GFP_KERNEL 0
#define kmalloc(n,flags) malloc(n)
#define kfree(p) free(p)
static int i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m, int n) { return s7d_ddc_xfer(a,m,n); }
#define dev_dbg(...) ((void)0)
struct s7d_hdmi { void *dev; struct s7d_hdmi_ddc ddc; };
struct s7d_hdmi_state { bool scdc, high_tmds; };
'''
    p = root / 'drivers/gpu/drm/display/drm_scdc_helper.c'
    scdc = source(p)
    code += scdc[:scdc.index('#include')]
    for name in ['drm_scdc_read', 'drm_scdc_write']:
        code += function(scdc, name, p)
    p = root / 'include/drm/display/drm_scdc_helper.h'
    scdc = source(p)
    for name in ['drm_scdc_readb', 'drm_scdc_writeb']:
        code += function(scdc, name, p)
    p = src / 's7d-hdmi.c'
    code += function(source(p), 's7d_hdmi_setup_scdc', p)
    code += '\n#line 1 "ddc-tests.c"\n' + (here / 'ddc-tests.c').read_text()
    (out / 'ddc.c').write_text(code)
    subprocess.run(cc + ['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-Wno-sign-compare','-fsanitize=undefined','-fno-sanitize-recover=all',
                    str(out / 'ddc.c'),'-o',str(out / 'ddc')], check=True)
    result = subprocess.run([str(out / 'ddc'), str(out / 'edid/hdmi20-4k-hdr.bin')],
                            check=True, text=True, stdout=subprocess.PIPE)
    (out / 'ddc.log').write_text(result.stdout)
    print(result.stdout, end='')
    return result.stdout
