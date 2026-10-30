/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_AV1_PROTOCOL_H
#define S7D_AV1_PROTOCOL_H

#include "../common/s7d-hevc-engine.h"
#include "s7d-av1-dma-context.h"
#include "s7d-av1-rpm.h"

enum s7d_av1_firmware_status {
	S7D_AV1_FGS_CONTINUE = 8,
	S7D_AV1_DECODE_EMPTY = 0x20,
	S7D_AV1_TIMEOUT = 0x21,
	S7D_AV1_SEARCH_EMPTY = 0x22,
	S7D_AV1_OVERSIZE = 0x23,
	S7D_AV1_EOS = 0x24,
	S7D_AV1_NAL_DONE = 0x25,
	S7D_AV1_PICTURE_END = 0xe0,
	S7D_AV1_FGS_REQUEST = 0xe1,
	S7D_AV1_PICTURE_PRE_END = 0xe2,
	S7D_AV1_SEQUENCE_PARSED = 0xf2,
	S7D_AV1_FRAME_HEADER_PARSED = 0xf3,
	S7D_AV1_FRAME_PARSED = 0xf4,
	S7D_AV1_REDUNDANT_PARSED = 0xf5,
};

enum s7d_av1_execution_phase {
	S7D_AV1_EXECUTION_IDLE,
	S7D_AV1_EXECUTION_SEARCH,
	S7D_AV1_EXECUTION_HEADER,
	S7D_AV1_EXECUTION_DECODE,
	S7D_AV1_EXECUTION_PICTURE,
	S7D_AV1_EXECUTION_INPUT,
	S7D_AV1_EXECUTION_RESTART,
	S7D_AV1_EXECUTION_FAILED,
};

enum s7d_av1_execution_event {
	S7D_AV1_EVENT_NONE,
	S7D_AV1_EVENT_SEQUENCE,
	S7D_AV1_EVENT_FRAME,
	S7D_AV1_EVENT_EXISTING,
	S7D_AV1_EVENT_REDUNDANT,
	S7D_AV1_EVENT_PICTURE,
	S7D_AV1_EVENT_INPUT,
	S7D_AV1_EVENT_EOS,
	S7D_AV1_EVENT_FGS_READ,
};

struct s7d_av1_irq_snapshot {
	u32 status;
	u32 shift_bytes;
	u32 lcu_count;
	u32 mmu_pages;
	int error;
};

struct s7d_av1_execution {
	struct s7d_hevc_irq_cookie cookie;
	enum s7d_av1_execution_phase phase;
	u32 input_bytes;
	u32 max_shift_bytes;
	u32 expected_frames;
	u32 headers_seen;
	u32 pictures_done;
	u32 last_shift_bytes;
	u32 paused_status;
	bool sequence_seen;
	bool show_existing;
	bool input_consumed;
};

/* Powered exclusive AV1M/core2/no-swap lease, post-TEE IQIT, CPU/fetch off. */
int s7d_av1_protocol_init(struct regmap *map,
			  const struct s7d_av1_workspace *workspace,
			  const struct s7d_av1_region *mmu_map,
			  const struct s7d_av1_sequence *sequence);
/* No CPU, AXI port or Linux IRQ operation; their owner starts only afterward. */
int s7d_av1_protocol_start(struct regmap *map,
			   const struct s7d_av1_workspace *workspace,
			   const struct s7d_av1_input_plan *input, u32 decode_count);
int s7d_av1_protocol_fgs_read_ack(struct regmap *map);
int s7d_av1_protocol_irq_snapshot(struct regmap *map,
				  struct s7d_av1_irq_snapshot *snapshot);
int s7d_av1_execution_begin(struct s7d_av1_execution *execution,
			    const struct s7d_hevc_irq_cookie *cookie,
			    const struct s7d_av1_input_plan *input,
			    u32 expected_frames);
/* Serialized process/thread context; snapshot and RPM belong to this cookie. */
int s7d_av1_execution_receive(struct s7d_av1_execution *execution,
			      const struct s7d_hevc_irq_cookie *cookie,
			      const struct s7d_av1_irq_snapshot *snapshot,
			      enum s7d_av1_execution_event *event);
/* Only after actual sequence validation or current picture programming. */
int s7d_av1_execution_resume(struct s7d_av1_execution *execution,
			     struct regmap *map, bool picture_configured);
int s7d_av1_execution_accept_frame(struct s7d_av1_execution *execution,
				   const struct s7d_av1_header *header,
				   enum s7d_av1_execution_event *event);
/* Keep backing pinned while SEARCH drains the remaining bytes after one picture. */
int s7d_av1_execution_drain(struct s7d_av1_execution *execution,
			    struct regmap *map);
/* A picture IRQ is not an input completion or a DMA-idle proof. */
int s7d_av1_execution_picture_complete(struct s7d_av1_execution *execution,
				       bool physical_idle);

#endif
