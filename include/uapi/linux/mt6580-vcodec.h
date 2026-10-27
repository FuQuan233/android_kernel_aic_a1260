/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MT6580_VCODEC_H
#define _UAPI_LINUX_MT6580_VCODEC_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * struct mt6580_vcodec_buffer - owned codec DMA buffer
 * @fd: IMPORT input DMA-BUF fd; ALLOC input/output -1; RELEASE must be 0.
 * @id: output handle, scoped to this open file; RELEASE input handle.
 * @size: IMPORT output whole DMA-BUF length; ALLOC input requested length
 *        and output page-rounded length; RELEASE must be 0.
 * @iova: output contiguous 32-bit device address; never a CPU address.
 * @mmap_offset: ALLOC output opaque mmap64 offset, IMPORT output 0.
 *
 * IMPORT requires all fields except fd to be zero. ALLOC requires id, iova
 * and mmap_offset to be zero. RELEASE requires all fields except id to be
 * zero. Handles are not reused within an open file. RELEASE removes the
 * handle, but an existing coherent mmap remains valid until its last unmap;
 * that memory continues to count against the per-file quota meanwhile.
 * IMPORT buffers are mapped through their original DMA-BUF fd, with CPU
 * access bracketed by DMA_BUF_IOCTL_SYNC. These ioctls do not submit work,
 * program registers, or synchronize completion of a hardware job.
 */
struct mt6580_vcodec_buffer {
	__s32 fd;
	__u32 id;
	__aligned_u64 size;
	__aligned_u64 iova;
	__aligned_u64 mmap_offset;
};

#define MT6580_VCODEC_IMPORT \
	_IOWR('V', 0x80, struct mt6580_vcodec_buffer)
#define MT6580_VCODEC_ALLOC \
	_IOWR('V', 0x81, struct mt6580_vcodec_buffer)
#define MT6580_VCODEC_RELEASE \
	_IOW('V', 0x82, struct mt6580_vcodec_buffer)

#endif /* _UAPI_LINUX_MT6580_VCODEC_H */
