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

/* Native privileged hybrid H.264 interface; no legacy ioctl emulation. */
#define MT6580_VCODEC_H264_DRIVER 6
#define MT6580_VCODEC_OAL_MAX_REGS 16
/* mmap2's page offset is 32-bit on ARM. Reserve its upper half for MMIO. */
#define MT6580_VCODEC_MMAP_REGS (1ULL << 43)
/* Logical offsets concatenate two resources, never the physical gap. */
#define MT6580_VCODEC_REG_AUX_BASE 0x2000U
#define MT6580_VCODEC_MMAP_AUX \
	(MT6580_VCODEC_MMAP_REGS + MT6580_VCODEC_REG_AUX_BASE)

/**
 * struct mt6580_vcodec_oal - bind owned coherent memory to the hybrid ISR
 * @header_id: buffer handle containing the 16-byte ARM32 OAL header
 * @header_offset: byte offset, aligned to 4
 * @status_id: buffer handle containing count 8-byte address/value entries
 * @status_offset: byte offset, aligned to 4
 * @count: number of entries, at most 16; 0 permits status_id/status_offset 0
 * @register_offsets: validated logical byte offsets; unused entries must be 0
 *
 * Offsets 0x0000..0x1ffc select VDEC (physical 0x1500b000, 8 KiB).
 * Offsets 0x2000..0x2ffc select MP4-VENC auxiliary registers (physical
 * 0x15009000, 4 KiB). Each read is 4-byte aligned and confined to one
 * resource. mmap at MMAP_REGS may cover the two VDEC pages or its second
 * page alone; mmap at MMAP_AUX may cover the one auxiliary page. A mapping
 * spanning the logical boundary between resources is rejected.
 *
 * The OAL header is four u32 words: completed, timeout, count, status_ptr.
 * Only owned coherent buffers are accepted. The registered offsets, not
 * user-provided addresses, select registers to read. Each status entry's
 * address word and header status_ptr are preserved, never dereferenced.
 * Results/timeout precede release publication of completed=1. Each acquired
 * job retains an immutable binding snapshot. BIND/UNBIND may replace the
 * default binding without changing active jobs; they never program registers.
 * Callers must serialize their BIND+ACQUIRE pair when sharing one file.
 */
struct mt6580_vcodec_oal {
	__u32 header_id;
	__u32 header_offset;
	__u32 status_id;
	__u32 status_offset;
	__u32 count;
	__u32 register_offsets[MT6580_VCODEC_OAL_MAX_REGS];
};

/**
 * struct mt6580_vcodec_job - one hybrid H.264 hardware lock/completion
 * @driver_type: must be MT6580_VCODEC_H264_DRIVER
 * @timeout_ms: ACQUIRE lock wait or WAIT completion wait, at most 30000 ms;
 *              0 is a nonblocking check (WAIT returns EAGAIN while pending,
 *              without cancelling); RELEASE requires 0
 * @handle: nonzero per-file context cookie, also matched by WAIT/RELEASE
 * @irq_status: WAIT output raw VDEC IRQ status; input must be 0
 * @reserved: must be 0
 *
 * ACQUIRE enables the real power/clock chain and IRQ, holding a wake source.
 * Userspace may then access only its mapped VDEC/MP4-VENC windows. MMIO VMAs
 * themselves do not hold power: accessing them outside an acquired job is
 * forbidden by this privileged interface's caller contract. IRQ completion masks
 * IRQ and releases power automatically. A timed-out WAIT or explicit RELEASE
 * synchronizes IRQ before abort/power release. Successful WAIT does not erase
 * the result; RELEASE frees its record, returning EALREADY after completion.
 * ACQUIRE may replace a finished record with the same cookie. Completed
 * records are bounded to 64 per open file; callers must release old cookies.
 * An abort requires a new real ISP-domain OFF notification. If stopping
 * cannot be proven, or power release fails, the device latches a fault,
 * retains all possibly active DMA buffers and its wake source until reboot,
 * and rejects subsequent acquisitions/buffer operations with EIO.
 * No decode work is fabricated for an unknown/unsupported request.
 */
struct mt6580_vcodec_job {
	__u32 driver_type;
	__u32 timeout_ms;
	__aligned_u64 handle;
	__u32 irq_status;
	__u32 reserved;
};

#define MT6580_VCODEC_OAL_BIND \
	_IOW('V', 0x83, struct mt6580_vcodec_oal)
#define MT6580_VCODEC_JOB_ACQUIRE \
	_IOW('V', 0x84, struct mt6580_vcodec_job)
#define MT6580_VCODEC_JOB_WAIT \
	_IOWR('V', 0x85, struct mt6580_vcodec_job)
#define MT6580_VCODEC_JOB_RELEASE \
	_IOW('V', 0x86, struct mt6580_vcodec_job)
#define MT6580_VCODEC_OAL_UNBIND _IO('V', 0x87)

#endif /* _UAPI_LINUX_MT6580_VCODEC_H */
