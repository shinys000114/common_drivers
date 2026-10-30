#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Create synthetic sink capability fixtures, never install them on hardware."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

PROFILES = {
    'hdmi14-4k30': dict(hf=False, y444=True, deep=True, hdr=False, y420=False),
    'hdmi20-4k-rgb444': dict(hf=True, y444=True, deep=True, hdr=False, y420=False),
    'hdmi20-4k-y422': dict(hf=True, y444=False, deep=False, hdr=False, y420=False),
    'hdmi20-4k-hdr': dict(hf=True, y444=True, deep=True, hdr=True, y420=False),
    'hdmi20-4k60-y420-only': dict(hf=True, y444=True, deep=True, hdr=False, y420=True),
}

def checksum(block):
    block[127] = (-sum(block[:127])) & 255
    return block

def descriptor(tag, value=b''):
    return bytes([0, 0, 0, tag, 0]) + value.ljust(13, b' ')[:13]

def dtd_1080p60():
    return bytes.fromhex('02 3a 80 18 71 38 2d 40 58 2c 45 00 58 54 21 00 00 1e')

def block(tag, payload):
    assert 0 < len(payload) <= 31
    return bytes([(tag << 5) | len(payload)]) + bytes(payload)

def make(profile):
    p = PROFILES[profile]
    base = bytearray(128)
    base[:8] = b'\x00\xff\xff\xff\xff\xff\xff\x00'
    base[8:10] = ((8 << 10) | (4 << 5) | 3).to_bytes(2, 'big') # HDC synthetic
    base[10:12] = (0xC500 + list(PROFILES).index(profile)).to_bytes(2, 'little')
    base[12:16] = (1).to_bytes(4, 'little')
    base[16:25] = bytes([1, 36, 1, 3, 0x80, 60, 34, 120, 0x0E])
    base[25:35] = bytes.fromhex('ee 91 a3 54 4c 99 26 0f 50 54')
    base[35] = 0x20 # 640x480p60 established timing, mandatory HDMI baseline
    base[38:54] = b'\x01\x01' * 8
    base[54:72] = dtd_1080p60()
    base[72:90] = descriptor(0xFC, b'C5 TEST SINK\n')
    base[90:108] = descriptor(0xFD, bytes([24, 120, 15, 160, 60, 0, 10]) + b'      ')
    base[108:126] = bytes([0, 0, 0, 0x10, 0]) + bytes(13)
    base[126] = 1
    cta = bytearray(128)
    cta[:4] = bytes([2, 3, 0, 0x91 | (0x20 if p['y444'] else 0)])
    vics = [16, 1, 93, 94, 95]
    if p['hf'] and not p['y420']:
        vics += [96, 97, 98, 102]
    blocks = block(2, vics)
    # HDMI VSDB: OUI 000c03, address 1.0.0.0, DC_36/DC_30/DC_Y444,
    # 300 MHz maximum in HDMI VSDB; HF-VSDB raises it to 600 MHz.
    blocks += block(3, [3, 12, 0, 0x10, 0, 0x38 if p['deep'] else 0, 60])
    if p['hf']:
        # Version 1, 600 MHz, SCDC present, no FRL/DSC/420 deep color claimed.
        blocks += block(3, [0xD8, 0x5D, 0xC4, 1, 120, 0x80, 0])
    # Video Capability DB: selectable RGB/YCC quantization, all scan types support.
    blocks += block(7, [0, 0xCA])
    if p['hdr']:
        blocks += block(7, [5, 0xE0, 0]) # BT.2020 cYCC/YCC/RGB
        blocks += block(7, [6, 0x0D, 1]) # SDR, PQ, HLG, static metadata type 1
    if p['y420']:
        blocks += block(7, [0x0E, 97]) # VIC97 only in 4:2:0
    cta[2] = 4 + len(blocks)
    cta[4:4 + len(blocks)] = blocks
    cta[cta[2]:cta[2] + 18] = dtd_1080p60()
    return bytes(checksum(base) + checksum(cta))

def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for name, properties in PROFILES.items():
        data = make(name)
        path = out / (name + '.bin')
        path.write_bytes(data)
        decoded = subprocess.run(['edid-decode', '--check', str(path)], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name + '.txt')).write_text(decoded.stdout)
        manifest[name] = dict(properties, sha256=hashlib.sha256(data).hexdigest(),
                              decoder_exit=decoded.returncode)
        print(f'{name}: 256 bytes, edid-decode exit {decoded.returncode}')
        if decoded.returncode:
            print(decoded.stdout)
            raise RuntimeError(f'Invalid fixture: {name}')
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path(__file__).parent / 'edid')
    generate(parser.parse_args().out)
