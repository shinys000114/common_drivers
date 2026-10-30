#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Write real 10-bit input codes; no display/capture claim and no device access."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, default=Path(__file__).parent / 'patterns')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
width, height, pitch = 1024, 16, 4096
pixels = bytearray()
for y in range(height):
    for x in range(width):
        colors = [512, 512, 512]
        if y % 4 < 3:
            colors[y % 4] = x
        else:
            colors = [x, 1023 - x, x ^ 0x155]
        r, g, b = colors
        pixels += struct.pack('<I', (3 << 30) | (b << 20) | (g << 10) | r)
# Distinct lower two bits would collapse under truncation to RGB8.
for x in range(0, 1024, 4):
    values = [struct.unpack_from('<I', pixels, (x + i) * 4)[0] & 1023 for i in range(4)]
    assert values == list(range(x, x + 4))
    assert len(set(v >> 2 for v in values)) == 1
path = args.out / 'rgb10-lowbits-abgr2101010.raw'
path.write_bytes(pixels)
info = {'file': path.name, 'fourcc': 'ABGR2101010', 'modifier': 'LINEAR',
        'width': width, 'height': height, 'pitch_bytes': pitch,
        'packing': 'little endian u32 A[31:30] B[29:20] G[19:10] R[9:0]; alpha=3',
        'pattern': 'y%4=0:R ramp, 1:G ramp, 2:B ramp, 3:(x,1023-x,x^0x155)',
        'input_codes_per_ramp': 1024, 'sha256': hashlib.sha256(pixels).hexdigest(),
        'validation': 'input file packing only; production output/capture precision untested',
        'board_plan': 'Upload without 8-bit conversion or scaling; select Full RGB 10/12 transport. Capture only after a >=10-bit packing and tap are validated. Compare 4 adjacent codes sharing their upper 8 bits.'}
(args.out / 'manifest.json').write_text(json.dumps(info, indent=2) + '\n')
print(f'PASS: {len(pixels)} bytes of ABGR2101010 input; 1024 distinct codes per channel ramp, pitch aligned64; no hardware')
