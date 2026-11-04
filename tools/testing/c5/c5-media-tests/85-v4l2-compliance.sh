#!/bin/bash
# 85-v4l2-compliance.sh: v4l2-compliance on the decoder, the JPEG encoder and both GE2D nodes (no streaming
# tests). Known (2026-11-03): decoder 45/48 (NM12 accepted but not listed, control event), JPEG 48/48,
# GE2D 48/48 (2026-10-29, before d2a9b6bf4). Full output of each node in the log.
NAME=85-v4l2-compliance; DESC="v4l2-compliance: decoder, JPEG encoder, GE2D single- and multi-planar nodes"
. "$(dirname "$0")/lib/common.sh"
. "$LIB/nodes.sh"
sysinfo
modprobe s7d_ge2d 2>/dev/null; sleep 1
say "v4l2-compliance: $(v4l2-compliance --version 2>&1 | head -1)"
R=
for n in "S7D Video Decoder" "S7D JPEG Encoder" meson-ge2d meson-ge2d-mplane; do
	dev=$(node "$n")
	if [ -z "$dev" ]; then R="$R $n: no node;"; say "$n: no node"; continue; fi
	v4l2-compliance -d "$dev" > "$TMP/c.log" 2>&1
	{ echo "=== $n ($dev)"; cat "$TMP/c.log"; } >> "$LOG.client"
	t=$(grep -a '^Total for' "$TMP/c.log" | tail -1 |
		sed -E 's/.*: ([0-9]+), Succeeded: ([0-9]+), Failed: ([0-9]+), Warnings: ([0-9]+).*/\2\/\1 (failed \3, warnings \4)/')
	say "$n ($dev): ${t:-no total}"
	grep -a 'fail:' "$TMP/c.log" | sed 's/^[[:space:]]*/  /' | tee -a "$LOG"
	R="$R $n ${t%% *};"
done
report "${R# }"
