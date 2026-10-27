// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 codec DMA-buffer preparation. No codec register or job interface. */
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <uapi/linux/mt6580-vcodec.h>
#include <asm/dma-iommu.h>
#include <asm/dma-mapping.h>

#define VC_ALLOC_LIMIT SZ_32M
#define VC_FILE_LIMIT SZ_64M

struct vc_device {
	struct device *dev;
	struct miscdevice misc;
	struct mutex lock;
	struct kref ref;
	struct completion drained;
	bool removing;
	atomic64_t allocations;
	atomic64_t imports;
	atomic64_t mmaps;
	atomic64_t vmas;
	atomic64_t buffers;
	atomic64_t bytes;
};

struct vc_file {
	struct vc_device *vc;
	struct mutex lock;
	struct kref ref;
	struct xarray buffers;
	atomic_long_t bytes;
	u32 next_id;
};

struct vc_buffer {
	struct vc_file *owner;
	struct kref ref;
	size_t size;
	size_t charge;
	dma_addr_t iova;
	void *cpu;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
};

static void vc_device_release(struct kref *ref)
{
	struct vc_device *vc = container_of(ref, struct vc_device, ref);

	complete(&vc->drained);
}

static void vc_file_release(struct kref *ref)
{
	struct vc_file *ctx = container_of(ref, struct vc_file, ref);

	kref_put(&ctx->vc->ref, vc_device_release);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
}

static void vc_buffer_release(struct kref *ref)
{
	struct vc_buffer *buf = container_of(ref, struct vc_buffer, ref);
	struct vc_file *ctx = buf->owner;
	struct vc_device *vc = ctx->vc;

	if (buf->dmabuf) {
		if (buf->sgt)
			dma_buf_unmap_attachment_unlocked(buf->attachment, buf->sgt,
							 DMA_BIDIRECTIONAL);
		if (buf->attachment)
			dma_buf_detach(buf->dmabuf, buf->attachment);
		dma_buf_put(buf->dmabuf);
	} else if (buf->cpu) {
		dma_free_coherent(vc->dev, buf->size, buf->cpu, buf->iova);
	}
	/* A released handle with surviving VMAs remains charged until here. */
	atomic_long_sub(buf->charge, &ctx->bytes);
	atomic64_sub(buf->charge, &vc->bytes);
	atomic64_dec(&vc->buffers);
	kfree(buf);
	kref_put(&ctx->ref, vc_file_release);
}

/* Caller serializes reservations with ctx->lock; final unmaps only subtract. */
static struct vc_buffer *vc_buffer_create(struct vc_file *ctx, size_t size)
{
	struct vc_buffer *buf;
	size_t charge = PAGE_ALIGN(size);

	if (charge > VC_FILE_LIMIT ||
	    atomic_long_read(&ctx->bytes) > VC_FILE_LIMIT - charge)
		return ERR_PTR(-EDQUOT);
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);
	buf->owner = ctx;
	buf->size = size;
	buf->charge = charge;
	kref_init(&buf->ref);
	kref_get(&ctx->ref);
	atomic_long_add(charge, &ctx->bytes);
	atomic64_add(charge, &ctx->vc->bytes);
	atomic64_inc(&ctx->vc->buffers);
	return buf;
}

static int vc_check_iova(struct vc_buffer *buf)
{
	struct scatterlist *sg;
	u64 start = 0, end = 0;
	unsigned int i;

	for_each_sgtable_dma_sg(buf->sgt, sg, i) {
		u64 addr = sg_dma_address(sg);
		u64 len = sg_dma_len(sg);

		if (!len || addr + len > BIT_ULL(32))
			return -ERANGE;
		if (!i)
			start = addr;
		else if (addr != end)
			return -ERANGE;
		end = addr + len;
	}
	if (!i || end - start < buf->size)
		return -ERANGE;
	buf->iova = start;
	return 0;
}

static struct vc_buffer *vc_import(struct vc_file *ctx, int fd)
{
	struct dma_buf_attachment *attachment;
	struct dma_buf *dmabuf;
	struct sg_table *sgt;
	struct vc_buffer *buf;
	int ret;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return ERR_CAST(dmabuf);
	if (!dmabuf->size || dmabuf->size > VC_FILE_LIMIT) {
		dma_buf_put(dmabuf);
		return ERR_PTR(-E2BIG);
	}
	buf = vc_buffer_create(ctx, dmabuf->size);
	if (IS_ERR(buf)) {
		dma_buf_put(dmabuf);
		return buf;
	}
	buf->dmabuf = dmabuf;
	attachment = dma_buf_attach(dmabuf, ctx->vc->dev);
	if (IS_ERR(attachment)) {
		ret = PTR_ERR(attachment);
		goto err;
	}
	buf->attachment = attachment;
	sgt = dma_buf_map_attachment_unlocked(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err;
	}
	buf->sgt = sgt;
	ret = vc_check_iova(buf);
	if (ret)
		goto err;
	return buf;
err:
	kref_put(&buf->ref, vc_buffer_release);
	return ERR_PTR(ret);
}

static struct vc_buffer *vc_alloc(struct vc_file *ctx, u64 size)
{
	struct vc_buffer *buf;

	if (!size || size > VC_ALLOC_LIMIT)
		return ERR_PTR(-E2BIG);
	buf = vc_buffer_create(ctx, PAGE_ALIGN(size));
	if (IS_ERR(buf))
		return buf;
	buf->cpu = dma_alloc_coherent(ctx->vc->dev, buf->size, &buf->iova,
				      GFP_KERNEL | __GFP_ZERO);
	if (!buf->cpu) {
		kref_put(&buf->ref, vc_buffer_release);
		return ERR_PTR(-ENOMEM);
	}
	if ((u64)buf->iova + buf->size > BIT_ULL(32)) {
		kref_put(&buf->ref, vc_buffer_release);
		return ERR_PTR(-ERANGE);
	}
	return buf;
}

static long vc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vc_file *ctx = file->private_data;
	struct mt6580_vcodec_buffer request;
	struct vc_buffer *buf;
	void __user *user = (void __user *)arg;
	int ret = 0;
	u32 id;

	if (cmd != MT6580_VCODEC_IMPORT && cmd != MT6580_VCODEC_ALLOC &&
	    cmd != MT6580_VCODEC_RELEASE)
		return -ENOTTY;
	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	mutex_lock(&ctx->lock);
	if (cmd == MT6580_VCODEC_RELEASE) {
		if (!request.id || request.fd || request.size || request.iova ||
		    request.mmap_offset) {
			ret = -EINVAL;
			goto out;
		}
		buf = xa_erase(&ctx->buffers, request.id);
		if (!buf) {
			ret = -ENOENT;
			goto out;
		}
		kref_put(&buf->ref, vc_buffer_release);
		goto out;
	}
	if (READ_ONCE(ctx->vc->removing)) {
		ret = -ENODEV;
		goto out;
	}
	if (request.id || request.iova || request.mmap_offset ||
	    (cmd == MT6580_VCODEC_IMPORT && request.size) ||
	    (cmd == MT6580_VCODEC_ALLOC && request.fd != -1)) {
		ret = -EINVAL;
		goto out;
	}
	id = ctx->next_id;
	if (!id) {
		ret = -ENOSPC;
		goto out;
	}
	buf = cmd == MT6580_VCODEC_IMPORT ? vc_import(ctx, request.fd) :
		vc_alloc(ctx, request.size);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto out;
	}
	ret = xa_insert(&ctx->buffers, id, buf, GFP_KERNEL);
	if (ret)
		goto put;
	/* Never recycle an offset while an old VMA or stale token can exist. */
	ctx->next_id++;
	request.id = id;
	request.size = buf->size;
	request.iova = buf->iova;
	request.mmap_offset = buf->cpu ? (u64)id << PAGE_SHIFT : 0;
	if (copy_to_user(user, &request, sizeof(request))) {
		xa_erase(&ctx->buffers, id);
		ret = -EFAULT;
		goto put;
	}
	if (buf->cpu)
		atomic64_inc(&ctx->vc->allocations);
	else
		atomic64_inc(&ctx->vc->imports);
	goto out;
put:
	kref_put(&buf->ref, vc_buffer_release);
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static void vc_vma_open(struct vm_area_struct *vma)
{
	struct vc_buffer *buf = vma->vm_private_data;

	kref_get(&buf->ref);
	atomic64_inc(&buf->owner->vc->mmaps);
	atomic64_inc(&buf->owner->vc->vmas);
}

static void vc_vma_close(struct vm_area_struct *vma)
{
	struct vc_buffer *buf = vma->vm_private_data;

	atomic64_dec(&buf->owner->vc->vmas);
	kref_put(&buf->ref, vc_buffer_release);
}

static const struct vm_operations_struct vc_vm_ops = {
	.open = vc_vma_open,
	.close = vc_vma_close,
};

static int vc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vc_file *ctx = file->private_data;
	unsigned long id = vma->vm_pgoff;
	struct vc_buffer *buf;
	int ret;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC) ||
	    !id || id > U32_MAX)
		return -EINVAL;
	mutex_lock(&ctx->lock);
	buf = xa_load(&ctx->buffers, id);
	if (READ_ONCE(ctx->vc->removing)) {
		ret = -ENODEV;
		goto out;
	}
	if (!buf || !buf->cpu || vma->vm_end - vma->vm_start > buf->size) {
		ret = -EINVAL;
		goto out;
	}
	/* ARM IOMMU DMA mmap uses vm_map_pages, with no private VMA handlers. */
	if (vma->vm_ops && vma->vm_ops != &vma_dummy_vm_ops) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vm_flags_clear(vma, VM_MAYEXEC);
	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(ctx->vc->dev, vma, buf->cpu, buf->iova,
				buf->size);
	vma->vm_pgoff = id;
	/* Do not replace private mapping management from another DMA backend. */
	if (!ret && vma->vm_ops && vma->vm_ops != &vma_dummy_vm_ops)
		ret = -EOPNOTSUPP;
	if (!ret) {
		vma->vm_private_data = buf;
		vma->vm_ops = &vc_vm_ops;
		vc_vma_open(vma);
	}
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int vc_open(struct inode *inode, struct file *file)
{
	struct vc_device *vc = container_of(file->private_data, struct vc_device,
					  misc);
	struct vc_file *ctx;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mutex_lock(&vc->lock);
	if (vc->removing) {
		mutex_unlock(&vc->lock);
		kfree(ctx);
		return -ENODEV;
	}
	kref_get(&vc->ref);
	mutex_unlock(&vc->lock);
	ctx->vc = vc;
	ctx->next_id = 1;
	kref_init(&ctx->ref);
	mutex_init(&ctx->lock);
	xa_init(&ctx->buffers);
	atomic_long_set(&ctx->bytes, 0);
	file->private_data = ctx;
	ret = nonseekable_open(inode, file);
	if (ret)
		kref_put(&ctx->ref, vc_file_release);
	return ret;
}

static int vc_release(struct inode *inode, struct file *file)
{
	struct vc_file *ctx = file->private_data;
	struct vc_buffer *buf;
	unsigned long id;

	mutex_lock(&ctx->lock);
	xa_for_each(&ctx->buffers, id, buf) {
		xa_erase(&ctx->buffers, id);
		kref_put(&buf->ref, vc_buffer_release);
	}
	xa_destroy(&ctx->buffers);
	mutex_unlock(&ctx->lock);
	kref_put(&ctx->ref, vc_file_release);
	return 0;
}

static const struct file_operations vc_fops = {
	.owner = THIS_MODULE,
	.open = vc_open,
	.release = vc_release,
	.unlocked_ioctl = vc_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.mmap = vc_mmap,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buffer)
{
	struct vc_device *vc = dev_get_drvdata(dev);

	return sysfs_emit(buffer,
		"allocations=%lld imports=%lld mmaps=%lld vmas=%lld buffers=%lld bytes=%lld\n",
		atomic64_read(&vc->allocations), atomic64_read(&vc->imports),
		atomic64_read(&vc->mmaps), atomic64_read(&vc->vmas),
		atomic64_read(&vc->buffers), atomic64_read(&vc->bytes));
}
static DEVICE_ATTR_RO(state);

static struct attribute *vc_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vc);

static int vc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vc_device *vc;
	int ret;

	static_assert(sizeof(struct mt6580_vcodec_buffer) == 32);
	if (!to_dma_iommu_mapping(dev) ||
	    !iommu_get_domain_for_dev(dev) ||
	    to_dma_iommu_mapping(dev)->domain != iommu_get_domain_for_dev(dev))
		return dev_err_probe(dev, -ENODEV, "M4U DMA domain required\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "32-bit DMA mask required\n");
	dma_set_max_seg_size(dev, VC_FILE_LIMIT);
	dma_set_seg_boundary(dev, DMA_BIT_MASK(32));
	vc = devm_kzalloc(dev, sizeof(*vc), GFP_KERNEL);
	if (!vc)
		return -ENOMEM;
	vc->dev = dev;
	mutex_init(&vc->lock);
	kref_init(&vc->ref);
	init_completion(&vc->drained);
	vc->misc.minor = MISC_DYNAMIC_MINOR;
	vc->misc.name = "mt6580-vcodec";
	vc->misc.fops = &vc_fops;
	vc->misc.parent = dev;
	vc->misc.mode = 0600;
	platform_set_drvdata(pdev, vc);
	/* A buffer or open fd is not an active codec job. Drop genpd's initial
	 * domain hold through the real supplier chain without opening VCODEC CG.
	 */
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Initial runtime resume failed\n");
		goto disable_pm;
	}
	ret = pm_runtime_put_sync_suspend(dev);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Initial runtime suspend failed\n");
		goto disable_pm;
	}
	ret = misc_register(&vc->misc);
	if (ret) {
		dev_err_probe(dev, ret, "Buffer device registration failed\n");
		goto disable_pm;
	}
	return 0;
disable_pm:
	pm_runtime_disable(dev);
	return ret;
}

static void vc_remove(struct platform_device *pdev)
{
	struct vc_device *vc = platform_get_drvdata(pdev);

	mutex_lock(&vc->lock);
	WRITE_ONCE(vc->removing, true);
	mutex_unlock(&vc->lock);
	misc_deregister(&vc->misc);
	/* Do not let DMA/IOMMU teardown race an open file or surviving VMA. */
	kref_put(&vc->ref, vc_device_release);
	wait_for_completion(&vc->drained);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id vc_match[] = {
	{ .compatible = "mediatek,mt6580-vcodec" },
	{ }
};
MODULE_DEVICE_TABLE(of, vc_match);

static struct platform_driver vc_driver = {
	.probe = vc_probe,
	.remove = vc_remove,
	.driver = {
		.name = "mt6580-vcodec",
		.of_match_table = vc_match,
		.suppress_bind_attrs = true,
		.dev_groups = vc_groups,
	},
};
module_platform_driver(vc_driver);

MODULE_DESCRIPTION("MediaTek MT6580 codec DMA buffer bridge");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
