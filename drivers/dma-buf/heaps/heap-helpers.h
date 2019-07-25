/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DMABUF Heaps helper code
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019 Linaro Ltd.
 */

#ifndef _HEAP_HELPERS_H
#define _HEAP_HELPERS_H

#include <linux/dma-heap.h>
#include <linux/list.h>

struct heap_helper_buffer {
	struct dma_heap *heap;
	struct dma_buf *dmabuf;
	size_t size;
	unsigned long flags;

	unsigned long private_flags;
	void *priv_virt;
	struct mutex lock;
	int vmap_cnt;
	void *vaddr;
	pgoff_t pagecount;
	struct page **pages;
	struct list_head attachments;

	void (*free)(struct heap_helper_buffer *buffer);
};

void init_heap_helper_buffer(struct heap_helper_buffer *buffer,
				 void (*free)(struct heap_helper_buffer *));
struct dma_buf *heap_helper_export_dmabuf(
				struct heap_helper_buffer *helper_buffer,
				int fd_flags);
extern const struct dma_buf_ops heap_helper_ops;


#endif /* _HEAP_HELPERS_H */
