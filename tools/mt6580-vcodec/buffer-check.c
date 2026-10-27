// SPDX-License-Identifier: GPL-2.0-only
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../include/uapi/linux/mt6580-vcodec.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d %s errno=%d\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static void release_buffer(int fd, uint32_t id)
{
	struct mt6580_vcodec_buffer q = { .id = id };
	CHECK(ioctl(fd, MT6580_VCODEC_RELEASE, &q) == 0);
}
int main(void)
{
	struct dma_heap_allocation_data heap = { .len = 2 * 1024 * 1024,
		.fd_flags = O_RDWR | O_CLOEXEC };
	struct mt6580_vcodec_buffer q = {0}, stale;
	struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
	int fd, second, hfd, status;
	volatile uint32_t *data;
	pid_t child;
	setbuf(stdout, NULL);
	fd = open("/dev/mt6580-vcodec", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);
	second = open("/dev/mt6580-vcodec", O_RDWR | O_CLOEXEC);
	CHECK(second >= 0);
	hfd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	CHECK(hfd >= 0);
	CHECK(ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &heap) == 0);
	q.fd = heap.fd;
	CHECK(ioctl(fd, MT6580_VCODEC_IMPORT, &q) == 0);
	CHECK(q.size == heap.len && q.iova && q.iova + q.size <= 0x100000000ULL);
	printf("IMPORT id=%u bytes=%llu real_iova=%08llx\n", q.id,
	       (unsigned long long)q.size, (unsigned long long)q.iova);
	stale = (struct mt6580_vcodec_buffer){ .id = q.id };
	errno = 0;
	CHECK(ioctl(second, MT6580_VCODEC_RELEASE, &stale) < 0 && errno == ENOENT);
	data = mmap(NULL, q.size, PROT_READ | PROT_WRITE, MAP_SHARED, heap.fd, 0);
	CHECK(data != MAP_FAILED);
	CHECK(ioctl(heap.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
	for (uint32_t i = 0; i < q.size / 4; i++) data[i] = i ^ 0xa1260658;
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
	CHECK(ioctl(heap.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
	CHECK(ioctl(heap.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
	for (uint32_t i = 0; i < q.size / 4; i++) CHECK(data[i] == (i ^ 0xa1260658));
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
	CHECK(ioctl(heap.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0);
	CHECK(munmap((void *)data, q.size) == 0);
	release_buffer(fd, q.id);
	errno = 0;
	CHECK(ioctl(fd, MT6580_VCODEC_RELEASE, &stale) < 0 && errno == ENOENT);
	close(heap.fd);
	close(hfd);

	q = (struct mt6580_vcodec_buffer){ .fd = -1, .size = 8192 };
	CHECK(ioctl(fd, MT6580_VCODEC_ALLOC, &q) == 0);
	CHECK(q.size == 8192 && q.iova && q.mmap_offset);
	printf("COHERENT id=%u bytes=%llu real_iova=%08llx offset=%08llx\n", q.id,
	       (unsigned long long)q.size, (unsigned long long)q.iova,
	       (unsigned long long)q.mmap_offset);
	CHECK(mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fd, q.mmap_offset) == MAP_FAILED);
	data = mmap(NULL, q.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, q.mmap_offset);
	CHECK(data != MAP_FAILED);
	data[0] = 0x12345678;
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (data[0] != 0x12345678) _exit(3);
		data[1024] = 0x6580a126;
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(data[1024] == 0x6580a126);
	release_buffer(fd, q.id);
	CHECK(mmap(NULL, q.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fd, q.mmap_offset) == MAP_FAILED);
	close(fd);
	close(second);
	/* RELEASE and close must not invalidate a still-mapped coherent buffer. */
	CHECK(data[0] == 0x12345678 && data[1024] == 0x6580a126);
	data[2047] = 0xcafe6580;
	CHECK(data[2047] == 0xcafe6580);
	CHECK(munmap((void *)data, q.size) == 0);
	puts("BUFFER_CHECK_COMPLETE (mapping/cache/lifetime; no decoder DMA job)");
	return 0;
}
