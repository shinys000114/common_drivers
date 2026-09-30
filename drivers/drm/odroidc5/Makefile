# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
ccflags-y += -I$(srctree)/$(src)

obj-$(CONFIG_AMLOGIC_C5_NATIVE_DISPLAY) += s7d-display.o
s7d-display-y := s7d-drv.o s7d-hdmi.o s7d-hdmi-audio.o s7d-hdmi-io.o s7d-hdmi-ddc.o \
		s7d-vpu.o s7d-vpp.o s7d-crtc.o s7d-plane.o s7d-gem.o \
		s7d-osd.o s7d-encp.o s7d-osd-pipeline.o s7d-scanout.o s7d-rdma.o
