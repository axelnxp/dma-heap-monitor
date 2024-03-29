// SPDX-License-Identifier: GPL-2.0
/*
 * Framework for userspace DMA-BUF allocations
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019 Linaro Ltd.
 */

#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/xarray.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/dma-heap.h>
#include <linux/seq_file.h>
#include <uapi/linux/dma-heap.h>

#define DEVNAME "dma_heap"

#define NUM_HEAP_MINORS 128

/**
 * struct dma_heap - represents a dmabuf heap in the system
 * @name:		used for debugging/device-node name
 * @ops:		ops struct for this heap
 * @minor		minor number of this heap device
 * @heap_devt		heap device node
 * @heap_cdev		heap char device
 *
 * Represents a heap of memory from which buffers can be made.
 */
struct dma_heap {
	const char *name;
	const struct dma_heap_ops *ops;
	void *priv;
	unsigned int minor;
	dev_t heap_devt;
	struct cdev heap_cdev;

	enum dma_heap_type type;
	struct dentry *debug_root;
	int (*debug_show)(struct dma_heap *heap, struct seq_file *, void *);
};

static dev_t dma_heap_devt;
static struct class *dma_heap_class;
static DEFINE_XARRAY_ALLOC(dma_heap_minors);

static int dma_heap_buffer_alloc(struct dma_heap *heap, size_t len,
				 unsigned int fd_flags,
				 unsigned int heap_flags)
{
	/*
	 * Allocations from all heaps have to begin
	 * and end on page boundaries.
	 */
	len = PAGE_ALIGN(len);
	if (!len)
		return -EINVAL;

	return heap->ops->allocate(heap, len, fd_flags, heap_flags);
}

static int dma_heap_open(struct inode *inode, struct file *file)
{
	struct dma_heap *heap;

	heap = xa_load(&dma_heap_minors, iminor(inode));
	if (!heap) {
		pr_err("dma_heap: minor %d unknown.\n", iminor(inode));
		return -ENODEV;
	}

	/* instance data as context */
	file->private_data = heap;
	nonseekable_open(inode, file);

	return 0;
}

static long dma_heap_ioctl_allocate(struct file *file, unsigned long arg)
{
	struct dma_heap_allocation_data heap_allocation;
	struct dma_heap *heap = file->private_data;
	int fd;

	if (copy_from_user(&heap_allocation, (void __user *)arg,
			   sizeof(heap_allocation)))
		return -EFAULT;

	if (heap_allocation.fd ||
	    heap_allocation.reserved0 ||
	    heap_allocation.reserved1) {
		pr_warn_once("dma_heap: ioctl data not valid\n");
		return -EINVAL;
	}

	if (heap_allocation.fd_flags & ~DMA_HEAP_VALID_FD_FLAGS) {
		pr_warn_once("dma_heap: fd_flags has invalid or unsupported flags set\n");
		return -EINVAL;
	}

	if (heap_allocation.heap_flags & ~DMA_HEAP_VALID_HEAP_FLAGS) {
		pr_warn_once("dma_heap: heap flags has invalid or unsupported flags set\n");
		return -EINVAL;
	}


	fd = dma_heap_buffer_alloc(heap, heap_allocation.len,
				   heap_allocation.fd_flags,
				   heap_allocation.heap_flags);
	if (fd < 0)
		return fd;

	heap_allocation.fd = fd;

	if (copy_to_user((void __user *)arg, &heap_allocation,
			 sizeof(heap_allocation))) {
		ksys_close(fd);
		return -EFAULT;
	}

	return 0;
}

static long dma_heap_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	int ret = 0;

	switch (cmd) {
	case DMA_HEAP_IOC_ALLOC:
		ret = dma_heap_ioctl_allocate(file, arg);
		break;
	default:
		return -ENOTTY;
	}

	return ret;
}

static const struct file_operations dma_heap_fops = {
	.owner          = THIS_MODULE,
	.open		= dma_heap_open,
	.unlocked_ioctl = dma_heap_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= dma_heap_ioctl,
#endif
};

static int dma_heap_debug_show(struct seq_file *s, void *unused)
{
	struct dma_heap *heap = s->private;
	if(heap->debug_show)
		heap->debug_show(heap, s, unused);

	return 0;
}

static int dma_heap_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, dma_heap_debug_show, inode->i_private);
}

static const struct file_operations dma_heap_debug_fops = {
	.open        = dma_heap_debug_open,
	.read        = seq_read,
	.llseek      = seq_lseek,
	.release     = single_release,
};

/**
 * dma_heap_get_data() - get per-subdriver data for the heap
 * @heap: DMA-Heap to retrieve private data for
 *
 * Returns:
 * The per-subdriver data for the heap.
 */
void *dma_heap_get_data(struct dma_heap *heap)
{
	return heap->priv;
}

struct dma_heap *dma_heap_add(const struct dma_heap_export_info *exp_info)
{
	struct dma_heap *heap, *err_ret;
	struct device *dev_ret;
	int ret;

	if (!exp_info->name || !strcmp(exp_info->name, "")) {
		pr_err("dma_heap: Cannot add heap without a name\n");
		return ERR_PTR(-EINVAL);
	}

	if (!exp_info->ops || !exp_info->ops->allocate) {
		pr_err("dma_heap: Cannot add heap with invalid ops struct\n");
		return ERR_PTR(-EINVAL);
	}

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);

	heap->name = exp_info->name;
	heap->ops = exp_info->ops;
	heap->priv = exp_info->priv;

	/* Find unused minor number */
	ret = xa_alloc(&dma_heap_minors, &heap->minor, heap,
			XA_LIMIT(0, NUM_HEAP_MINORS - 1), GFP_KERNEL);
	if (ret < 0) {
		pr_err("dma_heap: Unable to get minor number for heap\n");
		err_ret = ERR_PTR(ret);
		goto err0;
	}

	/* Create device */
	heap->heap_devt = MKDEV(MAJOR(dma_heap_devt), heap->minor);

	cdev_init(&heap->heap_cdev, &dma_heap_fops);
	ret = cdev_add(&heap->heap_cdev, heap->heap_devt, 1);
	if (ret < 0) {
		pr_err("dma_heap: Unable to add char device\n");
		err_ret = ERR_PTR(ret);
		goto err1;
	}

	dev_ret = device_create(dma_heap_class,
				NULL,
				heap->heap_devt,
				NULL,
				heap->name);
	if (IS_ERR(dev_ret)) {
		pr_err("dma_heap: Unable to create device\n");
		err_ret = (struct dma_heap *)dev_ret;
		goto err2;
	}

	heap->debug_root = debugfs_create_dir("dma-heap", NULL);
	if (!heap->debug_root) {
		pr_err("dma-heap: failed to create debugfs root directory.\n");
	}

	debug_file = debugfs_create_file(heap->name, 0644,
	                                 heap->debug_root, heap,
	                                 &dma_heap_debug_fops);
	if (!debug_file) {
        char buf[256], *path;
        path = dentry_path(heap->debug_root, buf, 256);
        pr_err("Failed to create heap debugfs at %s/%s\n",
               path, heap->name);
    }

	return heap;

err2:
	cdev_del(&heap->heap_cdev);
err1:
	xa_erase(&dma_heap_minors, heap->minor);
err0:
	kfree(heap);
	return err_ret;

}

static char *dma_heap_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "dma_heap/%s", dev_name(dev));
}

static int dma_heap_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&dma_heap_devt, 0, NUM_HEAP_MINORS, DEVNAME);
	if (ret)
		return ret;

	dma_heap_class = class_create(THIS_MODULE, DEVNAME);
	if (IS_ERR(dma_heap_class)) {
		unregister_chrdev_region(dma_heap_devt, NUM_HEAP_MINORS);
		return PTR_ERR(dma_heap_class);
	}
	dma_heap_class->devnode = dma_heap_devnode;

	return 0;
}
subsys_initcall(dma_heap_init);
