/*
 * Copyright (C) 2012-2017 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 *
 *
 */

 #include "mali_internal_sync.h"
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
#include <linux/ioctl.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>

#include "mali_osk.h"
#include "mali_kernel_common.h"
#if defined(DEBUG)
#include "mali_session.h"
#include "mali_timeline.h"
#endif


static const struct dma_fence_ops dma_fence_ops;


static struct mali_internal_sync_point *mali_internal_fence_to_sync_pt(struct dma_fence *fence)
{
	MALI_DEBUG_ASSERT_POINTER(fence);
	return container_of(fence, struct mali_internal_sync_point, base);
}

static inline struct mali_internal_sync_timeline *mali_internal_sync_pt_to_sync_timeline(struct mali_internal_sync_point *sync_pt)
{
	MALI_DEBUG_ASSERT_POINTER(sync_pt);
	return container_of(sync_pt->base.lock, struct mali_internal_sync_timeline, sync_pt_list_lock);
}

static void mali_internal_sync_timeline_free(struct kref *kref_count)
{
	struct mali_internal_sync_timeline *sync_timeline;

	MALI_DEBUG_ASSERT_POINTER(kref_count);

	sync_timeline = container_of(kref_count, struct mali_internal_sync_timeline, kref_count);

	if (sync_timeline->ops->release_obj)
		sync_timeline->ops->release_obj(sync_timeline);

	kfree(sync_timeline);
}


struct mali_internal_sync_timeline *mali_internal_sync_timeline_create(const struct mali_internal_sync_timeline_ops *ops,
					   int size, const char *name)
{
	struct mali_internal_sync_timeline *sync_timeline = NULL;

	MALI_DEBUG_ASSERT_POINTER(ops);

	if (size < sizeof(struct mali_internal_sync_timeline)) {
		MALI_PRINT_ERROR(("Mali internal sync:Invalid size to create the mali internal sync timeline.\n"));
		goto err;
	}

	sync_timeline = kzalloc(size, GFP_KERNEL);
	if (NULL == sync_timeline) {
		MALI_PRINT_ERROR(("Mali internal sync:Failed to  allocate buffer  for the mali internal sync timeline.\n"));
		goto err;
	}
	kref_init(&sync_timeline->kref_count);
	sync_timeline->ops = ops;
	sync_timeline->dma_fence_context = dma_fence_context_alloc(1);
	strlcpy(sync_timeline->name, name, sizeof(sync_timeline->name));

	INIT_LIST_HEAD(&sync_timeline->sync_pt_list_head);
	spin_lock_init(&sync_timeline->sync_pt_list_lock);

	return sync_timeline;
err:
	if (NULL != sync_timeline) {
		kfree(sync_timeline);
	}
	return NULL;
}

void mali_internal_sync_timeline_destroy(struct mali_internal_sync_timeline *sync_timeline)
{
	MALI_DEBUG_ASSERT_POINTER(sync_timeline);

	sync_timeline->destroyed = MALI_TRUE;

	smp_wmb();

	mali_internal_sync_timeline_signal(sync_timeline);
	kref_put(&sync_timeline->kref_count, mali_internal_sync_timeline_free);
}

void mali_internal_sync_timeline_signal(struct mali_internal_sync_timeline *sync_timeline)
{
	unsigned long flags;
	struct mali_internal_sync_point *sync_pt, *next;

	MALI_DEBUG_ASSERT_POINTER(sync_timeline);

	spin_lock_irqsave(&sync_timeline->sync_pt_list_lock, flags);

	list_for_each_entry_safe(sync_pt, next, &sync_timeline->sync_pt_list_head,
				 sync_pt_list) {
		if (dma_fence_is_signaled_locked(&sync_pt->base))
			list_del_init(&sync_pt->sync_pt_list);
	}

	spin_unlock_irqrestore(&sync_timeline->sync_pt_list_lock, flags);
}

struct mali_internal_sync_point *mali_internal_sync_point_create(struct mali_internal_sync_timeline *sync_timeline, int size)
{
	unsigned long flags;
	struct mali_internal_sync_point *sync_pt = NULL;

	MALI_DEBUG_ASSERT_POINTER(sync_timeline);

	if (size < sizeof(struct mali_internal_sync_point)) {
		MALI_PRINT_ERROR(("Mali internal sync:Invalid size to create the mali internal sync point.\n"));
		goto err;
	}

	sync_pt = kzalloc(size, GFP_KERNEL);
	if (NULL == sync_pt) {
		MALI_PRINT_ERROR(("Mali internal sync:Failed to  allocate buffer  for the mali internal sync point.\n"));
		goto err;
	}
	spin_lock_irqsave(&sync_timeline->sync_pt_list_lock, flags);
	kref_get(&sync_timeline->kref_count);
	dma_fence_init(&sync_pt->base, &dma_fence_ops, &sync_timeline->sync_pt_list_lock,
		   sync_timeline->dma_fence_context, ++sync_timeline->value);
	INIT_LIST_HEAD(&sync_pt->sync_pt_list);
	spin_unlock_irqrestore(&sync_timeline->sync_pt_list_lock, flags);

	return sync_pt;
err:
	if (NULL != sync_pt) {
		kfree(sync_pt);
	}
	return NULL;
}

struct sync_file *mali_internal_sync_fence_fdget(int fd)
{
 struct dma_fence *f = sync_file_get_fence(fd);
 struct sync_file *sf;
 if (!f) return NULL;
 sf = sync_file_create(f);
 dma_fence_put(f);
 return sf;
}
struct sync_file *mali_internal_sync_fence_merge(struct sync_file *a, struct sync_file *b)
{
 struct dma_fence **fences;
 struct dma_fence_array *array;
 struct sync_file *sf;
 fences = kmalloc_array(2, sizeof(*fences), GFP_KERNEL);
 if (!fences) return NULL;
 fences[0] = dma_fence_get(a->fence);
 fences[1] = dma_fence_get(b->fence);
 array = dma_fence_array_create(2, fences, dma_fence_context_alloc(1), 1, false);
 if (!array) { dma_fence_put(fences[0]); dma_fence_put(fences[1]); kfree(fences); return NULL; }
 sf = sync_file_create(&array->base);
 dma_fence_put(&array->base);
 return sf;
}
static void mali_fence_dispatch(struct dma_fence *f, struct dma_fence_cb *cb)
{
 struct mali_internal_sync_fence_waiter *w = container_of(cb, struct mali_internal_sync_fence_waiter, cb);
 w->callback(w->sync_file, w);
}
void mali_internal_sync_fence_waiter_init(struct mali_internal_sync_fence_waiter *waiter, mali_internal_sync_callback_t callback)
{
 INIT_LIST_HEAD(&waiter->cb.node);
 waiter->callback = callback;
}
int mali_internal_sync_fence_wait_async(struct sync_file *sf, struct mali_internal_sync_fence_waiter *waiter)
{
 int ret;
 waiter->sync_file = sf;
 ret = dma_fence_get_status(sf->fence);
 if (ret) return ret < 0 ? ret : 1;
 ret = dma_fence_add_callback(sf->fence, &waiter->cb, mali_fence_dispatch);
 if (!ret) return 0;
 ret = dma_fence_get_status(sf->fence);
 return ret < 0 ? ret : 1;
}
int mali_internal_sync_fence_cancel_async(struct sync_file *sf, struct mali_internal_sync_fence_waiter *waiter)
{
 return dma_fence_remove_callback(sf->fence, &waiter->cb) ? 0 : -ENOENT;
}

static const char *mali_internal_fence_get_driver_name(struct dma_fence *fence)
{
	struct mali_internal_sync_point *sync_pt;
	struct mali_internal_sync_timeline *parent;

	MALI_DEBUG_ASSERT_POINTER(fence);

	sync_pt = mali_internal_fence_to_sync_pt(fence);
	parent = mali_internal_sync_pt_to_sync_timeline(sync_pt);

	return parent->ops->driver_name;
}

static const char *mali_internal_fence_get_timeline_name(struct dma_fence *fence)
{
	struct mali_internal_sync_point *sync_pt;
	struct mali_internal_sync_timeline *parent;

	MALI_DEBUG_ASSERT_POINTER(fence);

	sync_pt = mali_internal_fence_to_sync_pt(fence);
	parent = mali_internal_sync_pt_to_sync_timeline(sync_pt);

	return parent->name;
}

static void mali_internal_fence_release(struct dma_fence *fence)
{
	unsigned long flags;
	struct mali_internal_sync_point *sync_pt;
	struct mali_internal_sync_timeline *parent;

	MALI_DEBUG_ASSERT_POINTER(fence);

	sync_pt = mali_internal_fence_to_sync_pt(fence);
	parent = mali_internal_sync_pt_to_sync_timeline(sync_pt);


	spin_lock_irqsave(fence->lock, flags);
	if (WARN_ON_ONCE(!list_empty(&sync_pt->sync_pt_list)))
		list_del(&sync_pt->sync_pt_list);
	spin_unlock_irqrestore(fence->lock, flags);

	if (parent->ops->free_pt)
		parent->ops->free_pt(sync_pt);

	kref_put(&parent->kref_count, mali_internal_sync_timeline_free);
	dma_fence_free(&sync_pt->base);
}

static bool mali_internal_fence_signaled(struct dma_fence *fence)
{
	int ret;
	struct mali_internal_sync_point *sync_pt;
	struct mali_internal_sync_timeline *parent;

	MALI_DEBUG_ASSERT_POINTER(fence);

	sync_pt = mali_internal_fence_to_sync_pt(fence);
	parent = mali_internal_sync_pt_to_sync_timeline(sync_pt);

	ret = parent->ops->has_signaled(sync_pt);
	if (0 > ret)
		dma_fence_set_error(fence, ret);
	return ret;
}

static bool mali_internal_fence_enable_signaling(struct dma_fence *fence)
{
	struct mali_internal_sync_point *sync_pt;
	struct mali_internal_sync_timeline *parent;

	MALI_DEBUG_ASSERT_POINTER(fence);

	sync_pt = mali_internal_fence_to_sync_pt(fence);
	parent = mali_internal_sync_pt_to_sync_timeline(sync_pt);

	if (mali_internal_fence_signaled(fence))
		return false;

	list_add_tail(&sync_pt->sync_pt_list, &parent->sync_pt_list_head);
	return true;
}

static const struct dma_fence_ops dma_fence_ops = {
	.get_driver_name = mali_internal_fence_get_driver_name,
	.get_timeline_name = mali_internal_fence_get_timeline_name,
	.enable_signaling = mali_internal_fence_enable_signaling,
	.signaled = mali_internal_fence_signaled,
	.release = mali_internal_fence_release,
};
#endif
