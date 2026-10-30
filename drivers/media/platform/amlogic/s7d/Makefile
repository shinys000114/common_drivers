# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.

obj-$(CONFIG_VIDEO_AMLOGIC_S7D) += s7d-vdec.o

s7d-vdec-y := \
	s7d-vdec-platform.o \
	h264/s7d-h264-backend.o \
	common/s7d-codec-session.o \
	common/s7d-codec-vb2.o \
	common/s7d-codec-memory.o \
	common/s7d-codec-dos.o \
	common/s7d-codec-firmware.o \
	h264/s7d-vdec-runtime.o \
	common/s7d-codec-layout.o \
	h264/s7d-h264-layout.o \
	h264/s7d-h264-protocol.o \
	h264/s7d-h264-context.o \
	h264/s7d-h264-input.o \
	h264/s7d-h264-dpb.o \
	h264/s7d-h264-picture-snapshot.o \
	h264/s7d-h264-capture.o \
	h264/s7d-h264-picture-hw.o \
	h264/s7d-h264-mailbox.o \
	common/s7d-v4l2.o \
	common/s7d-v4l2-device.o

s7d-vdec-$(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_ENGINE) += common/s7d-hevc-engine.o

s7d-vdec-$(CONFIG_VIDEO_AMLOGIC_S7D_HEVC_EXPERIMENTAL) += \
	hevc/s7d-hevc-input.o \
	hevc/s7d-hevc-dpb.o \
	hevc/s7d-hevc-sequence.o \
	hevc/s7d-hevc-rpm.o \
	hevc/s7d-hevc-context.o \
	hevc/s7d-hevc-picture.o \
	hevc/s7d-hevc-backend.o \
	hevc/s7d-hevc-v4l2.o

s7d-vdec-$(CONFIG_VIDEO_AMLOGIC_S7D_JPEG_EXPERIMENTAL) += \
	jpeg/s7d-jpeg-quant.o \
	jpeg/s7d-jpeg-bitstream.o \
	jpeg/s7d-jpeg-protocol.o \
	jpeg/s7d-jpeg-backend.o \
	jpeg/s7d-jpeg-v4l2.o

