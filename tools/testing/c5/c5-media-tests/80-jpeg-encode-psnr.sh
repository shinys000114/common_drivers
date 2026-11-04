#!/bin/bash
# 80-jpeg-encode-psnr.sh: S7D JPEG encoder (v4l2jpegenc) round trip. Frames decoded in software are encoded in
# hardware, decoded back with jpegdec and compared per plane (lib/vjpeg.py): 1080p at quality 75, 50 and 95,
# NV21, an odd 642x482 size, and 1080p frames that GE2D hands over as dma-bufs (encoder reads them in place).
# Pass: every frame round-trips, Y >= 38 dB at quality 75 (2026-10-30: Y 38.5 / UV 41.2 dB, about 495 KB).
NAME=80-jpeg-encode-psnr; DESC="JPEG encoder PSNR: 1080p q75/q50/q95, NV21, 642x482, dma-buf from GE2D"; CLIP=bbb-h264-1080p60.mp4
. "$(dirname "$0")/lib/common.sh"
sysinfo
# vjpeg.py runs as the desktop user and writes its results here
U=$TMP/u; mkdir -p "$U"; chown "$RUSER" "$U"
FRAMES=${FRAMES:-30}
modprobe s7d_ge2d 2>/dev/null
gst_registry_check
measure_bg
enc() { # LABEL ARGS...
	local l=$1; shift
	as_user timeout 300 python3 -W ignore "$LIB/vjpeg.py" "$MEDIA/$CLIP" "$U/$l.json" "$FRAMES" "$@" > "$TMP/$l.out" 2>&1
	say "$l: $(tail -1 "$TMP/$l.out")"
	cat "$TMP/$l.out" >> "$LOG.client"
}
enc q75 75; enc q50 50; enc q95 95; enc nv21 75 --fmt=NV21; enc odd 85 --size=642x482; enc ge2d 75 --via=ge2d
R=$(python3 - "$U" <<'PY'
import json, sys
out, bad = [], []
for l in ('q75', 'q50', 'q95', 'nv21', 'odd', 'ge2d'):
    try:
        r = json.load(open('%s/%s.json' % (sys.argv[1], l)))
    except Exception:
        bad.append(l + ' no result'); continue
    if not r.get('ok'):
        bad.append('%s %s' % (l, (r.get('error') or 'frames failed')[:40]))
    elif l in ('q75', 'nv21', 'ge2d') and r['y_min'] < 38:
        bad.append('%s Y %.1f dB' % (l, r['y_min']))
    if 'y_min' in r:
        out.append('%s %.1f/%.1f' % (l, r['y_min'], r['uv_min']))
print(('FAIL: ' + ', '.join(bad) + '; ' if bad else 'pass: ') + 'Y/UV dB ' + ', '.join(out))
PY
)
report "$R"
