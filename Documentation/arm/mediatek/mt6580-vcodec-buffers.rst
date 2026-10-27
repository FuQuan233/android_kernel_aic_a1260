MT6580 codec buffer bridge
=========================

Scope and ABI
-------------

This implements buffer preparation for the stock ARM32 decoder. It does not
create a V4L2/Codec2/OMX media service, program VDEC registers, enable its
functional gate, or submit decode jobs. The Linux device is deliberately
/dev/mt6580-vcodec, not a placeholder /dev/Vcodec returning success for
unimplemented legacy commands. Access requires root plus CAP_SYS_RAWIO.

The ioctl structure and field initialization rules are in
include/uapi/linux/mt6580-vcodec.h. IMPORT uses the DMA-BUF API and validates
that every DMA segment forms one continuous 32-bit IOVA range covering the
whole buffer. ALLOC uses dma_alloc_coherent and returns an opaque mmap64
offset. RELEASE operates on an id owned by that file, never on a user
supplied kernel pointer. Ids are not recycled. VMAs hold buffer references
across fork, splitting, handle RELEASE and fd close. Per-file charged
memory remains limited to 64 MiB until the last VMA releases it; coherent
requests are individually limited to 32 MiB. Failed operations unwind all
DMA mappings/attachments. A buffer or open fd does not hold ISP powered.

The device node describes the BSP VDEC MISC/VLD two-page window at
0x1500b000, SPI137 level-low and LARB1 POST0/CDMA/WDMA ports 3/9/10. The
current driver does not map the MMIO window or request/enable the IRQ.
The existing CLK_SMI SRAM handshake dependency still applies to ISP.

ARM32 shared DMA address space
------------------------------

Build #171 exposed an existing ARM32 DMA setup limitation once RDMA and
codec were declared together. They are in one hardware IOMMU group, with
one M4U page table. arm_setup_iommu_dma_ops used iommu_attach_device, which
correctly rejects groups with more than one member. Both attaches failed;
the first 2 MiB DMA-BUF import was rejected with ERANGE by the continuity
check. The original native-171.txt and dmesg-import-failed-171.txt are
retained. A non-NULL default domain alone is not evidence of IOMMU DMA ops;
the bridge also requires the ARM DMA mapping to have been installed.

The automatic ARM DMA setup now caches one mapping per multi-device group
under a private mutex, attaches the group once and shares the page table
AND bitmap IOVA allocator. Splitting the group into independent allocators
would risk address collisions and is not used. Different DMA apertures are
rejected rather than replacing live mappings. The group has one base
reference and each automatically attached member keeps the existing two
create/attach ownership references. Intermediate detach clears only that
device's DMA ops; the last member detaches the group. Exported detach plus
release semantics are retained. A manual attachment of the same mapping
to another group does not decrement the automatic group's member count.
Single-device setup keeps its original allocation path. No group's
single-device isolation check is relaxed and no driver's group-private data
slot is borrowed. Core DMA ownership must also recognize ARM's automatic
UNMANAGED domain installed over the identity default: otherwise the second
consumer is rejected before its driver probe with EBUSY. An immutable
automatic marker plus the current domain and actual ARM IOMMU DMA ops are
checked; explicit manual domains do not qualify. Existing owner/PASID
checks and owner counting remain intact. This is not a driver-managed DMA
flag bypass. Builds #174/#175 retain this pre-probe failure evidence.

Build #176 then imported real DMA-BUF memory but rejected coherent mmap
because the VMA has the kernel's default empty vma_dummy_vm_ops. Both pre-
and post-DMA-map guards now permit that empty default while refusing a
backend's private handlers. The #176/#177 failed fixture outputs are
retained; they are not counted as successful coherent lifetime tests.

This static MT6580 group is the hardware qualification scope. Other ARM
platforms, mixed-aperture groups, hotplug/single-to-multiple migration and
manual detach/re-attach consumers are not hardware qualified. The latter
have source review only. A new multi-device setup cannot replace an
already-owned single-device domain; that case may fail safely with EBUSY.

Private stock VAL adapter
------------------------

The source-only tools/mt6580-vcodec adapter preloads into an isolated
process, not the system media stack. Its 96-byte VAL descriptor follows
val_types_public.h:326..372 and the actual stock eVideoMemAlloc accesses.
ARM32 callers initialize only the low 32-bit union word. Normal kinds 0..2
use the real system DMA heap and IMPORT; kind3 uses coherent ALLOC. The
old reservedSize kernel-address leak is replaced by an opaque owned id.
VA-to-MVA lookup only accepts bridge-owned ranges and a live InitMVA
handle, with nonsecure/noncoherent decoder configuration. Alignment over
4096 and unknown addresses/configuration fail explicitly.

Streaming flush/invalidate issue real DMA_BUF_IOCTL_SYNC operations every
time; range operations cover their whole allocation, while optype0 covers
all tracked allocations. Coherent memory uses a CPU barrier. These are
cache operations, not job completion fences, and actual decoder cache
ownership has not been qualified. Fork children cannot RELEASE inherited
parent handles through the adapter. Controlled private library lifetime is
required; this is not a general concurrent media-plugin unloading API.

Stock Create stores its copied VAL descriptor in instance+0x48; Release
copies that descriptor back to a stack object before freeing. REA evidence
for this is rea-create.json and rea-release.json in the preceding vcodec
investigation. Additional eVDecDrvInit decompilation is retained as
vcodec-next/rea-init-driver.json, Evidence
ev_452e0b835721754aa570064360fd27131ec26426c468f8eb92ee26c708abb9eb.
It identifies VAL driver type6, the HAL instance and hybrid init dispatch;
no successful decoder initialization is inferred from static analysis.

Qualification evidence is retained in out/a1260-runtime-pm/vcodec-next.
The remaining integration is legacy Vcodec locks/shared OAL/validated IRQ
results and actual submit/completion/cache ownership, then known-bitstream
frame comparison, followed by Android media integration. Proprietary
libraries are not included in this repository.

Build #178 buffer qualification
-------------------------------

native-178.txt imports a 2 MiB system DMA-BUF at real IOVA 0x00c00000 and
allocates an 8 KiB coherent buffer at IOVA 0x001a0000. Foreign-file handles,
double release, oversize/stale mmap, forked shared content, and continued
access after RELEASE/fd close pass; final vmas/buffers/bytes are 0/0/0.
These are CPU/cache-API/lifetime checks, not decoder DMA traffic.

stock-create-178.txt exercises all four memory kinds with a 65537-byte
request and dirty input-union high words, range mapping, synchronization and
free. Then the real stock H.264 Create/Release completes 32 rounds. The
adapter logs all 32 actual 864-byte descriptor allocations; total kernel
counters become allocations2/imports36/mmaps3 with live counts zero. This
establishes actual interposition and release, beyond the preceding metadata
query. It still does not call Init or decode a bitstream.

The combined qualification-178.txt repeats the native and stock checks
after two full deep cycles (counter stopped 8.892298497/9.052424153 seconds,
CPU0/L2 cold2, 8 MiB RAM hash unchanged). The second round initially aborted
with alarmtimer -EBUSY due to an approaching Android alarm, then retried
successfully: suspend success/fail is 2/1, not 2/0. Four LARB1 runtime
cycles retain VC2/eleven OSTD1 and MMU bitmap0x608 for the declared decoder
ports, with VCODEC bit9 still off. GPU renders 1820 frames, green readback
and GL error0. Aligned 4 MiB eMMC write and two reads match SHA256
948086aedc1a05658ab645f2a1c685e4af5b2b08f7b178bdd5e391178764d91b,
at 8-bit/52 MHz/error0. Final cumulative bridge counters are coherent4,
imports72, mappings6, live VMA/buffer/bytes0. M4U faults/TLB timeouts and
RDMA underflow/abnormal remain zero, taint0. Final MD1/ISP/DISP/MFG domains
are off, including the codec device runtime suspended. No retained codec
buffer across deep, actual decoder IRQ/DMA, physical current or fault
injection is claimed. The standard product boot/kernel are #178; private
libraries/scripts are removed and ordinary shell/default CPU/deep policy
restored. Full dt-schema validation tools were unavailable; binding YAML
syntax and normal kernel/DTB compilation passed.

Native hybrid job bring-up (#179--#181)
-------------------------------------

The native UAPI now binds owned coherent OAL memory, serializes actual
H.264 hardware ownership, enables the VCODEC/ISP/LARB supplier chain and
waits for the real level-low IRQ. Validated register-result offsets select
only the two declared resources. The VDEC MISC/VLD window remains at
0x1500b000 (8 KiB); the XA H.264 binary additionally accesses MP4-VENC at
0x15009000 (4 KiB). It is mapped as a separate resource and opaque AUX
mmap token, never as a larger physical mapping across intervening blocks.
MMIO requires CAP_SYS_RAWIO, a shared non-executable mapping and an active
job by the private caller contract. MMIO VMAs do not retain power.

The 64-bit opaque mmap token needs FOP_UNSIGNED_OFFSET on ARM32: without
it mm/mmap.c rejects this character-device offset with EOVERFLOW before
the driver's mmap callback. #180 verified the corrected VDEC mapping;
#181 adds the exact auxiliary mapping needed by the actual decoder.

An IRQ completion masks IRQ, publishes coherent results/completed and
releases clocks/runtime PM. A timeout, cancellation or file close masks
and synchronizes IRQ before power release. The old BSP does not establish
a usable reset sequence, so an abort must observe a fresh real ISP OFF
notification. If stopping cannot be proven, all possibly active buffers
(including RELEASEd but still mapped objects) are quarantined and a wake
source is retained until reboot. No unsafe DMA mapping is freed as though
cancellation alone were proof that the hardware stopped.

The isolated legacy adapter translates actual locks, thread registration,
OAL binding, register mappings and WAIT into this native ABI. It does not
fabricate a successful completion. The 8.1 driver requires SetParam(0xf)
with the packed Linux OMX TIDs before Init. External frame descriptors
are 0x88 bytes. Input and DPB descriptors remain stable until their distinct
free-input and free-frame queries return them; display notification alone
does not release a referenced DPB frame.

As with the earlier GPU port, the original DRVB library validates this
device's real dev_info words. The private packaging adapts only the old
sysfs pathname to the existing same-device /vendor/etc/mt6580-devinfo.bin.
It preserves the validation code. Missing that input produced error 0x1004
and the original library's deliberate 0xddeeaadd caller-stack poisoning;
no memory allocator padding or disabled validation is used to hide it.

The successful evidence is vcodec-next/decode-181-aux.txt: eight Baseline
128x96 non-I_PCM H.264 frames match an independent libavc software-decoded
I420 reference exactly. IRQs increase by eight during Decode (excluding
Init), timeouts/cancellations/spurious/power errors stay zero, and DeInit /
Release leave VMA/buffer/byte counts at zero. The private socket worker
then passes 20 reset/EOS cycles (160 frames) and two continuous 128-frame
runs, matching pixels, originating tokens and PTS. The cumulative hardware
count is 432 jobs/432 IRQs, no faults, taint0 and no remaining live buffers.
This establishes native hardware decode and repeated descriptor reuse;
Android MediaCodec integration is still being qualified separately.

All proprietary DSOs and same-device efuses remain outside this repository.

Build #182 corrects zero-timeout WAIT into a non-cancelling EAGAIN poll.
job-poll-182.txt checks an acquired empty lock (no DMA command), unchanged
pending OAL words, then explicit cancellation with a fresh ISP OFF and no
live objects. Its one cancellation is deliberate, not a decode timeout.
worker-182-720p.txt then verifies eight 1280x720 Baseline frames against
independent I420 pixels/token/PTS with eight real IRQs, timeouts0 and live0.
The 8.1 library actually selects its dual-thread XB implementation for this
sample; this is not a performance or all-profile/stream qualification.

worker-182-720p-final.txt repeats two continuous 32-frame 720p runs with the
final adapter (unused/uninitialized thread-array words are no longer copied
or logged). All 64 frames/token/PTS match; cumulative #182 counters become
jobs73/IRQs72/timeouts0/cancellations1 (the preceding explicit empty-lock
check), no faults/power errors and live0. Android integration remains pending.
