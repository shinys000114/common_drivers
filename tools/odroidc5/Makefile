# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
CC ?= cc
CROSS_COMPILE ?= aarch64-linux-gnu-
CROSS_CC ?= $(CROSS_COMPILE)gcc
PYTHON ?= python3
src := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
KERNEL_SRC ?= $(abspath $(src)/../../..)
O ?= /tmp/odroidc5-tools
out := $(abspath $(O))

.PHONY: all writeback packing check check-hdmi fixtures
all: writeback packing
writeback packing:
	$(MAKE) -C "$(src)/$@" KERNEL_SRC="$(KERNEL_SRC)" O="$(out)/$@" CC="$(CC)" CROSS_CC="$(CROSS_CC)" all
check:
	$(MAKE) -C "$(src)/writeback" KERNEL_SRC="$(KERNEL_SRC)" O="$(out)/writeback" CC="$(CC)" check
	$(MAKE) -C "$(src)/packing" KERNEL_SRC="$(KERNEL_SRC)" O="$(out)/packing" CC="$(CC)" PYTHON="$(PYTHON)" check
	$(MAKE) -C "$(src)/hdmi-sink" KERNEL_SRC="$(KERNEL_SRC)" O="$(out)/hdmi-sink" CC="$(CC)" PYTHON="$(PYTHON)" check
	$(MAKE) -C "$(src)/afbc-engine" KERNEL_SRC="$(KERNEL_SRC)" O="$(out)/afbc-engine" CC="$(CC)" check
	$(MAKE) -C "$(src)/audio-loopback" PYTHON="$(PYTHON)" check
check-hdmi:
	$(MAKE) -C "$(src)/hdmi-sink" KERNEL_SRC="$(KERNEL_SRC)" O="$(out)/hdmi-sink" CC="$(CC)" PYTHON="$(PYTHON)" check guards
fixtures:
	$(MAKE) -C "$(src)/hdmi-sink" O="$(out)/hdmi-sink" PYTHON="$(PYTHON)" fixtures
