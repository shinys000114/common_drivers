#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Compare actual C-generated packing fixtures with an independent reference."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def reference(bits, x, y):
    mask = (1 << bits) - 1
    value = x & mask
    if y < 128:
        return value, 1 << (bits - 4), 1 << (bits - 1)
    if y < 256:
        return 1 << (bits - 4), value, 1 << (bits - 1)
    if y < 384:
        return 1 << (bits - 4), 1 << (bits - 1), value
    if y < 512:
        return value, value, value
    if y < 640:
        return (x + 3 * y) & mask, (3 * x + 5 * y) & mask, (7 * x + 11 * y) & mask
    return mask if x & 1 else 0, mask if y & 1 else 0, mask if x & 32 else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-tool', type=Path, required=True)
    args = parser.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='odroidc5-packing-') as temporary:
        output = Path(temporary)
        subprocess.run([str(args.fixture_tool.resolve()), str(output)], check=True)
        for bits, name in [(8, 'XRGB8888'), (10, 'XBGR2101010')]:
            data = (output / (name + '.raw')).read_bytes()
            assert len(data) == 1920 * 720 * 4
            for index, (observed,) in enumerate(struct.iter_unpack('<I', data)):
                y, x = divmod(index, 1920)
                r, g, b = reference(bits, x, y)
                expected = (r << 16) | (g << 8) | b if bits == 8 else r | (g << 10) | (b << 20)
                assert observed == expected, (name, x, y, observed, expected)
                count += 1
    print(f'PASS actual_C_fixture_pixels={count} source_reference_only=1 device_access=0')


if __name__ == '__main__':
    main()
