// SPDX-License-Identifier: GPL-2.0-only
/* Private ARM32 VAL memory adapter. Not an Android media service. */
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../include/uapi/linux/mt6580-vcodec.h"
#include "val-buffer-bridge.h"

struct val_memory {
	uint32_t signature, type;
	uint64_t size, va, iova;
	uint32_t alignment, pad;
	uint64_t aligned_va, aligned_iova;
	uint32_t codec, share_fd;
	uint64_t ion_handle, reserved, reserved_size;
	uint32_t ion_fd, pad_end;
};
_Static_assert(sizeof(struct val_memory) == 96, "VAL memory ABI");
_Static_assert(offsetof(struct val_memory, aligned_va) == 40, "VAL aligned VA");
struct allocation {
	struct allocation *next;
	struct mt6580_vcodec_buffer buffer;
	void *va;
	size_t length;
	int fd;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct allocation *allocations;
static int codec_fd = -1;
static pid_t owner_pid;

__attribute__((constructor)) static void init_owner(void)
{
	owner_pid = getpid();
}

enum { VAL_OK = 0, VAL_INVALID_PARAMETER = 2, VAL_INVALID_MEMORY = 3 };

static int sync_cpu(struct allocation *a, int start)
{
	struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_RW |
		(start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) };
	if (a->fd < 0) {
		__sync_synchronize(); /* coherent allocation, no streaming cache */
		return 0;
	}
	if (ioctl(a->fd, DMA_BUF_IOCTL_SYNC, &sync))
		return -1;
	return 0;
}

static int buffer_request(unsigned long command, struct mt6580_vcodec_buffer *request)
{
	int rc;
	/* Legacy software workers may allocate while the previous HW job ends.
	 * The kernel refuses buffer mutations until IRQ/timeout returns ownership.
	 */
	for (unsigned int attempt = 0; ; attempt++) {
		rc = ioctl(codec_fd, command, request);
		if (!rc || errno != EBUSY || attempt == 2000)
			return rc;
		usleep(1000);
	}
}

static int release_buffer(uint32_t id)
{
	struct mt6580_vcodec_buffer request = { .id = id };
	return buffer_request(MT6580_VCODEC_RELEASE, &request);
}

static int open_codec(void)
{
	if (codec_fd < 0)
		codec_fd = open("/dev/mt6580-vcodec", O_RDWR | O_CLOEXEC);
	return codec_fd;
}

static struct allocation *find_range(uint64_t va, uint64_t size)
{
	struct allocation *a;
	for (a = allocations; a; a = a->next) {
		uintptr_t start = (uintptr_t)a->va;
		if (va >= start && size <= a->length && va - start <= a->length - size)
			return a;
	}
	return NULL;
}

int mt6580_bridge_fd(void)
{
	int fd;
	if (owner_pid != getpid()) { errno = EPERM; return -1; }
	pthread_mutex_lock(&lock);
	fd = open_codec();
	pthread_mutex_unlock(&lock);
	return fd;
}

int mt6580_bridge_buffer(uintptr_t va, size_t bytes, uint32_t *id,
			 uint32_t *offset, int require_coherent)
{
	struct allocation *a;
	int rc = -1;
	if (owner_pid != getpid()) { errno = EPERM; return -1; }
	pthread_mutex_lock(&lock);
	a = find_range(va, bytes);
	if (a && (!require_coherent || a->fd < 0)) {
		*id = a->buffer.id;
		*offset = va - (uintptr_t)a->va;
		rc = 0;
	}
	pthread_mutex_unlock(&lock);
	if (rc) errno = EFAULT;
	return rc;
}

int mt6580_bridge_sync(int cpu)
{
	struct allocation *a;
	int rc = 0;
	if (owner_pid != getpid()) { errno = EPERM; return -1; }
	pthread_mutex_lock(&lock);
	for (a = allocations; a; a = a->next)
		if (sync_cpu(a, cpu)) rc = -1;
	pthread_mutex_unlock(&lock);
	return rc;
}

int eVideoMemAlloc(struct val_memory *m, uint32_t size)
{
	struct allocation *a = NULL;
	struct dma_heap_allocation_data heap = { .fd_flags = O_RDWR | O_CLOEXEC };
	uint32_t align, bytes;
	int heap_fd = -1, imported = 0, rc = VAL_INVALID_MEMORY;

	if (owner_pid != getpid() || !m || size != sizeof(*m))
		return VAL_INVALID_PARAMETER;
	/* ARM32 callers initialize only the low word of each extended union. */
	bytes = (uint32_t)m->size;
	if (!bytes || bytes > 32 * 1024 * 1024)
		return VAL_INVALID_PARAMETER;
	align = m->alignment;
	/* Page-aligned VA and IOVA give the same offset for these alignments. */
	if (!align || (align & (align - 1)) || align > 4096 || m->type > 3)
		return VAL_INVALID_PARAMETER;
	pthread_mutex_lock(&lock);
	if (open_codec() < 0)
		goto out;
	a = calloc(1, sizeof(*a));
	if (!a)
		goto out;
	a->fd = -1;
	a->buffer.fd = -1;
	a->length = ((uint64_t)bytes + 4095) & ~(uint64_t)4095;
	if (m->type == 3) {
		a->buffer.size = a->length;
		if (buffer_request(MT6580_VCODEC_ALLOC, &a->buffer))
			goto out;
		imported = 1;
		a->va = mmap(NULL, a->length, PROT_READ | PROT_WRITE, MAP_SHARED,
			     codec_fd, a->buffer.mmap_offset);
	} else {
		heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
		if (heap_fd < 0)
			goto out;
		heap.len = a->length;
		if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &heap))
			goto out;
		a->fd = heap.fd;
		a->buffer.fd = a->fd;
		if (buffer_request(MT6580_VCODEC_IMPORT, &a->buffer))
			goto out;
		imported = 1;
		a->va = mmap(NULL, a->length, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd, 0);
	}
	if (a->va == MAP_FAILED) {
		a->va = NULL;
		goto out;
	}
	if (sync_cpu(a, 1))
		goto out;
	m->signature = 0x56636f64;
	m->size = bytes;
	m->va = m->aligned_va = (uintptr_t)a->va;
	m->iova = m->aligned_iova = a->buffer.iova;
	m->share_fd = a->fd;
	m->ion_handle = a->buffer.id;
	m->reserved = 0;
	m->reserved_size = a->buffer.id; /* opaque ownership token, never KVA */
	m->ion_fd = codec_fd;
	a->next = allocations;
	allocations = a;
	fprintf(stderr, "VAL_ALLOC type=%u bytes=%llu id=%u iova=%08llx\n",
		m->type, (unsigned long long)m->size, a->buffer.id,
		(unsigned long long)a->buffer.iova);
	a = NULL;
	rc = VAL_OK;
out:
	if (a) {
		if (a->va)
			munmap(a->va, a->length);
		if (imported)
			release_buffer(a->buffer.id);
		if (a->fd >= 0)
			close(a->fd);
		free(a);
	}
	if (heap_fd >= 0)
		close(heap_fd);
	if (rc)
		fprintf(stderr, "VAL_ALLOC_FAILED rc=%d errno=%d\n", rc, errno);
	pthread_mutex_unlock(&lock);
	return rc;
}

int eVideoMemFree(struct val_memory *m, uint32_t size)
{
	struct allocation **p, *a;
	int rc = VAL_INVALID_MEMORY;
	if (owner_pid != getpid() || !m || size != sizeof(*m))
		return VAL_INVALID_PARAMETER;
	pthread_mutex_lock(&lock);
	for (p = &allocations; (a = *p); p = &a->next) {
		if ((uint32_t)m->va != (uintptr_t)a->va ||
		    (uint32_t)m->ion_handle != a->buffer.id)
			continue;
		if (sync_cpu(a, 0) || release_buffer(a->buffer.id))
			break;
		*p = a->next;
		/* A caller may place its descriptor in the allocation itself. */
		m->va = m->iova = m->aligned_va = m->aligned_iova = 0;
		m->ion_handle = m->reserved_size = 0;
		m->share_fd = UINT32_MAX;
		munmap(a->va, a->length);
		if (a->fd >= 0)
			close(a->fd);
		free(a);
		rc = VAL_OK;
		break;
	}
	pthread_mutex_unlock(&lock);
	return rc;
}

static unsigned char mva_cookie;
static unsigned int mva_users;
uint32_t eVideoInitMVA(void **handle)
{
	if (owner_pid != getpid() || !handle)
		return UINT32_MAX;
	pthread_mutex_lock(&lock);
	mva_users++;
	*handle = &mva_cookie;
	pthread_mutex_unlock(&lock);
	return 0;
}
uint32_t eVideoDeInitMVA(void *handle)
{
	uint32_t rc = UINT32_MAX;
	if (owner_pid != getpid())
		return rc;
	pthread_mutex_lock(&lock);
	if (handle == &mva_cookie && mva_users) {
		mva_users--;
		rc = 0;
	}
	pthread_mutex_unlock(&lock);
	return rc;
}

uint32_t eVideoAllocMVA(void *handle, uint32_t va, uint32_t *iova,
		       uint32_t size, const void *config)
{
	struct allocation *a;
	uint32_t rc = UINT32_MAX;
	const uint32_t *cfg = config;
	if (owner_pid != getpid() || !iova || !size)
		return rc;
	pthread_mutex_lock(&lock);
	/* Only ordinary, nonsecure decoder buffers are supported. */
	if (handle != &mva_cookie || !mva_users ||
	    (cfg && (cfg[0] != 1 || cfg[1] || cfg[2]))) {
		pthread_mutex_unlock(&lock);
		return rc;
	}
	a = find_range(va, size);
	if (a) {
		*iova = a->buffer.iova + va - (uintptr_t)a->va;
		rc = 0;
	}
	pthread_mutex_unlock(&lock);
	return rc;
}

uint32_t eVideoFreeMVA(void *handle, uint32_t va, uint32_t iova,
		      uint32_t size, const void *config)
{
	uint32_t expected;
	/* The allocation owns its stable mapping until eVideoMemFree. */
	if (eVideoAllocMVA(handle, va, &expected, size, config) || iova != expected)
		return UINT32_MAX;
	return 0;
}

static int cache_access(struct val_memory *m, uint32_t size, uint32_t operation, int start)
{
	struct allocation *a;
	int rc = VAL_INVALID_MEMORY;
	if (owner_pid != getpid() || operation > 1 ||
	    (operation == 1 && (!m || size != sizeof(*m))))
		return VAL_INVALID_PARAMETER;
	pthread_mutex_lock(&lock);
	if (!operation) {
		rc = VAL_OK;
		for (a = allocations; a; a = a->next)
			if (sync_cpu(a, start))
				rc = VAL_INVALID_MEMORY;
		pthread_mutex_unlock(&lock);
		return rc;
	}
	a = find_range((uint32_t)m->va, (uint32_t)m->size);
	if (a && !sync_cpu(a, start))
		rc = VAL_OK;
	pthread_mutex_unlock(&lock);
	return rc;
}
int eVideoFlushCache(struct val_memory *m, uint32_t size, uint32_t operation)
{
	return cache_access(m, size, operation, 0);
}
int eVideoInvalidateCache(struct val_memory *m, uint32_t size, uint32_t operation)
{
	return cache_access(m, size, operation, 1);
}

__attribute__((destructor)) static void release_all(void)
{
	struct allocation *a;
	/* A fork child shares kernel handles with its parent; never RELEASE them. */
	if (owner_pid != getpid())
		return;
	pthread_mutex_lock(&lock);
	while ((a = allocations)) {
		allocations = a->next;
		sync_cpu(a, 0);
		release_buffer(a->buffer.id);
		munmap(a->va, a->length);
		if (a->fd >= 0)
			close(a->fd);
		free(a);
	}
	if (codec_fd >= 0)
		close(codec_fd);
	pthread_mutex_unlock(&lock);
}
