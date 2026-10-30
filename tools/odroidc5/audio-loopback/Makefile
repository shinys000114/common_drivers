# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
PYTHON ?= python3
src := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

.PHONY: all check
all:
check:
	$(PYTHON) $(src)/loopback.py selftest
	sh -n $(src)/run.sh
	sh -n $(src)/route.sh
