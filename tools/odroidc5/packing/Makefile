# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
CC ?= cc
CROSS_COMPILE ?= aarch64-linux-gnu-
CROSS_CC ?= $(CROSS_COMPILE)gcc
PYTHON ?= python3
O ?= /tmp/odroidc5-packing
src := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
out := $(abspath $(O))
KERNEL_SRC ?= $(abspath $(src)/../../../..)
CFLAGS ?= -O2 -g
TOOL_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -ffunction-sections -fdata-sections
TOOL_CPPFLAGS := -D__user= -I$(out)/include
TOOL_LDFLAGS := -Wl,--gc-sections
DEPS := $(src)/packing-matrix.c $(src)/../common/c5-kms-compose.c $(src)/../common/c5-kms-base.c $(wildcard $(KERNEL_SRC)/include/uapi/drm/*.h)

.PHONY: all host arm64 check sanitize clean FORCE
all: host arm64
host: $(out)/packing-matrix-host $(out)/fixture-reference
arm64: $(out)/packing-matrix-aarch64
$(out):
	mkdir -p $@
$(out)/uapi-path: FORCE | $(out)
	@test -f "$(KERNEL_SRC)/include/uapi/drm/drm.h"
	@mkdir -p "$(out)/include"
	@if test "$$(readlink "$(out)/include/drm")" != "$(KERNEL_SRC)/include/uapi/drm"; then ln -sfnT "$(KERNEL_SRC)/include/uapi/drm" "$(out)/include/drm"; printf '%s\n' "$(KERNEL_SRC)" > "$@"; elif test ! -f "$@"; then printf '%s\n' "$(KERNEL_SRC)" > "$@"; fi
FORCE:

$(out)/packing-matrix-host: $(DEPS) $(out)/uapi-path | $(out)
	$(CC) $(CPPFLAGS) $(TOOL_CPPFLAGS) $(CFLAGS) $(TOOL_CFLAGS) $< $(LDFLAGS) $(TOOL_LDFLAGS) -o $@
$(out)/packing-matrix-aarch64: $(DEPS) $(out)/uapi-path | $(out)
	$(CROSS_CC) $(CPPFLAGS) $(TOOL_CPPFLAGS) $(CFLAGS) $(TOOL_CFLAGS) $< $(LDFLAGS) $(TOOL_LDFLAGS) -o $@
$(out)/fixture-reference: $(src)/fixture-reference.c $(DEPS) $(out)/uapi-path | $(out)
	$(CC) $(CPPFLAGS) $(TOOL_CPPFLAGS) $(CFLAGS) $(TOOL_CFLAGS) $< $(LDFLAGS) $(TOOL_LDFLAGS) -o $@
$(out)/packing-matrix-sanitized: $(DEPS) $(out)/uapi-path | $(out)
	$(CC) $(CPPFLAGS) $(TOOL_CPPFLAGS) -O1 -g $(TOOL_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie $< $(LDFLAGS) $(TOOL_LDFLAGS) -o $@
check: host
	$(out)/packing-matrix-host --selftest
	$(PYTHON) $(src)/fixture10.py --selftest
	$(PYTHON) $(src)/check.py --fixture-tool $(out)/fixture-reference
sanitize: $(out)/packing-matrix-sanitized
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(out)/packing-matrix-sanitized --selftest
clean:
	rm -f $(out)/packing-matrix-host $(out)/packing-matrix-aarch64 $(out)/packing-matrix-sanitized $(out)/fixture-reference
