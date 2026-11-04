#!/bin/bash
# 58-ge2d-nv12m-import.sh: the multi-planar GE2D node reads NV12M (luma and chroma in two buffers). The same
# videotestsrc frames go NV12 -> BGRx through the single-planar node and NV12M -> BGRx through the multi-planar
# node; GStreamer picks NV12M there by itself. Both outputs must be identical, byte for byte.
NAME=58-ge2d-nv12m-import; DESC="GE2D multi-planar node: NV12M source gives the same BGRx as NV12 on the single-planar node"
. "$(dirname "$0")/lib/common.sh"
. "$LIB/nodes.sh"
sysinfo
# the player runs as the desktop user and writes its frames here
U=$TMP/u; mkdir -p "$U"; chown "$RUSER" "$U"
modprobe s7d_ge2d 2>/dev/null; sleep 1
gst_registry_check
SP=$(node meson-ge2d); MP=$(node meson-ge2d-mplane)
ESP=$(gst_element "$SP" convert); EMP=$(gst_element "$MP" convert)
say "nodes: single-planar $SP ($ESP), multi-planar $MP ($EMP)"
if [ -z "$ESP" ] || [ -z "$EMP" ]; then report "failed: GE2D node or GStreamer element missing"; exit 1; fi
conv() { # ELEMENT OUT: 30 SMPTE frames, 1280x720 NV12 in, BGRx out
	as_user env GST_DEBUG="v4l2*:5" GST_DEBUG_NO_COLOR=1 gst-launch-1.0 -q videotestsrc num-buffers=30 pattern=smpte ! \
		video/x-raw,format=NV12,width=1280,height=720 ! "$1" ! video/x-raw,format=BGRx,width=1280,height=720 ! \
		filesink location="$2" > "$2.log" 2>&1
}
conv "$ESP" "$U/sp.raw"; conv "$EMP" "$U/mp.raw"
cat "$U/sp.raw.log" "$U/mp.raw.log" > "$LOG.client"
A=$(md5sum < "$U/sp.raw" | cut -c1-12); B=$(md5sum < "$U/mp.raw" | cut -c1-12)
N=$(( $(stat -c %s "$U/mp.raw" 2>/dev/null || echo 0) / (1280 * 720 * 4) ))
grep -aq "Desired format.*NM12" "$U/mp.raw.log" && M=NM12 || M="NM12 not seen in the log"
say "single-planar $A, multi-planar $B ($N frames, source $M)"
if [ "$N" = 30 ] && [ "$A" = "$B" ]; then R="pass: $N frames identical, source $M"; else R="FAIL: $A vs $B, $N frames"; fi
report "$R"
