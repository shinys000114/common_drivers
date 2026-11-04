#!/usr/bin/python3
"""vjpeg: encode decoded video frames with the S7D JPEG encoder (v4l2jpegenc) and check them.

usage: vjpeg.py CLIP OUT.json [FRAMES] [QUALITY] [--fmt=NV12|NV21] [--size=WxH] [--save=DIR]
                [--via=ge2d]

Frames are decoded in software to NV12 (or NV21), encoded by v4l2jpegenc, decoded back by
the software jpegdec and compared per plane with the source frame (PSNR in dB).
--via=ge2d passes the frames through GE2D (v4l2convert) first and has the encoder import
GE2D's capture buffers as dma-bufs; the reference is then GE2D's output.
(Copy of tools/testing/c5/v4l2-20261029/vjpeg.py with --via and an early copy of the
reference frames, so no capture buffer stays held.)
"""
import json, math, os, sys
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstApp', '1.0')
gi.require_version('GstVideo', '1.0')
from gi.repository import Gst, GstApp, GstVideo  # noqa: F401
import numpy as np

# decode the source in software: the DOS block runs one engine at a time, so a hardware
# decoder would keep the JPEG encoder (HCODEC) from starting
os.environ.setdefault('GST_PLUGIN_FEATURE_RANK',
                      'v4l2h264dec:NONE,v4l2h265dec:NONE,v4l2vp9dec:NONE,v4l2av1dec:NONE')
Gst.init(None)
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
clip, out = args[0], args[1]
frames = int(args[2]) if len(args) > 2 else 30
quality = int(args[3]) if len(args) > 3 else 75
fmt = opts.get('fmt', 'NV12')
size = opts.get('size')
scale = ''
if size:
    w, h = size.split('x')
    scale = ',width=%s,height=%s' % (w, h)
ge2d, enc_io = '', ''
if opts.get('via') == 'ge2d':
    ge2d = 'v4l2convert capture-io-mode=dmabuf ! video/x-raw,format=%s%s ! ' % (fmt, scale)
    enc_io = ' output-io-mode=dmabuf-import'


def planes(sample):
    """Y, U, V arrays (copies) of a raw sample (NV12/NV21/I420)."""
    buf = sample.get_buffer()
    info = GstVideo.VideoInfo.new_from_caps(sample.get_caps())
    meta = GstVideo.buffer_get_video_meta(buf)
    offs = meta.offset if meta else info.offset
    strd = meta.stride if meta else info.stride
    ok, m = buf.map(Gst.MapFlags.READ)
    d = np.frombuffer(m.data, dtype=np.uint8)
    w, h = info.width, info.height
    y = np.array([d[offs[0] + r * strd[0]:offs[0] + r * strd[0] + w] for r in range(h)])
    name = info.finfo.name
    if name in ('NV12', 'NV21'):
        uv = np.array([d[offs[1] + r * strd[1]:offs[1] + r * strd[1] + w] for r in range(h // 2)])
        u, v = (uv[:, 0::2], uv[:, 1::2]) if name == 'NV12' else (uv[:, 1::2], uv[:, 0::2])
    else:
        u = np.array([d[offs[1] + r * strd[1]:offs[1] + r * strd[1] + w // 2] for r in range(h // 2)])
        v = np.array([d[offs[2] + r * strd[2]:offs[2] + r * strd[2] + w // 2] for r in range(h // 2)])
    buf.unmap(m)
    return name, w, h, np.ascontiguousarray(y), np.ascontiguousarray(u), np.ascontiguousarray(v)


def psnr(a, b):
    mse = float(np.mean((a.astype(np.int32) - b.astype(np.int32)) ** 2))
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


demux = 'matroskademux' if clip.endswith(('.webm', '.mkv')) else 'qtdemux'
desc = ('filesrc location="%s" ! %s ! decodebin3 ! videoconvert ! videoscale ! '
        'video/x-raw,format=%s%s ! %sidentity eos-after=%d ! tee name=t '
        't. ! queue ! appsink name=raw sync=false max-buffers=1000 '
        't. ! queue ! v4l2jpegenc name=enc%s extra-controls="c,compression_quality=%d" ! '
        'image/jpeg ! appsink name=jpg sync=false max-buffers=1000'
        % (clip, demux, fmt, scale, ge2d, frames, enc_io, quality))
p = Gst.parse_launch(desc)
raw_sink, jpg_sink = p.get_by_name('raw'), p.get_by_name('jpg')
p.set_state(Gst.State.PLAYING)
raws, jpgs = [], []
while len(raws) < frames or len(jpgs) < frames:
    got = False
    if len(raws) < frames:
        s = raw_sink.try_pull_sample(5 * Gst.SECOND)
        if s:
            # copy now: a held sample would keep a GE2D capture buffer from its queue
            raws.append(planes(s))
            got = True
    if len(jpgs) < frames:
        s = jpg_sink.try_pull_sample(5 * Gst.SECOND)
        if s:
            jb = s.get_buffer()
            ok, m = jb.map(Gst.MapFlags.READ)
            jpgs.append(bytes(m.data))
            jb.unmap(m)
            got = True
    if not got:
        break
msg = p.get_bus().pop_filtered(Gst.MessageType.ERROR)
err = msg.parse_error()[0].message if msg else None
enc_dev = p.get_by_name('enc').get_property('device')
p.set_state(Gst.State.NULL)

dec = Gst.parse_launch('appsrc name=src caps=image/jpeg ! jpegdec ! videoconvert ! '
                       'video/x-raw,format=I420 ! appsink name=out sync=false')
src, dsink = dec.get_by_name('src'), dec.get_by_name('out')
dec.set_state(Gst.State.PLAYING)
res = {'clip': clip, 'frames': len(jpgs), 'raw_frames': len(raws), 'quality': quality,
       'format': fmt, 'via': opts.get('via'), 'device': enc_dev, 'error': err, 'per_frame': []}
for i, (r, data) in enumerate(zip(raws, jpgs)):
    if opts.get('save'):
        os.makedirs(opts['save'], exist_ok=True)
        open(os.path.join(opts['save'], 'f%03d.jpg' % i), 'wb').write(data)
    src.emit('push-buffer', Gst.Buffer.new_wrapped(data))
    s = dsink.try_pull_sample(5 * Gst.SECOND)
    if s is None:
        res['per_frame'].append({'i': i, 'bytes': len(data), 'decode': 'failed'})
        continue
    _, w, h, y0, u0, v0 = r
    _, w1, h1, y1, u1, v1 = planes(s)
    if (w, h) != (w1, h1):
        res['per_frame'].append({'i': i, 'bytes': len(data), 'size': [w1, h1]})
        continue
    res['per_frame'].append({'i': i, 'bytes': len(data), 'soi': data[:2] == b'\xff\xd8',
                             'eoi': data[-2:] == b'\xff\xd9',
                             'y': round(psnr(y0, y1), 2), 'u': round(psnr(u0, u1), 2),
                             'v': round(psnr(v0, v1), 2)})
dec.set_state(Gst.State.NULL)
good = [f for f in res['per_frame'] if 'y' in f]
# a source may end early (VP9 hidden frames): every frame it produced must round-trip
res['ok'] = (not err and bool(raws) and len(good) == len(raws) and
             all(f['soi'] and f['eoi'] for f in good))
if good:
    res['y_min'] = min(f['y'] for f in good)
    res['uv_min'] = min(min(f['u'], f['v']) for f in good)
    res['avg_bytes'] = int(sum(f['bytes'] for f in good) / len(good))
json.dump(res, open(out, 'w'))
print('jpeg %s q=%d %s%s frames=%d/%d ok=%s y_min=%s uv_min=%s avg_bytes=%s err=%s' % (
    fmt, quality, enc_dev, ' via ' + opts['via'] if opts.get('via') else '', len(good), frames,
    res['ok'], res.get('y_min'), res.get('uv_min'), res.get('avg_bytes'), err))
