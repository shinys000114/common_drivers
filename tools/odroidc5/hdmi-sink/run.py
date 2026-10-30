#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Compile actual C policy, timing and DDC code against private host fixtures."""
import sys
sys.dont_write_bytecode = True
import argparse
import hashlib
import json
import os
import shlex
import re
import subprocess
from pathlib import Path
from make_edids import generate
from run_state import checked_paths, new_attempt

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, required=True, help='Kernel source root containing common_drivers')
parser.add_argument('--out', type=Path, required=True, help='Output directory outside the kernel source tree')
parser.add_argument('--cc', default=os.environ.get('CC', 'cc'), help='Host compiler command; default CC or cc')
args = parser.parse_args()
cc = shlex.split(args.cc)
if not cc:
    parser.error('--cc must name a host compiler')
try:
    root, results_root = checked_paths(args.source_root, args.out)
except ValueError as error:
    parser.error(str(error))
with new_attempt(root, results_root) as attempt:
    out = attempt.directory
    src = root / 'common_drivers/drivers/drm/s7d'
    manifest = {'run_id': attempt.run_id, 'kind': 'source/model only; no hardware access', 'source_root': str(root), 'files': {}}

    def source(path):
        data = path.read_bytes()
        try:
            key = str(path.relative_to(root))
        except ValueError:
            key = str(path)
        manifest['files'][key] = hashlib.sha256(data).hexdigest()
        return data.decode()

    def function(text, name, path):
        # Locate a definition; skip quoted strings and comments when balancing braces.
        match = re.search(r'(?m)^[\w \t*\n]+?\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', text)
        if not match:
            raise RuntimeError(f'Function missing or changed: {name} in {path}')
        start, opening = match.start(), match.end() - 1
        depth = 0
        for token in re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[opening:]):
            if token.group() == '{': depth += 1
            elif token.group() == '}':
                depth -= 1
                if depth == 0:
                    end = opening + token.end()
                    line = text.count('\n', 0, start) + 1
                    return f'\n#line {line} "{path}"\n' + text[start:end] + '\n'
        raise RuntimeError(f'Unbalanced function: {name}')

    headers = {
    'linux/types.h': '''#pragma once
    #include <stdint.h>
    #include <stdbool.h>
    typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
    typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
    ''',
    'linux/errno.h': '#pragma once\n#define EINVAL 22\n#define ERANGE 34\n#define ENOSPC 28\n',
    'linux/bits.h': '#pragma once\n#define BIT(n) (1U << (n))\n#define BIT_ULL(n) (1ULL << (n))\n#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31 - (h))))\n',
    'linux/bitops.h': '#include <linux/bits.h>\nstatic inline unsigned hweight8(unsigned v) { return __builtin_popcount(v & 255); }\n',
    'linux/kernel.h': '''#pragma once
    #include <stddef.h>
    #include <linux/types.h>
    #define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
    #define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
    #define check_mul_overflow(a,b,p) __builtin_mul_overflow(a,b,p)
    #define DIV_ROUND_CLOSEST_ULL(n,d) (((n)+(d)/2)/(d))
    static inline u64 mul_u32_u32(u32 a, u32 b) { return (u64)a*b; }
    ''',
    'linux/limits.h': '#pragma once\n#include <stdint.h>\n#define U32_MAX UINT32_MAX\n',
    'linux/string.h': '#include <string.h>\n',
    'linux/math64.h': '''#pragma once
    #include <linux/types.h>
    static inline u64 div_u64(u64 n, u32 d) { return n/d; }
    static inline s64 div64_s64(s64 n, s64 d) { return n/d; }
    ''',
    'uapi/drm/drm_mode.h': '#pragma once\n#include <linux/types.h>\nstruct drm_color_ctm { u64 matrix[9]; };\n',
    'drm/drm_modes.h': '''#pragma once
    #include <linux/types.h>
    #define DRM_MODE_FLAG_PHSYNC (1U<<0)
    #define DRM_MODE_FLAG_NHSYNC (1U<<1)
    #define DRM_MODE_FLAG_PVSYNC (1U<<2)
    #define DRM_MODE_FLAG_NVSYNC (1U<<3)
    #define DRM_MODE_FLAG_INTERLACE (1U<<4)
    #define DRM_MODE_FLAG_DBLSCAN (1U<<5)
    #define DRM_MODE_FLAG_DBLCLK (1U<<12)
    #define DRM_MODE_FLAG_3D_MASK (0x1fU<<14)
    enum drm_mode_status { MODE_OK, MODE_NO_INTERLACE, MODE_NO_DBLESCAN,
    MODE_NO_VSCAN, MODE_NO_STEREO, MODE_BAD, MODE_CLOCK_LOW, MODE_CLOCK_HIGH,
    MODE_BAD_HVALUE, MODE_BAD_VVALUE, MODE_HBLANK_NARROW, MODE_VBLANK_NARROW,
    MODE_ERROR, MODE_CLOCK_RANGE, MODE_NO_420, MODE_VSYNC };
    struct drm_display_mode { int clock,hdisplay,hsync_start,hsync_end,htotal;
    int vdisplay,vsync_start,vsync_end,vtotal; unsigned flags; int vscan,hskew;
    u8 fixture_vic; };
    int drm_mode_vrefresh(const struct drm_display_mode *mode);
    ''',
    }
    headers['linux/media-bus-format.h'] = source(root / 'include/uapi/linux/media-bus-format.h')
    headers['linux/amlogic/clk/s7d-hdmi-pll-rate.h'] = source(root / 'common_drivers/include/linux/amlogic/clk/s7d-hdmi-pll-rate.h')
    for name, text in headers.items():
        target = out / 'include' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    for name in ['s7d-hdmi-output.c', 's7d-hdmi-output.h', 's7d-encp.c', 's7d-encp.h', 's7d-caps.c', 's7d-caps.h']:
        (out / name).write_text(source(src / name))
    hdmi = source(src / 's7d-hdmi.c')
    edid_path = root / 'drivers/gpu/drm/drm_edid.c'
    modes_path = root / 'drivers/gpu/drm/drm_modes.c'
    edid = source(edid_path)
    modes = source(modes_path)
    code = edid[:edid.index('#include')] + modes[:modes.index('#include')] + (HERE / 'policy-preamble.c').read_text()
    for name in ['drm_mode_vrefresh', 'drm_mode_is_420_only', 'drm_mode_is_420_also',
                 'drm_mode_is_420']:
        code += function(modes, name, modes_path)
    for name in ['drm_default_rgb_quant_range', 'drm_get_max_frl_rate',
                 'drm_parse_ycbcr420_deep_color_info', 'drm_parse_hdmi_forum_scds',
                 'drm_parse_hdmi_deep_color_info', 'drm_parse_hdmi_vsdb_video',
                 'eotf_supported', 'hdr_metadata_type', 'drm_parse_hdr_metadata_block',
                 'parse_cta_y420vdb']:
        code += function(edid, name, edid_path)
    code += hdmi[hdmi.index('struct s7d_hdmi_state {'):hdmi.index('struct s7d_hdmi {')]
    code += function(hdmi, 'to_s7d_hdmi_state', src / 's7d-hdmi.c')
    code += '\nstatic bool s7d_hdmi_link_same(const struct s7d_hdmi_state *a, const struct s7d_hdmi_state *b) { (void)a; (void)b; assert(!"Old bridge states are outside scope"); return false; }\n'
    for name in ['s7d_hdmi_y420_709', 's7d_hdmi_link_valid', 's7d_hdmi_mode_valid',
                 's7d_hdmi_requested_output', 's7d_hdmi_requested_sampling',
                 's7d_hdmi_bt2020', 's7d_hdmi_hdr_eotf',
                 's7d_hdmi_rgb_limited', 's7d_hdmi_sink_accepts_output', 's7d_hdmi_select_y420',
                 '__s7d_hdmi_select_output', 's7d_hdmi_select_output',
                 's7d_hdmi_atomic_check']:
        code += function(hdmi, name, src / 's7d-hdmi.c')
    code += '\n#line 1 "policy-tests.c"\n' + (HERE / 'policy-tests.c').read_text()
    (out / 'policy.c').write_text(code)
    compiler = cc + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
                '-fsanitize=undefined', '-fno-sanitize-recover=all']
    cmd = compiler + ['-I' + str(out / 'include'), '-I' + str(out), str(out / 'policy.c'),
                      str(out / 's7d-hdmi-output.c'), str(out / 's7d-encp.c'),
                      str(out / 's7d-caps.c'), '-o', str(out / 'policy')]
    subprocess.run(cmd, check=True)
    generate(out / 'edid')
    res = subprocess.run([str(out / 'policy'), str(out / 'edid')], check=True, text=True,
                         stdout=subprocess.PIPE)
    (out / 'policy.log').write_text(res.stdout)
    print(res.stdout, end='')
    from build_ddc import run as run_ddc
    ddc_log = run_ddc(HERE, root, out, source, function, cc)
    summary = {'validation': 'source/model only', 'policy_cases': int(re.search(r'PASS: (\d+) production-C', res.stdout).group(1)), 'sink_cases': int(re.search(r'PASS: (\d+) synthetic', ddc_log).group(1)), 'edid_profiles': 5, 'policy_log': res.stdout.splitlines(), 'ddc_log': ddc_log.splitlines(), 'board_access': False, 'production_changes': False}
    subprocess.run([sys.executable, str(HERE / 'make_rgb10_pattern.py'), '--out', str(out / 'patterns')], check=True)
    manifest['compiler'] = subprocess.check_output(cc + ['--version'], text=True).splitlines()[0]
    manifest['compile_command'] = cmd
    manifest['repository_heads'] = {str(p.relative_to(root)) or '.': subprocess.check_output(['git', '-C', str(p), 'rev-parse', 'HEAD'], text=True).strip() for p in [root, root / 'common_drivers']}
    (out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    attempt.summary = summary
