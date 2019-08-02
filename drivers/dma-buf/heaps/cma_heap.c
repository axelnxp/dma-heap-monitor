// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF CMA heap exporter
 *
 * Copyright (C) 2012, 2019 Linaro Ltd.
 * Author: <benjamin.gaignard@linaro.org> for ST-Ericsson.
 */

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/cma.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <linux/seq_file.h>

#include "heap-helpers.h"

struct cma_heap {
	struct dma_heap *heap;
	struct cma *cma;

	size_t heap_size;
	size_t free_size;
	size_t allocated_size;
	size_t allocated_peak;
	size_t largest_free_buf;
};

#define to_cma_heap(x) container_of(x, struct cma_heap, heap)

static void cma_heap_free(struct heap_helper_buffer *buffer)
{
	struct cma_heap *cma_heap = dma_heap_get_data(buffer->heap);
	unsigned long nr_pages = buffer->pagecount;
	struct page *cma_pages = buffer->priv_virt;

	/* free page list */
	kfree(buffer->pages);
	/* release memory */
	cma_release(cma_heap->cma, cma_pages, nr_pages);
	kfree(buffer);
}

/* dmabuf heap CMA operations functions */
static int cma_heap_allocate(struct dma_heap *heap,
				unsigned long len,
				unsigned long fd_flags,
				unsigned long heap_flags)
{
	struct cma_heap *cma_heap = dma_heap_get_data(heap);
	struct heap_helper_buffer *helper_buffer;
	struct page *cma_pages;
	size_t size = PAGE_ALIGN(len);
	unsigned long nr_pages = size >> PAGE_SHIFT;
	unsigned long align = get_order(size);
	struct dma_buf *dmabuf;
	int ret = -ENOMEM;
	pgoff_t pg;

	if (align > CONFIG_CMA_ALIGNMENT)
		align = CONFIG_CMA_ALIGNMENT;

	helper_buffer = kzalloc(sizeof(*helper_buffer), GFP_KERNEL);
	if (!helper_buffer)
		return -ENOMEM;

	init_heap_helper_buffer(helper_buffer, cma_heap_free);
	helper_buffer->flags = heap_flags;
	helper_buffer->heap = heap;
	helper_buffer->size = len;

	cma_pages = cma_alloc(cma_heap->cma, nr_pages, align, false);
	if (!cma_pages)
		goto free_buf;

	if (PageHighMem(cma_pages)) {
		unsigned long nr_clear_pages = nr_pages;
		struct page *page = cma_pages;

		while (nr_clear_pages > 0) {
			void *vaddr = kmap_atomic(page);

			memset(vaddr, 0, PAGE_SIZE);
			kunmap_atomic(vaddr);
			page++;
			nr_clear_pages--;
		}
	} else {
		memset(page_address(cma_pages), 0, size);
	}

	helper_buffer->pagecount = nr_pages;
	helper_buffer->pages = kmalloc_array(helper_buffer->pagecount,
					     sizeof(*helper_buffer->pages),
					     GFP_KERNEL);
	if (!helper_buffer->pages) {
		ret = -ENOMEM;
		goto free_cma;
	}

	for (pg = 0; pg < helper_buffer->pagecount; pg++) {
		helper_buffer->pages[pg] = &cma_pages[pg];
		if (!helper_buffer->pages[pg])
			goto free_pages;
	}

	/* create the dmabuf */
	dmabuf = heap_helper_export_dmabuf(helper_buffer, fd_flags);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto free_pages;
	}

	helper_buffer->dmabuf = dmabuf;
	helper_buffer->priv_virt = cma_pages;

	ret = dma_buf_fd(dmabuf, fd_flags);
	if (ret < 0) {
		dma_buf_put(dmabuf);
		/* just return, as put will call release and that will free */
		return ret;
	}

	return ret;

free_pages:
	kfree(helper_buffer->pages);
free_cma:
	cma_release(cma_heap->cma, cma_pages, nr_pages);
free_buf:
	kfree(helper_buffer);
	return ret;
}

static const struct dma_heap_ops cma_heap_ops = {
	.allocate = cma_heap_allocate,
};

static void update_cma_heap_info(struct dma_heap* heap)
{
	struct cma_heap *cma_heap;
	cma_heap = to_cma_heap(heap);

	cma_heap->heap_size = cma_get_size(cma_heap->cma);
	cma_heap->free_size = cma_get_free_size(cma_heap->cma);
	cma_heap->allocated_size = cma_heap->heap_size - cma_heap->free_size;
	if(cma_heap->allocated_size > cma_heap->allocated_peak) cma_heap->allocated_peak = cma_heap->allocated_size;
	cma_heap->largest_free_buf = cma_get_largest_free_buf(cma_heap->cma);
}

static int dma_cma_heap_debug_show(struct dma_heap *heap, struct seq_file *s, void *unused)
{
	struct cma_heap *cma_heap;
	size_t heap_frag;

	cma_heap = to_cma_heap(heap);

	seq_puts(s, "\n----- DMA CMA HEAP DEBUG -----\n");
	if(heap->type == DMA_HEAP_TYPE_CMA) {
		update_cma_heap_info(heap);

		heap_frag = ((cma_heap->free_size - cma_heap->largest_free_buf) * 100) / cma_heap->free_size;

		seq_printf(s, "%19s %19zu\n", "heap size", cma_heap->heap_size);
		seq_printf(s, "%19s %19zu\n", "free size", cma_heap->free_size);
		seq_printf(s, "%19s %19zu\n", "allocated size", cma_heap->allocated_size);
		seq_printf(s, "%19s %19zu\n", "allocated peak", cma_heap->allocated_peak);
		seq_printf(s, "%19s %19zu\n", "largest free buffer", cma_heap->largest_free_buf);
		seq_printf(s, "%19s %19zu\n", "heap fragmentation", heap_frag);
	}
	else {
		pr_err("%s: Invalid heap type for debug: %d\n", __func__, heap->type);
	}
	seq_puts(s, "\n");
	return 0;
}

static int __add_cma_heap(struct cma *cma, void *data)
{
	struct cma_heap *cma_heap;
	struct dma_heap_export_info exp_info;

	cma_heap = kzalloc(sizeof(*cma_heap), GFP_KERNEL);
	if (!cma_heap)
		return -ENOMEM;
	cma_heap->cma = cma;

	exp_info.name = cma_get_name(cma);
	exp_info.ops = &cma_heap_ops;
	exp_info.priv = cma_heap;

	cma_heap->heap = dma_heap_add(&exp_info);
	if (IS_ERR(cma_heap->heap)) {
		int ret = PTR_ERR(cma_heap->heap);

		kfree(cma_heap);
		return ret;
	}

	cma_heap->heap.type = DMA_HEAP_TYPE_CMA;
	cma_heap->heap.debug_show = dma_cma_heap_debug_show;
	update_cma_heap_info(cma_heap->heap);

	return 0;
}

static int add_cma_heaps(void)
{
	cma_for_each_area(__add_cma_heap, NULL);
	return 0;
}
device_initcall(add_cma_heaps);
