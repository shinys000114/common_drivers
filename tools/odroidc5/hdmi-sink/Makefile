# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
PYTHON ?= python3
CC ?= cc
src := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
KERNEL_SRC ?= $(abspath $(src)/../../../..)
O ?= /tmp/odroidc5-hdmi-sink

.PHONY: all check guards fixtures
all: check
check:
	$(PYTHON) $(src)/run.py --source-root "$(KERNEL_SRC)" --out "$(O)" --cc "$(CC)"
guards:
	$(PYTHON) $(src)/test_runner_guards.py --source-root "$(KERNEL_SRC)"
fixtures:
	$(PYTHON) $(src)/make_edids.py --out "$(O)/edid"
	$(PYTHON) $(src)/make_rgb10_pattern.py --out "$(O)/patterns"
