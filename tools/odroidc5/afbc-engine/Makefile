# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
CC ?= cc
O ?= /tmp/odroidc5-afbc-engine
src := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
out := $(abspath $(O))
KERNEL_SRC ?= $(abspath $(src)/../../../..)
driver := $(KERNEL_SRC)/common_drivers/drivers/drm/s7d
headers := linux/align.h linux/array_size.h linux/bitops.h linux/bits.h linux/errno.h \
	linux/instruction_pointer.h linux/limits.h linux/overflow.h linux/string.h \
	linux/timekeeping.h linux/types.h linux/wordpart.h drm/drm_fourcc.h \
	drm/drm_framebuffer.h drm/drm_gem_dma_helper.h drm/drm_rect.h drm/drm_util.h
CFLAGS ?= -O1 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
	-fsanitize=undefined -fno-sanitize-recover=all
SOURCES := $(src)/engine-test.c $(driver)/s7d-afbc.c $(driver)/s7d-afbc-engine.c
AFRC_SOURCES := $(src)/afrc-test.c $(driver)/s7d-afrc.c

.PHONY: all check clean
all: $(out)/afbc-engine-host $(out)/afrc-host
$(out)/include/.stamp: $(src)/Makefile
	mkdir -p $(out)/include/linux $(out)/include/drm
	for h in $(headers); do echo '#include "shim.h"' > $(out)/include/$$h; done
	touch $@
$(out)/afbc-engine-host: $(SOURCES) $(src)/shim.h $(wildcard $(driver)/s7d-afbc*.h) $(out)/include/.stamp
	$(CC) $(CFLAGS) -I$(out)/include -I$(src) -I$(driver) $(SOURCES) -o $@
$(out)/afrc-host: $(AFRC_SOURCES) $(src)/shim.h $(driver)/s7d-afrc.h $(out)/include/.stamp
	$(CC) $(CFLAGS) -I$(out)/include -I$(src) -I$(driver) $(AFRC_SOURCES) -o $@
check: all
	$(out)/afbc-engine-host
	$(out)/afrc-host
clean:
	rm -rf $(out)
