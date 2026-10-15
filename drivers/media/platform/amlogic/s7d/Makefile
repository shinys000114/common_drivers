# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.

obj-$(CONFIG_VIDEO_AMLOGIC_S7D) += s7d-vdec.o

s7d-vdec-y := \
	s7d-vdec-platform.o \
	s7d-h264-backend.o \
	s7d-codec-session.o \
	s7d-codec-vb2.o \
	s7d-codec-memory.o \
	s7d-codec-dos.o \
	s7d-codec-firmware.o \
	s7d-vdec-runtime.o \
	s7d-codec-layout.o \
	s7d-h264-layout.o \
	s7d-h264-protocol.o \
	s7d-h264-context.o \
	s7d-h264-input.o \
	s7d-h264-dpb.o \
	s7d-h264-picture-snapshot.o \
	s7d-h264-capture.o \
	s7d-h264-picture-hw.o \
	s7d-h264-mailbox.o \
	s7d-v4l2.o \
	s7d-v4l2-device.o

s7d-vdec-$(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_EXPERIMENTAL) += \
	s7d-hevc-input.o \
	s7d-hevc-dpb.o \
	s7d-hevc-sequence.o \
	s7d-hevc-rpm.o \
	s7d-hevc-context.o \
	s7d-hevc-picture.o \
	s7d-hevc-backend.o \
	s7d-hevc-v4l2.o

s7d-vdec-$(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) += \
	s7d-jpeg-quant.o \
	s7d-jpeg-bitstream.o \
	s7d-jpeg-protocol.o \
	s7d-jpeg-backend.o \
	s7d-jpeg-v4l2.o
