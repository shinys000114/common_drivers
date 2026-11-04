/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Hardkernel Co., Ltd. */

#ifndef S7D_CODEC_MEMORY_H
#define S7D_CODEC_MEMORY_H

#include <linux/types.h>

#include "s7d-codec-types.h"

struct device;
struct dma_buf;
struct page;
struct s7d_codec_dma_copy_plane;
struct s7d_codec_ledger;
struct vb2_mem_ops;

/*
 * Every engine allocates from one codec CMA pool. A client reserves what its
 * session may take when it sets its coded format; it is admitted only while
 * the pool holds that beside the other clients' reservations, or beside what
 * the device holds now when that is more. Fragmentation can still fail an
 * admitted allocation, so a late -ENOMEM stays possible.
 */
struct s7d_codec_reservation {
	enum s7d_codec_engine engine;
	size_t bytes;
};

/* The platform owner names its pool once, before any client exists. */
int s7d_codec_ledger_attach(struct device *dev, size_t pool_bytes);
void s7d_codec_ledger_detach(struct device *dev);
/*
 * Replace @reservation with @bytes on @engine. Growing it is refused with
 * -EBUSY when the pool cannot hold it; shrinking or keeping it never is.
 * The caller serializes changes to one reservation.
 */
int s7d_codec_ledger_reserve(struct device *dev, struct s7d_codec_reservation *reservation,
			     enum s7d_codec_engine engine, size_t bytes);
/*
 * Set @reservation without admission: to put back one saved before a failed
 * change, to drop one (zero bytes), or to count an allocation already made.
 */
void s7d_codec_ledger_set(struct device *dev, struct s7d_codec_reservation *reservation,
			  const struct s7d_codec_reservation *value);
/* The client is gone or changes codec. */
void s7d_codec_ledger_drop(struct device *dev, struct s7d_codec_reservation *reservation);
/* Count an allocation of @bytes from @dev's pool; returns what to uncharge. */
struct s7d_codec_ledger *s7d_codec_ledger_charge(struct device *dev, size_t bytes);
void s7d_codec_ledger_uncharge(struct s7d_codec_ledger *ledger, size_t bytes);

/*
 * Ordinary system pages below 4 GiB, mapped for the device, that only the
 * hardware touches (an MMU-mapped FBC body): the CPU never reads or writes
 * them. Caller serializes and frees them only once the hardware is stopped.
 */
struct s7d_codec_pages {
	struct device *dev;
	struct page **page;
	dma_addr_t *dma;
	u32 count;
	u32 capacity;
};

/* Room for @capacity pages, none held yet; again on an initialized set is a no-op. */
int s7d_codec_pages_init(struct s7d_codec_pages *set, struct device *dev, u32 capacity);
/* Grow @set to @count pages, from @pool (may be NULL) first, then the system. */
int s7d_codec_pages_fill(struct s7d_codec_pages *set, struct s7d_codec_pages *pool,
			 u32 count);
/* Shrink @set to @count pages: the rest go to @pool while it has room, else are freed. */
void s7d_codec_pages_trim(struct s7d_codec_pages *set, struct s7d_codec_pages *pool,
			  u32 count);
/* Free every page and the bookkeeping. */
void s7d_codec_pages_destroy(struct s7d_codec_pages *set);

/* Coherent CAPTURE MMAP only; link with the native platform owner. */
extern const struct vb2_mem_ops s7d_codec_memory_memops;

int s7d_codec_memory_copy(struct dma_buf *const dst[2], const size_t dst_offset[2],
			  struct dma_buf *const src[2], const size_t src_offset[2],
			  const size_t bytes[2], bool (*cancelled)(void *priv), void *priv);

/* CPU view of a dma-buf this exporter made (coherent, lives with the dma-buf). */
void *s7d_codec_memory_cpu(struct dma_buf *dmabuf);

/* Internal: the session owner has validated and pinned every source plane. */
int s7d_codec_memory_copy_dma(struct dma_buf *const dst[2], const size_t dst_offset[2],
			      const struct s7d_codec_dma_copy_plane *planes,
			      bool (*cancelled)(void *priv), void *priv);

#endif
