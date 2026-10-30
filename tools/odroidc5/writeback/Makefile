# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
CC ?= cc
CROSS_COMPILE ?= aarch64-linux-gnu-
CROSS_CC ?= $(CROSS_COMPILE)gcc
O ?= /tmp/odroidc5-writeback
src := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
out := $(abspath $(O))
KERNEL_SRC ?= $(abspath $(src)/../../../..)
CPPFLAGS += -D__user= -I$(out)/include
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
DEPS := $(src)/writeback-test.c $(src)/../common/c5-kms-compose.c $(src)/../common/c5-kms-base.c $(wildcard $(KERNEL_SRC)/include/uapi/drm/*.h)

.PHONY: all host arm64 check clean FORCE
all: host arm64
host: $(out)/writeback-test-host
arm64: $(out)/writeback-test-aarch64
$(out):
	mkdir -p $@
$(out)/uapi-path: FORCE | $(out)
	@test -f "$(KERNEL_SRC)/include/uapi/drm/drm_mode.h"
	@mkdir -p "$(out)/include"
	@if test "$$(readlink "$(out)/include/drm")" != "$(KERNEL_SRC)/include/uapi/drm"; then ln -sfnT "$(KERNEL_SRC)/include/uapi/drm" "$(out)/include/drm"; printf '%s\n' "$(KERNEL_SRC)" > "$@"; elif test ! -f "$@"; then printf '%s\n' "$(KERNEL_SRC)" > "$@"; fi
FORCE:

$(out)/writeback-test-host: $(DEPS) $(out)/uapi-path | $(out)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@
$(out)/writeback-test-aarch64: $(DEPS) $(out)/uapi-path | $(out)
	$(CROSS_CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@
check: host
	$(out)/writeback-test-host --selftest
	$(out)/writeback-test-host --help
clean:
	rm -f $(out)/writeback-test-host $(out)/writeback-test-aarch64
