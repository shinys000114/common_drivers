#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Generate and check per-channel PCM patterns for the S7D TDMIN_LB loopback.

The playback pattern gives every sample a channel tag and a frame counter, so a
loopback capture can be compared bit for bit: channel order, dropped or
repeated frames, and the number of significant bits that survived the path.
Files are raw little-endian interleaved PCM, as used by aplay/arecord -t raw.
"""

import argparse
import json
import struct
import sys

FORMATS = {
    'S16_LE': (2, '<h', 16),
    'S24_LE': (4, '<i', 24),  # 24 bits in the low bits of a 32-bit container
    'S32_LE': (4, '<i', 32),
}


def word(channel, frame, bits):
    """Sample value for a channel and frame.

    The top 16 bits always survive the narrowest (16-bit) path: a 4-bit channel
    tag and a 12-bit frame counter. Lower bits carry a hash of the channel and
    frame, so a wider path can be checked bit for bit.
    """
    n = frame & 0xfff
    top = ((channel & 0xf) << 12) | n
    low_bits = bits - 16
    value = top << low_bits
    if low_bits:
        h = ((channel + 1) * 0x9e3779b1 ^ n * 0x85ebca6b) & 0xffffffff
        value |= h >> (32 - low_bits)
    if value >= 1 << (bits - 1):
        value -= 1 << bits
    return value


def generate(path, fmt, channels, frames):
    size, code, bits = FORMATS[fmt]
    pack = struct.Struct(code).pack
    with open(path, 'wb') as f:
        for n in range(frames):
            f.write(b''.join(pack(word(c, n, bits)) for c in range(channels)))


def read_frames(path, fmt, channels):
    size, code, _ = FORMATS[fmt]
    data = open(path, 'rb').read()
    frame_bytes = size * channels
    count = len(data) // frame_bytes
    unpack = struct.Struct('<' + code[1] * channels).unpack_from
    return [unpack(data, i * frame_bytes) for i in range(count)]


def unsigned(value, bits):
    return value & ((1 << bits) - 1)


def analyze(path, fmt, channels, play_channels, valid_bits, played=None):
    """Compare a capture with the pattern and return a JSON-ready report.

    valid_bits is the number of leading bits expected to survive (for example
    24 when a 32-bit pattern passes a 24-bit path). The capture may start with
    silence and at any pattern frame. With played (the pattern length), every
    pattern frame from the first one captured must be present and the capture
    must be silent after the last one.
    """
    _, _, bits = FORMATS[fmt]
    mask = ((1 << valid_bits) - 1) << (bits - valid_bits)
    frames = read_frames(path, fmt, channels)
    report = {'frames': len(frames), 'format': fmt, 'channels': channels,
              'valid_bits': valid_bits}
    start = next((i for i, f in enumerate(frames) if any(f)), None)
    if start is None:
        report['result'] = 'silent'
        return report
    low_bits = bits - 16
    top = [unsigned(v, bits) >> low_bits for v in frames[start]]
    report['leading_silence'] = start
    report['start_counter'] = top[0] & 0xfff

    # Map each captured channel to the playback channel whose tag it carries.
    mapping = []
    for c in range(channels):
        tag = top[c] >> 12
        mapping.append(tag if tag < play_channels else None)
    report['channel_map'] = mapping

    errors = []
    first = top[0] & 0xfff
    compared = 0
    for i in range(start, len(frames)):
        n = first + i - start
        ended = played is not None and n >= played
        compared += not ended
        for c in range(channels):
            src = mapping[c]
            if src is None or ended:
                want = 0
            else:
                want = unsigned(word(src, n, bits), bits) & mask
            got = unsigned(frames[i][c], bits) & mask
            if got != want:
                errors.append({'frame': i, 'channel': c,
                               'want': hex(want), 'got': hex(got)})
                if len(errors) >= 16:
                    break
        if len(errors) >= 16:
            break
    report['errors'] = errors
    report['compared'] = compared
    complete = played is None or first + compared == played
    if not complete:
        report['truncated'] = played - first - compared
    identity = mapping[:play_channels] == list(range(play_channels))
    report['result'] = 'pass' if not errors and identity and complete else 'fail'
    return report


def selftest():
    import os
    import tempfile

    def capture(fmt, channels, frames, silence=0, start=0, swap=None,
                drop_bits=0, missing=None, gap=None, tail=0):
        size, code, bits = FORMATS[fmt]
        pack = struct.Struct(code).pack
        out = bytearray(b'\0' * size * channels * silence)
        for n in range(start, start + frames):
            if gap is not None and n == start + gap:
                continue
            vals = [word(c, n, bits) for c in range(channels)]
            if swap:
                a, b = swap
                vals[a], vals[b] = vals[b], vals[a]
            if missing is not None:
                vals[missing] = 0
            if drop_bits:
                vals = [v & ~((1 << drop_bits) - 1) for v in vals]
            out += b''.join(pack(v) for v in vals)
        out += b'\0' * size * channels * tail
        return bytes(out)

    cases = [
        ('S32_LE', 8, dict(), 32, 'pass'),
        ('S32_LE', 8, dict(silence=37, start=1000), 32, 'pass'),
        ('S32_LE', 2, dict(drop_bits=8), 24, 'pass'),
        ('S32_LE', 2, dict(drop_bits=8, start=12345, silence=5), 24, 'pass'),
        ('S32_LE', 2, dict(drop_bits=16, start=999), 16, 'pass'),
        ('S32_LE', 2, dict(drop_bits=8), 32, 'fail'),
        ('S16_LE', 2, dict(start=70000), 16, 'pass'),
        ('S32_LE', 6, dict(swap=(2, 3)), 32, 'fail'),
        ('S32_LE', 8, dict(missing=7), 32, 'fail'),
        ('S32_LE', 2, dict(drop_bits=9, start=300), 24, 'fail'),
        ('S16_LE', 2, dict(gap=100), 16, 'fail'),
        ('S24_LE', 8, dict(), 24, 'pass'),
        ('S32_LE', 2, dict(silence=40, tail=60), 32, 'pass', 512),
        ('S32_LE', 2, dict(start=100, tail=60), 32, 'pass', 612),
        ('S32_LE', 2, dict(silence=40), 32, 'fail', 600),
        ('S32_LE', 2, dict(tail=60), 32, 'fail', 400),
    ]
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for fmt, ch, kw, valid, want, *played in cases:
            path = os.path.join(tmp, 'cap.raw')
            with open(path, 'wb') as f:
                f.write(capture(fmt, ch, 512, **kw))
            got = analyze(path, fmt, ch, ch, valid, *played)['result']
            ok = got == want
            failed += not ok
            print('%s %s ch=%d %s valid=%d played=%s -> %s' %
                  ('ok  ' if ok else 'FAIL', fmt, ch, kw, valid,
                   played[0] if played else '-', got))
        gen = os.path.join(tmp, 'gen.raw')
        generate(gen, 'S32_LE', 8, 300)
        got = analyze(gen, 'S32_LE', 8, 8, 32)['result']
        failed += got != 'pass'
        print('%s generated pattern -> %s' % ('ok  ' if got == 'pass' else 'FAIL', got))
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('generate')
    g.add_argument('output')
    a = sub.add_parser('analyze')
    a.add_argument('capture')
    a.add_argument('--play-channels', type=int)
    a.add_argument('--valid-bits', type=int)
    for s in (g, a):
        s.add_argument('--format', choices=FORMATS, default='S32_LE')
        s.add_argument('--channels', type=int, default=2)
    g.add_argument('--frames', type=int, default=48000 * 3)
    a.add_argument('--frames', type=int, help='number of frames played')
    sub.add_parser('selftest')
    args = p.parse_args()
    if args.cmd == 'selftest':
        return selftest()
    if args.cmd == 'generate':
        generate(args.output, args.format, args.channels, args.frames)
        return 0
    report = analyze(args.capture, args.format, args.channels,
                     args.play_channels or args.channels,
                     args.valid_bits or FORMATS[args.format][2], args.frames)
    print(json.dumps(report, indent=1))
    return 0 if report['result'] == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())
