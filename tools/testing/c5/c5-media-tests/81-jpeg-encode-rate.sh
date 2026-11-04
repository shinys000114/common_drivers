#!/bin/bash
# 81-jpeg-encode-rate.sh: JPEG encoder rate, unpaced: 600 videotestsrc frames, 1920x1080 NV12, quality 75, from
# system memory (copied into MMAP buffers) and from GE2D capture buffers imported as dma-bufs. The audit's
# measurement for ENC-3 (staging and firmware kept for the session).
NAME=81-jpeg-encode-rate; DESC="JPEG encoder rate, 600 frames 1080p NV12, MMAP and dma-buf from GE2D"
. "$(dirname "$0")/lib/common.sh"
sysinfo
modprobe s7d_ge2d 2>/dev/null
gst_registry_check
measure_bg
rate() { # LABEL ELEMENTS...: frames/s of the whole pipeline
	local l=$1 t0 t1; shift
	t0=$(date +%s.%N)
	as_user timeout 300 gst-launch-1.0 -q videotestsrc num-buffers=600 ! video/x-raw,format=NV12,width=1920,height=1080 ! \
		"$@" ! fakesink sync=false > "$TMP/$l.log" 2>&1
	local rc=$?
	t1=$(date +%s.%N)
	cat "$TMP/$l.log" >> "$LOG.client"
	[ $rc = 0 ] && awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", 600 / (b - a)}' || echo "failed($rc)"
}
A=$(rate mmap v4l2jpegenc)
B=$(rate dmabuf v4l2convert capture-io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080 ! \
	v4l2jpegenc output-io-mode=dmabuf-import)
say "mmap: $A fps, GE2D dma-buf: $B fps"
report "MMAP $A fps, GE2D dma-buf $B fps"
