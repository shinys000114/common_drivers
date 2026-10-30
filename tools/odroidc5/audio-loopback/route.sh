#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
# Restore the default mixer route of one output and point the TDMIN_LB
# loopback (hw:0,4) at its TDM output. The card applies the same routes
# from the device tree when it registers.
#
# usage: route.sh hdmi|spdif2hdmi|line|spdif|i2s
#   hdmi hw:0,0, spdif2hdmi hw:0,1 (no loopback), line hw:0,2,
#   spdif hw:0,3 (no loopback), i2s hw:0,2 with the i2s overlay
set -u
a() { amixer -c0 -q cset name="$1" "$2" || echo "FAIL $1=$2"; }
case ${1:-} in
hdmi)   a 'FRDDR_A SINK 1 SEL' 'OUT 1'; a 'FRDDR_A SRC 1 EN Switch' on; a 'TDMOUT_B SRC SEL' 'IN 0'
        a 'TOHDMITX I2S SRC' 'I2S B'; a 'TOHDMITX I2S OUT EN Switch' on; a 'TDMIN_LB SRC SEL' 'IN 13';;
spdif2hdmi)
        a 'FRDDR_B SINK 1 SEL' 'OUT 4'; a 'FRDDR_B SRC 1 EN Switch' on; a 'SPDIFOUT_B SRC SEL' 'IN 1'
        a 'TOHDMITX SPDIF SRC' 'SPDIF B'; a 'TOHDMITX SPDIF OUT EN Switch' on;;
line)   a 'FRDDR_C SINK 1 SEL' 'OUT 2'; a 'FRDDR_C SRC 1 EN Switch' on; a 'TDMOUT_C SRC SEL' 'IN 2'
        a 'TOACODEC SRC' 'I2S C'; a 'TOACODEC OUT EN Switch' on; a 'TDMIN_LB SRC SEL' 'IN 14';;
spdif)  a 'FRDDR_D SINK 1 SEL' 'OUT 3'; a 'FRDDR_D SRC 1 EN Switch' on; a 'SPDIFOUT_A SRC SEL' 'IN 3';;
i2s)    a 'FRDDR_C SINK 2 SEL' 'OUT 0'; a 'FRDDR_C SRC 2 EN Switch' on; a 'TDMOUT_A SRC SEL' 'IN 2'
        a 'TDMIN_LB SRC SEL' 'IN 12';;
*)      echo 'usage: route.sh hdmi|spdif2hdmi|line|spdif|i2s' >&2; exit 1;;
esac
a 'TODDR_A SRC SEL' 'IN 6'
