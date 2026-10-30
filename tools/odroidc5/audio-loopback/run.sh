#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
# Play a tagged pattern on one PCM and capture it from the TDMIN_LB loopback.
#
# usage: run.sh PLAY_PCM CAPTURE_PCM [channels] [format] [rate] [seconds] [valid_bits]
#   e.g. run.sh hw:0,0 hw:0,4 8 S32_LE 48000 3 24
set -eu
play=$1
capture=$2
channels=${3:-2}
format=${4:-S32_LE}
rate=${5:-48000}
seconds=${6:-3}
valid=${7:-}
here=$(dirname "$0")
work=${WORK:-$(mktemp -d /var/tmp/c5-loopback.XXXXXX)}
mkdir -p "$work"
frames=$((rate * seconds))

python3 "$here/loopback.py" generate "$work/pattern.raw" \
	--format "$format" --channels "$channels" --frames "$frames"
arecord -q -D "$capture" -t raw -f "$format" -c "$channels" -r "$rate" \
	-d $((seconds + 1)) "$work/capture.raw" &
rec=$!
sleep 0.3
aplay -q -D "$play" -t raw -f "$format" -c "$channels" -r "$rate" "$work/pattern.raw"
wait $rec
set -- --format "$format" --channels "$channels" --frames "$frames"
[ -n "$valid" ] && set -- "$@" --valid-bits "$valid"
python3 "$here/loopback.py" analyze "$work/capture.raw" "$@" > "$work/report.json" && rc=0 || rc=$?
cat "$work/report.json"
echo "work dir: $work"
exit $rc
