// SPDX-License-Identifier: GPL-2.0-only
/* Privileged MT6580 DMA buffers and serialized native hybrid codec jobs. */
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/capability.h>
#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/pm_wakeup.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <linux/wait.h>
#include <uapi/linux/mt6580-vcodec.h>
#include <asm/dma-iommu.h>
#include <asm/dma-mapping.h>
#include <dt-bindings/memory/mtk-memory-port.h>

#define VC_ALLOC_LIMIT SZ_32M
#define VC_FILE_LIMIT SZ_64M
#define VC_BUFFER_ID_MAX 0x7fffffffU
#define VC_JOB_MAX 64
#define VC_JOB_TIMEOUT_MAX 30000U
#define VC_REG_SIZE SZ_8K
#define VC_AUX_SIZE SZ_4K
#define VC_AUX_BASE MT6580_VCODEC_REG_AUX_BASE
#define VC_IRQ_STATUS 0x10
#define VC_IRQ_ACK 0x0c
#define VC_ENC_IRQ_ENABLE 0x668
#define VC_ENC_IRQ_ACK 0x678
#define VC_ENC_IRQ_STATUS 0x67c
#define VC_REQUIRED_LARB1_PORTS (BIT(3) | BIT(7) | BIT(8) | BIT(9) | BIT(10))

enum vc_engine {
	VC_DECODER,
	VC_ENCODER,
	VC_ENGINE_COUNT,
};

struct vc_job;
struct vc_buffer;

struct vc_oal_header {
	u32 completed, timeout, count, status_ptr;
};

struct vc_binding {
	struct vc_buffer *header_buffer;
	struct vc_buffer *status_buffer;
	struct vc_oal_header *header;
	u32 *status;
	u32 count;
	u32 offsets[MT6580_VCODEC_OAL_MAX_REGS];
};

struct vc_device {
	struct device *dev;
	struct miscdevice misc;
	struct mutex lock;
	struct kref ref;
	struct completion drained;
	bool removing;
	struct mutex job_lock;
	wait_queue_head_t job_idle;
	struct vc_job *owner;
	struct vc_job *quarantined_job;
	bool faulted;
	struct notifier_block power_notifier;
	wait_queue_head_t power_changed;
	atomic64_t off_generations;
	atomic64_t poweroff_errors;
	void __iomem *regs;
	void __iomem *aux_regs;
	phys_addr_t reg_start;
	phys_addr_t aux_reg_start;
	struct clk *clock;
	struct wakeup_source *wake;
	int irq[VC_ENGINE_COUNT];
	u32 larb1_ports;
	atomic64_t jobs;
	atomic64_t irqs;
	atomic64_t timeouts;
	atomic64_t cancellations;
	atomic64_t spurious;
	atomic64_t power_errors;
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
	/* Includes RELEASEd coherent objects still held by VMAs. */
	struct list_head live_buffers;
	spinlock_t buffers_lock;
	atomic_long_t bytes;
	u32 next_id;
	struct vc_binding binding;
	struct list_head jobs;
	u32 job_count;
};

struct vc_buffer {
	struct list_head live_node;
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

struct vc_job {
	struct list_head node;
	struct kref ref;
	struct vc_file *ctx;
	struct vc_binding binding;
	struct vc_buffer **buffers;
	u32 buffer_count;
	struct completion done;
	u64 handle;
	u32 driver_type;
	bool active;
	bool stopping;
	int result;
	u32 irq_status;
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

	spin_lock(&ctx->buffers_lock);
	list_del(&buf->live_node);
	spin_unlock(&ctx->buffers_lock);
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
	spin_lock(&ctx->buffers_lock);
	list_add_tail(&buf->live_node, &ctx->live_buffers);
	spin_unlock(&ctx->buffers_lock);
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

static void vc_binding_put(struct vc_binding *binding)
{
	struct vc_buffer *header = binding->header_buffer;
	struct vc_buffer *status = binding->status_buffer;

	memset(binding, 0, sizeof(*binding));
	if (status)
		kref_put(&status->ref, vc_buffer_release);
	if (header)
		kref_put(&header->ref, vc_buffer_release);
}

static void vc_binding_get(struct vc_binding *dest,
			   const struct vc_binding *source)
{
	*dest = *source;
	kref_get(&dest->header_buffer->ref);
	if (dest->status_buffer)
		kref_get(&dest->status_buffer->ref);
}

static void vc_job_buffers_put(struct vc_job *job)
{
	u32 i;

	for (i = 0; i < job->buffer_count; i++)
		kref_put(&job->buffers[i]->ref, vc_buffer_release);
	kfree(job->buffers);
	job->buffers = NULL;
	job->buffer_count = 0;
}

/* job_lock excludes new buffer allocations. Final VMA unmaps may remove
 * entries, so collect references under the independent live-buffer lock.
 */
static int vc_job_buffers_get(struct vc_job *job)
{
	struct vc_file *ctx = job->ctx;
	struct vc_buffer *buffer;
	u32 count = 0;

	spin_lock(&ctx->buffers_lock);
	list_for_each_entry(buffer, &ctx->live_buffers, live_node)
		count++;
	spin_unlock(&ctx->buffers_lock);
	job->buffers = kcalloc(count, sizeof(*job->buffers), GFP_KERNEL);
	if (!job->buffers)
		return -ENOMEM;
	spin_lock(&ctx->buffers_lock);
	list_for_each_entry(buffer, &ctx->live_buffers, live_node) {
		if (kref_get_unless_zero(&buffer->ref))
			job->buffers[job->buffer_count++] = buffer;
	}
	spin_unlock(&ctx->buffers_lock);
	return 0;
}

static void vc_job_release(struct kref *ref)
{
	struct vc_job *job = container_of(ref, struct vc_job, ref);
	struct vc_file *ctx = job->ctx;

	vc_binding_put(&job->binding);
	vc_job_buffers_put(job);
	kfree(job);
	kref_put(&ctx->ref, vc_file_release);
}

static struct vc_job *vc_find_job(struct vc_file *ctx, u64 handle)
{
	struct vc_job *job;

	list_for_each_entry(job, &ctx->jobs, node)
		if (job->handle == handle)
			return job;
	return NULL;
}

static bool vc_driver_type_valid(u32 driver_type)
{
	return driver_type == MT6580_VCODEC_H264_DRIVER ||
		driver_type == MT6580_VCODEC_MP4_ENC_DRIVER;
}

static enum vc_engine vc_job_engine(const struct vc_job *job)
{
	return job->driver_type == MT6580_VCODEC_MP4_ENC_DRIVER ?
		VC_ENCODER : VC_DECODER;
}

static int vc_job_irq(struct vc_device *vc, const struct vc_job *job)
{
	return vc->irq[vc_job_engine(job)];
}

static u32 vc_job_irq_status(struct vc_device *vc, const struct vc_job *job)
{
	if (vc_job_engine(job) == VC_ENCODER)
		return readl(vc->aux_regs + VC_ENC_IRQ_STATUS);
	return readl(vc->regs + VC_IRQ_STATUS);
}

static void vc_job_irq_ack(struct vc_device *vc, const struct vc_job *job,
			   u32 status)
{
	if (vc_job_engine(job) == VC_ENCODER) {
		/* MT6580 MP4-VENC acknowledges even status 2 by writing 1. */
		writel(1, vc->aux_regs + VC_ENC_IRQ_ACK);
		readl(vc->aux_regs + VC_ENC_IRQ_STATUS);
	} else {
		writel(status, vc->regs + VC_IRQ_ACK);
		readl(vc->regs + VC_IRQ_STATUS);
	}
}

static bool vc_buffer_range(struct vc_buffer *buf, u32 offset, size_t size)
{
	return buf && buf->cpu && !(offset & 3) &&
		offset <= buf->size && size <= buf->size - offset;
}

static bool vc_register_offset_valid(u32 offset)
{
	if (offset & 3)
		return false;
	if (offset < VC_REG_SIZE)
		return offset <= VC_REG_SIZE - sizeof(u32);
	return offset >= VC_AUX_BASE &&
		offset <= VC_AUX_BASE + VC_AUX_SIZE - sizeof(u32);
}

static u32 vc_read_registered(struct vc_device *vc, u32 offset)
{
	if (offset >= VC_AUX_BASE)
		return readl(vc->aux_regs + offset - VC_AUX_BASE);
	return readl(vc->regs + offset);
}

static int vc_oal_bind(struct vc_file *ctx, void __user *user)
{
	struct mt6580_vcodec_oal request;
	struct vc_binding binding = { }, old;
	struct vc_buffer *header, *status = NULL;
	u32 i;
	int ret = 0;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (request.count > MT6580_VCODEC_OAL_MAX_REGS ||
	    (!request.count && (request.status_id || request.status_offset)))
		return -EINVAL;
	for (i = 0; i < MT6580_VCODEC_OAL_MAX_REGS; i++) {
		if ((i < request.count &&
		     !vc_register_offset_valid(request.register_offsets[i])) ||
		    (i >= request.count && request.register_offsets[i]))
			return -EINVAL;
	}
	mutex_lock(&ctx->vc->job_lock);
	mutex_lock(&ctx->lock);
	if (READ_ONCE(ctx->vc->removing) || ctx->vc->faulted) {
		ret = ctx->vc->faulted ? -EIO : -ENODEV;
		goto out;
	}
	header = xa_load(&ctx->buffers, request.header_id);
	if (!vc_buffer_range(header, request.header_offset,
			     sizeof(struct vc_oal_header))) {
		ret = -EINVAL;
		goto out;
	}
	if (request.count) {
		status = xa_load(&ctx->buffers, request.status_id);
		if (!vc_buffer_range(status, request.status_offset,
				     request.count * 2 * sizeof(u32))) {
			ret = -EINVAL;
			goto out;
		}
		if (header == status &&
		    request.header_offset < request.status_offset + request.count * 8 &&
		    request.status_offset < request.header_offset + sizeof(struct vc_oal_header)) {
			ret = -EINVAL;
			goto out;
		}
	}
	binding.header_buffer = header;
	binding.status_buffer = status;
	binding.header = header->cpu + request.header_offset;
	binding.status = status ? status->cpu + request.status_offset : NULL;
	binding.count = request.count;
	memcpy(binding.offsets, request.register_offsets, sizeof(binding.offsets));
	kref_get(&header->ref);
	if (status)
		kref_get(&status->ref);
	/* An active job has its own immutable snapshot and memory references. */
	old = ctx->binding;
	ctx->binding = binding;
	vc_binding_put(&old);
out:
	mutex_unlock(&ctx->lock);
	mutex_unlock(&ctx->vc->job_lock);
	return ret;
}

/* The provider reports OFF only after its real SPM power-off handshake.
 * Do not mistake runtime-PM bookkeeping success for proof that DMA stopped.
 */
static int vc_power_notify(struct notifier_block *nb, unsigned long event,
			   void *unused)
{
	struct vc_device *vc = container_of(nb, struct vc_device, power_notifier);

	if (event == GENPD_NOTIFY_OFF) {
		atomic64_inc(&vc->off_generations);
		wake_up_all(&vc->power_changed);
	}
	return NOTIFY_OK;
}

/* IRQ is masked and job_lock is held; all operations here may sleep. */
static void vc_finish_job(struct vc_device *vc, struct vc_job *job,
			  int result, u32 irq_status)
{
	struct vc_oal_header *header = job->binding.header;
	u64 off_generation = atomic64_read(&vc->off_generations);
	bool unsafe = false;
	int ret;

	WRITE_ONCE(header->timeout, result ? 1 : 0);
	/* Register results/timeout become visible before the terminal marker. */
	dma_wmb();
	smp_store_release(&header->completed, 1);
	clk_disable_unprepare(vc->clock);
	ret = pm_runtime_put_sync_suspend(vc->dev);
	if (ret < 0) {
		atomic64_inc(&vc->power_errors);
		unsafe = true;
		if (!result)
			result = ret;
	}
	if (result && !wait_event_timeout(vc->power_changed,
		atomic64_read(&vc->off_generations) != off_generation,
		msecs_to_jiffies(200))) {
		/* Neither engine has an established break/reset sequence here.
		 * Keep every possible DMA object mapped if stopping is unproven.
		 */
		atomic64_inc(&vc->poweroff_errors);
		unsafe = true;
	}
	job->irq_status = irq_status;
	job->result = result;
	job->active = false;
	vc->owner = NULL;
	if (unsafe) {
		vc->faulted = true;
		vc->quarantined_job = job;
		/* Transfer the active-owner ref to a permanent quarantine. Its
		 * buffer set includes RELEASEd-but-mapped objects, not just OAL.
		 * The wake source also stays held: recovery requires a reboot.
		 */
		dev_err(vc->dev, "Codec stop unproven; DMA buffers quarantined until reboot\n");
	} else {
		__pm_relax(vc->wake);
		vc_binding_put(&job->binding);
		vc_job_buffers_put(job);
	}
	complete_all(&job->done);
	wake_up_all(&vc->job_idle);
	if (!unsafe)
		kref_put(&job->ref, vc_job_release); /* active hardware owner */
}

static irqreturn_t vc_irq_thread(int irq, void *data)
{
	struct vc_device *vc = data;
	struct vc_job *job;
	u32 status, i;

	mutex_lock(&vc->job_lock);
	job = vc->owner;
	/* Cancellation masks then synchronizes IRQ outside job_lock. */
	if (!job || job->stopping) {
		mutex_unlock(&vc->job_lock);
		return IRQ_HANDLED;
	}
	if (irq != vc_job_irq(vc, job)) {
		/* A late thread from the other masked engine cannot finish this job. */
		mutex_unlock(&vc->job_lock);
		return IRQ_NONE;
	}
	status = vc_job_irq_status(vc, job);
	if (!status) {
		atomic64_inc(&vc->spurious);
		mutex_unlock(&vc->job_lock);
		return IRQ_NONE;
	}
	for (i = 0; i < job->binding.count; i++)
		WRITE_ONCE(job->binding.status[2 * i + 1],
			   vc_read_registered(vc, job->binding.offsets[i]));
	vc_job_irq_ack(vc, job, status); /* flush ACK before clock removal */
	disable_irq_nosync(irq);
	atomic64_inc(&vc->irqs);
	vc_finish_job(vc, job, 0, status);
	mutex_unlock(&vc->job_lock);
	return IRQ_HANDLED;
}

static void vc_cancel_job(struct vc_job *job, int result)
{
	struct vc_device *vc = job->ctx->vc;
	u32 status;

	mutex_lock(&vc->job_lock);
	if (!job->active) {
		mutex_unlock(&vc->job_lock);
		return;
	}
	if (job->stopping) {
		mutex_unlock(&vc->job_lock);
		wait_for_completion(&job->done);
		return;
	}
	job->stopping = true;
	disable_irq_nosync(vc_job_irq(vc, job));
	mutex_unlock(&vc->job_lock);
	/* A queued IRQ thread may need job_lock; never wait under that lock. */
	synchronize_irq(vc_job_irq(vc, job));
	mutex_lock(&vc->job_lock);
	status = vc_job_irq_status(vc, job);
	vc_job_irq_ack(vc, job, status);
	if (result == -ETIMEDOUT)
		atomic64_inc(&vc->timeouts);
	else
		atomic64_inc(&vc->cancellations);
	vc_finish_job(vc, job, result, status);
	mutex_unlock(&vc->job_lock);
}

static int vc_acquire_job(struct vc_file *ctx,
			  const struct mt6580_vcodec_job *request)
{
	struct vc_device *vc = ctx->vc;
	unsigned long deadline = jiffies + msecs_to_jiffies(request->timeout_ms);
	struct vc_job *job, *old;
	long remaining;
	u32 status;
	int ret;

	for (;;) {
		mutex_lock(&vc->job_lock);
		if (READ_ONCE(vc->removing) || vc->faulted) {
			ret = vc->faulted ? -EIO : -ENODEV;
			goto unlock;
		}
		if (!vc->owner)
			break;
		mutex_unlock(&vc->job_lock);
		if (!request->timeout_ms)
			return -EBUSY;
		remaining = (long)(deadline - jiffies);
		if (remaining <= 0)
			return -ETIMEDOUT;
		remaining = wait_event_interruptible_timeout(vc->job_idle,
			!READ_ONCE(vc->owner) || READ_ONCE(vc->removing),
			remaining);
		if (remaining < 0)
			return remaining;
		if (!remaining)
			return -ETIMEDOUT;
	}
	if (!ctx->binding.header_buffer) {
		ret = -ENXIO;
		goto unlock;
	}
	old = vc_find_job(ctx, request->handle);
	if (old && old->driver_type != request->driver_type) {
		ret = -EINVAL;
		goto unlock;
	}
	if (old) {
		list_del_init(&old->node);
		ctx->job_count--;
		kref_put(&old->ref, vc_job_release);
	}
	if (ctx->job_count >= VC_JOB_MAX) {
		ret = -ENOSPC;
		goto unlock;
	}
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto unlock;
	}
	kref_init(&job->ref);
	init_completion(&job->done);
	INIT_LIST_HEAD(&job->node);
	job->ctx = ctx;
	job->handle = request->handle;
	job->driver_type = request->driver_type;
	job->result = -EINPROGRESS;
	kref_get(&ctx->ref);
	vc_binding_get(&job->binding, &ctx->binding);
	ret = vc_job_buffers_get(job);
	if (ret) {
		kref_put(&job->ref, vc_job_release);
		goto unlock;
	}
	__pm_stay_awake(vc->wake);
	ret = pm_runtime_resume_and_get(vc->dev);
	if (ret < 0)
		goto relax;
	ret = clk_prepare_enable(vc->clock);
	if (ret) {
		pm_runtime_put_sync_suspend(vc->dev);
		goto relax;
	}
	/* Discard stale IRQ state before exposing the lock to userspace. */
	status = vc_job_irq_status(vc, job);
	if (status)
		vc_job_irq_ack(vc, job, status);
	if (vc_job_engine(job) == VC_ENCODER) {
		/* The MT6580 BSP enables this only after the shared power chain. */
		writel(1, vc->aux_regs + VC_ENC_IRQ_ENABLE);
		readl(vc->aux_regs + VC_ENC_IRQ_ENABLE);
	}
	WRITE_ONCE(job->binding.header->completed, 0);
	WRITE_ONCE(job->binding.header->timeout, 0);
	WRITE_ONCE(job->binding.header->count, job->binding.count);
	dma_wmb();
	job->active = true;
	kref_get(&job->ref); /* active hardware owner */
	list_add_tail(&job->node, &ctx->jobs);
	ctx->job_count++;
	vc->owner = job;
	atomic64_inc(&vc->jobs);
	enable_irq(vc_job_irq(vc, job));
	mutex_unlock(&vc->job_lock);
	return 0;
relax:
	atomic64_inc(&vc->power_errors);
	__pm_relax(vc->wake);
	kref_put(&job->ref, vc_job_release);
unlock:
	mutex_unlock(&vc->job_lock);
	return ret;
}

static long vc_job_ioctl(struct vc_file *ctx, unsigned int cmd,
			 unsigned long arg)
{
	struct vc_device *vc = ctx->vc;
	struct mt6580_vcodec_job request;
	struct vc_job *job;
	void __user *user = (void __user *)arg;
	long waited;
	bool active;
	int ret;

	if (cmd == MT6580_VCODEC_OAL_BIND)
		return vc_oal_bind(ctx, user);
	if (cmd == MT6580_VCODEC_OAL_UNBIND) {
		mutex_lock(&vc->job_lock);
		vc_binding_put(&ctx->binding);
		mutex_unlock(&vc->job_lock);
		return 0;
	}
	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!vc_driver_type_valid(request.driver_type) ||
	    !request.handle || request.irq_status || request.reserved ||
	    request.timeout_ms > VC_JOB_TIMEOUT_MAX ||
	    (cmd == MT6580_VCODEC_JOB_RELEASE && request.timeout_ms))
		return -EINVAL;
	if (cmd == MT6580_VCODEC_JOB_ACQUIRE)
		return vc_acquire_job(ctx, &request);
	mutex_lock(&vc->job_lock);
	job = vc_find_job(ctx, request.handle);
	if (job && job->driver_type != request.driver_type) {
		mutex_unlock(&vc->job_lock);
		return -EINVAL;
	}
	if (job)
		kref_get(&job->ref);
	mutex_unlock(&vc->job_lock);
	if (!job)
		return -ENOENT;
	if (cmd == MT6580_VCODEC_JOB_RELEASE) {
		mutex_lock(&vc->job_lock);
		active = job->active;
		mutex_unlock(&vc->job_lock);
		vc_cancel_job(job, -ECANCELED);
		mutex_lock(&vc->job_lock);
		if (!list_empty(&job->node)) {
			list_del_init(&job->node);
			ctx->job_count--;
			kref_put(&job->ref, vc_job_release); /* record ownership */
		}
		mutex_unlock(&vc->job_lock);
		ret = active ? 0 : -EALREADY;
		goto put;
	}
	if (!request.timeout_ms) {
		/* A poll must not cancel an otherwise valid hardware operation. */
		if (!completion_done(&job->done)) {
			ret = -EAGAIN;
			goto put;
		}
		waited = 1;
	} else {
		waited = wait_for_completion_interruptible_timeout(&job->done,
				msecs_to_jiffies(request.timeout_ms));
	}
	if (waited <= 0)
		vc_cancel_job(job, waited ? -ECANCELED : -ETIMEDOUT);
	mutex_lock(&vc->job_lock);
	request.irq_status = job->irq_status;
	ret = waited < 0 ? waited : job->result;
	mutex_unlock(&vc->job_lock);
	if (copy_to_user(user, &request, sizeof(request)))
		ret = -EFAULT;
put:
	kref_put(&job->ref, vc_job_release);
	return ret;
}

/* File close cancels its active hardware lock before freeing any DMA memory. */
static void vc_close_jobs(struct vc_file *ctx)
{
	struct vc_device *vc = ctx->vc;
	struct vc_job *job, *next;

	mutex_lock(&vc->job_lock);
	job = vc->owner;
	if (job && job->ctx == ctx)
		kref_get(&job->ref);
	else
		job = NULL;
	mutex_unlock(&vc->job_lock);
	if (job) {
		vc_cancel_job(job, -ECANCELED);
		kref_put(&job->ref, vc_job_release);
	}
	mutex_lock(&vc->job_lock);
	vc_binding_put(&ctx->binding);
	list_for_each_entry_safe(job, next, &ctx->jobs, node) {
		list_del_init(&job->node);
		kref_put(&job->ref, vc_job_release);
	}
	ctx->job_count = 0;
	mutex_unlock(&vc->job_lock);
}

/* Firmware IDs are binding evidence, not the old global /dev/m4u port enum. */
static int vc_m4u_ports(struct device *dev, u32 *ports)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	u32 mask = 0;
	unsigned int i;

	if (!mapping || !mapping->domain ||
	    mapping->domain != iommu_get_domain_for_dev(dev) ||
	    !fwspec || !fwspec->num_ids)
		return -ENODEV;
	for (i = 0; i < fwspec->num_ids; i++) {
		u32 id = fwspec->ids[i];
		u32 port = MTK_M4U_TO_PORT(id);

		if (MTK_M4U_TO_LARB(id) != 1 || port > 10 ||
		    id != MTK_M4U_ID(1, port))
			return -EINVAL;
		mask |= BIT(port);
	}
	if ((mask & VC_REQUIRED_LARB1_PORTS) != VC_REQUIRED_LARB1_PORTS)
		return -ENODEV;
	*ports = mask;
	return 0;
}

static long vc_get_caps(struct vc_file *ctx, void __user *user)
{
	struct vc_device *vc = ctx->vc;
	struct mt6580_vcodec_caps caps = {
		.driver_mask = BIT(MT6580_VCODEC_MP4_ENC_DRIVER) |
			       BIT(MT6580_VCODEC_H264_DRIVER),
		.flags = MT6580_VCODEC_CAP_M4U,
	};
	int ret;

	mutex_lock(&vc->job_lock);
	if (READ_ONCE(vc->removing))
		ret = -ENODEV;
	else
		ret = vc_m4u_ports(vc->dev, &caps.larb1_ports);
	if (!ret && caps.larb1_ports != vc->larb1_ports)
		ret = -ENODEV; /* This driver does not support rebinding live ports. */
	mutex_unlock(&vc->job_lock);
	if (ret)
		return ret;
	return copy_to_user(user, &caps, sizeof(caps)) ? -EFAULT : 0;
}

static long vc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vc_file *ctx = file->private_data;
	struct mt6580_vcodec_buffer request;
	struct vc_buffer *buf;
	void __user *user = (void __user *)arg;
	int ret = 0;
	u32 id;

	if (cmd == MT6580_VCODEC_GET_CAPS)
		return vc_get_caps(ctx, user);
	if (cmd == MT6580_VCODEC_OAL_BIND || cmd == MT6580_VCODEC_OAL_UNBIND ||
	    cmd == MT6580_VCODEC_JOB_ACQUIRE || cmd == MT6580_VCODEC_JOB_WAIT ||
	    cmd == MT6580_VCODEC_JOB_RELEASE)
		return vc_job_ioctl(ctx, cmd, arg);
	if (cmd != MT6580_VCODEC_IMPORT && cmd != MT6580_VCODEC_ALLOC &&
	    cmd != MT6580_VCODEC_RELEASE)
		return -ENOTTY;
	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	mutex_lock(&ctx->vc->job_lock);
	mutex_lock(&ctx->lock);
	/* No DMA handle may disappear while this prototype runs hardware. */
	if (ctx->vc->owner || ctx->vc->faulted) {
		ret = ctx->vc->faulted ? -EIO : -EBUSY;
		goto out;
	}
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
	if (!id || id > VC_BUFFER_ID_MAX) {
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
	mutex_unlock(&ctx->vc->job_lock);
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

static void vc_mmio_vma_open(struct vm_area_struct *vma)
{
	struct vc_file *ctx = vma->vm_private_data;

	kref_get(&ctx->ref);
	atomic64_inc(&ctx->vc->mmaps);
	atomic64_inc(&ctx->vc->vmas);
}

static void vc_mmio_vma_close(struct vm_area_struct *vma)
{
	struct vc_file *ctx = vma->vm_private_data;

	atomic64_dec(&ctx->vc->vmas);
	kref_put(&ctx->ref, vc_file_release);
}

static const struct vm_operations_struct vc_mmio_vm_ops = {
	.open = vc_mmio_vma_open,
	.close = vc_mmio_vma_close,
};

static int vc_mmio_mmap(struct vc_file *ctx, struct vm_area_struct *vma)
{
	unsigned long token = vma->vm_pgoff;
	unsigned long page = token - (MT6580_VCODEC_MMAP_REGS >> PAGE_SHIFT);
	u64 offset = (u64)page << PAGE_SHIFT;
	unsigned long length = vma->vm_end - vma->vm_start;
	phys_addr_t start;
	u32 region_size;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	if (offset < VC_REG_SIZE) {
		start = ctx->vc->reg_start;
		region_size = VC_REG_SIZE;
	} else if (offset >= VC_AUX_BASE &&
		   offset < VC_AUX_BASE + VC_AUX_SIZE) {
		start = ctx->vc->aux_reg_start;
		region_size = VC_AUX_SIZE;
		offset -= VC_AUX_BASE;
	} else {
		return -EINVAL;
	}
	/* Each VMA belongs to exactly one of the two disjoint resources. */
	if (length > region_size - offset)
		return -EINVAL;
	if (READ_ONCE(ctx->vc->removing))
		return -ENODEV;
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	vm_flags_clear(vma, VM_MAYEXEC);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_pgoff = offset >> PAGE_SHIFT;
	ret = vm_iomap_memory(vma, start, region_size);
	vma->vm_pgoff = token;
	if (!ret) {
		vma->vm_private_data = ctx;
		vma->vm_ops = &vc_mmio_vm_ops;
		vc_mmio_vma_open(vma);
	}
	return ret;
}

static int vc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vc_file *ctx = file->private_data;
	unsigned long id = vma->vm_pgoff;
	struct vc_buffer *buf;
	int ret;

	if (id >= (MT6580_VCODEC_MMAP_REGS >> PAGE_SHIFT))
		return vc_mmio_mmap(ctx, vma);
	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC) ||
	    !id || id > VC_BUFFER_ID_MAX)
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
	INIT_LIST_HEAD(&ctx->jobs);
	INIT_LIST_HEAD(&ctx->live_buffers);
	spin_lock_init(&ctx->buffers_lock);
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

	vc_close_jobs(ctx);
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
	/* mmap offsets are opaque 64-bit tokens, not byte positions in a file. */
	.fop_flags = FOP_UNSIGNED_OFFSET,
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
		"allocations=%lld imports=%lld mmaps=%lld vmas=%lld buffers=%lld bytes=%lld\n"
		"job_active=%u jobs=%lld irqs=%lld timeouts=%lld cancellations=%lld spurious=%lld power_errors=%lld\n"
		"faulted=%u off_generations=%lld poweroff_errors=%lld\n",
		atomic64_read(&vc->allocations), atomic64_read(&vc->imports),
		atomic64_read(&vc->mmaps), atomic64_read(&vc->vmas),
		atomic64_read(&vc->buffers), atomic64_read(&vc->bytes),
		READ_ONCE(vc->owner) != NULL, atomic64_read(&vc->jobs),
		atomic64_read(&vc->irqs), atomic64_read(&vc->timeouts),
		atomic64_read(&vc->cancellations), atomic64_read(&vc->spurious),
		atomic64_read(&vc->power_errors), READ_ONCE(vc->faulted),
		atomic64_read(&vc->off_generations),
		atomic64_read(&vc->poweroff_errors));
}
static DEVICE_ATTR_RO(state);

static struct attribute *vc_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vc);

static void vc_power_notifier_remove(void *data)
{
	struct vc_device *vc = data;

	dev_pm_genpd_remove_notifier(vc->dev);
}

static void vc_wakeup_unregister(void *data)
{
	wakeup_source_unregister(data);
}

static int vc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *resource;
	struct vc_device *vc;
	u32 larb1_ports;
	int ret;

	static_assert(sizeof(struct mt6580_vcodec_buffer) == 32);
	static_assert(sizeof(struct mt6580_vcodec_oal) == 84);
	static_assert(sizeof(struct mt6580_vcodec_job) == 24);
	static_assert(sizeof(struct mt6580_vcodec_caps) == 16);
	static_assert(sizeof(struct vc_oal_header) == 16);
	ret = vc_m4u_ports(dev, &larb1_ports);
	if (ret)
		return dev_err_probe(dev, ret,
				     "M4U DMA domain and LARB1 codec ports required\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "32-bit DMA mask required\n");
	dma_set_max_seg_size(dev, VC_FILE_LIMIT);
	dma_set_seg_boundary(dev, DMA_BIT_MASK(32));
	vc = devm_kzalloc(dev, sizeof(*vc), GFP_KERNEL);
	if (!vc)
		return -ENOMEM;
	vc->dev = dev;
	vc->larb1_ports = larb1_ports;
	mutex_init(&vc->lock);
	mutex_init(&vc->job_lock);
	init_waitqueue_head(&vc->job_idle);
	init_waitqueue_head(&vc->power_changed);
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != 0x1500b000 ||
	    resource_size(resource) != VC_REG_SIZE)
		return dev_err_probe(dev, -EINVAL, "Invalid VDEC resource\n");
	vc->reg_start = resource->start;
	vc->regs = devm_ioremap_resource(dev, resource);
	if (IS_ERR(vc->regs))
		return PTR_ERR(vc->regs);
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!resource || resource->start != 0x15009000 ||
	    resource_size(resource) != VC_AUX_SIZE)
		return dev_err_probe(dev, -EINVAL, "Invalid MP4-VENC auxiliary resource\n");
	vc->aux_reg_start = resource->start;
	vc->aux_regs = devm_ioremap_resource(dev, resource);
	if (IS_ERR(vc->aux_regs))
		return PTR_ERR(vc->aux_regs);
	vc->clock = devm_clk_get(dev, "vcodec");
	if (IS_ERR(vc->clock))
		return dev_err_probe(dev, PTR_ERR(vc->clock), "Missing VCODEC gate\n");
	for (int i = 0; i < VC_ENGINE_COUNT; i++) {
		vc->irq[i] = platform_get_irq(pdev, i);
		if (vc->irq[i] < 0)
			return vc->irq[i];
		if (irq_get_trigger_type(vc->irq[i]) != IRQ_TYPE_LEVEL_LOW)
			return dev_err_probe(dev, -EINVAL,
					     "Codec IRQ %d requires level-low trigger\n", i);
		ret = devm_request_threaded_irq(dev, vc->irq[i], NULL, vc_irq_thread,
					       IRQF_ONESHOT | IRQF_NO_AUTOEN,
					       dev_name(dev), vc);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot request codec IRQ %d\n", i);
	}
	vc->wake = wakeup_source_register(dev, "mt6580-vcodec-job");
	if (!vc->wake)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, vc_wakeup_unregister, vc->wake);
	if (ret)
		return ret;
	vc->power_notifier.notifier_call = vc_power_notify;
	ret = dev_pm_genpd_add_notifier(dev, &vc->power_notifier);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot observe ISP power-off\n");
	ret = devm_add_action_or_reset(dev, vc_power_notifier_remove, vc);
	if (ret)
		return ret;
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
	struct vc_job *job;

	mutex_lock(&vc->lock);
	WRITE_ONCE(vc->removing, true);
	mutex_unlock(&vc->lock);
	misc_deregister(&vc->misc);
	wake_up_all(&vc->job_idle);
	mutex_lock(&vc->job_lock);
	job = vc->owner;
	if (job)
		kref_get(&job->ref);
	mutex_unlock(&vc->job_lock);
	if (job) {
		vc_cancel_job(job, -ECANCELED);
		kref_put(&job->ref, vc_job_release);
	}
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
