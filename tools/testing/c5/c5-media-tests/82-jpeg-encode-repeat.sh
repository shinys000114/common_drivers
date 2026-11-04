#!/bin/bash
# 82-jpeg-encode-repeat.sh: JPEG encoder start/stop stress (like v4l2-20261029/vjrep.sh). Each round runs four
# short round trips (q75, q95, 642x482 q85, NV21 q60), each a full session start and EOS stop, then kills two
# encoders with SIGKILL while frames are in flight. JREP rounds (default 5; the audit asks for 20).
# Stopping HCODEC while its DMA runs once hung the board: keep the PowerMate at hand.
NAME=82-jpeg-encode-repeat; DESC="JPEG encoder start/stop and kill stress, JREP rounds"; CLIP=bbb-h264-1080p60.mp4
. "$(dirname "$0")/lib/common.sh"
sysinfo
# vjpeg.py runs as the desktop user and writes its results here
U=$TMP/u; mkdir -p "$U"; chown "$RUSER" "$U"
JREP=${JREP:-5}
gst_registry_check
ok=0; n=0; fails=
for i in $(seq 1 "$JREP"); do
	for t in "q75 10 75" "q95 10 95" "odd 10 85 --size=642x482" "nv21 10 60 --fmt=NV21"; do
		set -- $t; l=$1; shift
		echo "<6>c5-media-tests: $NAME round $i $l" > /dev/kmsg
		as_user timeout 150 python3 -W ignore "$LIB/vjpeg.py" "$MEDIA/$CLIP" "$U/r.json" "$@" > "$TMP/r.out" 2>&1
		n=$((n + 1))
		if python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1])).get("ok") else 1)' "$U/r.json" 2>/dev/null; then
			ok=$((ok + 1))
		else
			fails="$fails $i/$l"; say "round $i $l: $(tail -1 "$TMP/r.out")"
		fi
		rm -f "$U/r.json"
	done
	for k in 1 2; do
		as_user gst-launch-1.0 -q videotestsrc ! video/x-raw,format=NV12,width=1920,height=1080 ! v4l2jpegenc ! \
			fakesink sync=false > /dev/null 2>&1 &
		sleep 1.$((k * 3)); pkill -9 -f "v4l2jpegenc ! fakesink" ; wait 2>/dev/null
	done
done
# the encoder must still work after the kills
as_user timeout 150 python3 -W ignore "$LIB/vjpeg.py" "$MEDIA/$CLIP" "$U/r.json" 10 75 > "$TMP/r.out" 2>&1
say "after kills: $(tail -1 "$TMP/r.out")"
grep -q '"ok": true' "$U/r.json" 2>/dev/null && last=ok || last=FAIL
report "$ok/$n round trips ok${fails:+ (failed:$fails)}, after kills $last"
