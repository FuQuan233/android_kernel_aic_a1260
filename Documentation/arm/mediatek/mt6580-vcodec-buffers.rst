MT6580 codec buffer bridge
=========================

Scope and ABI
-------------

This implements privileged owned buffers and real serialized codec jobs for
stock ARM32 H.264 decoding and MPEG4/H263 hybrid encoding. Android Codec2
services and the isolated private binary workers live in the device repository.
The kernel provides power/clock ownership, bounded MMIO resources, actual IRQ
completion and abort lifetime protection; it does not implement a generic V4L2
or legacy OMX service. /dev/mt6580-vcodec never fabricates success for unknown
/dev/Vcodec commands. Device-node permission plus CAP_SYS_RAWIO is required.

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

The device node declares VDEC at 0x1500b000 (8 KiB), MP4-VENC at 0x15009000
(4 KiB), SPI137/138 level-low, and the actual LARB1 codec ports 3/7/8/9/10.
The two engines share one job owner because H.264 also uses the VENC auxiliary
window. The existing CLK_SMI SRAM-handshake dependency applies to ISP.

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


Android Codec2 qualification (#182)
----------------------------------

The product now registers c2.mt6580.avc.decoder through Codec2 AIDL V2.
The modern frontend exchanges bounded messages with a separately privileged
mediacodec worker (CAP_SYS_RAWIO, not UID 0), which privately loads the owned
8.1 decoder libraries. Ordinary shell MediaCodec automatically selects the
hardware component. The legacy buffer adapter opens the heap node O_RDONLY,
matching AOSP 0444 heap permissions; allocated DMA-BUFs remain writable.

mediacodec-yuv-final.txt and mediacodec-720-final.txt each verify 48 real
MediaCodec frames at 128x96 and 1280x720 against independent libavc I420
pixels. Original PTS, data/empty EOS, flush, stop/reconfigure/start and repeated
create/release pass. surface-yuv-final.txt verifies eight frames through a
real SurfaceTexture, Mali-400 MP external texture and changing GLES readback.
All 104 submitted frames have 104 real hardware IRQs, no timeouts,
cancellations, faults or power errors, and zero live VMA/buffer/bytes after
release. The frontend uses explicit CPU copies; this is not a zero-copy or
throughput qualification. Current scope is one progressive H.264 Baseline /
ConstrainedBaseline stream up to 1280x720. Main/High, other codecs,
interlaced/secure/adaptive playback and retained decode across deep are not
qualified. Standard product boot/kernel are #182 with the new vendor image.

The minigbm bridge adds linear YV12 without changing the original 168-byte,
two-FD Mali handle ABI. Logical Y/V/U plane metadata shares one image FD;
YUV is GPU composed rather than advertised for direct KMS scanout. The device
repository keeps the source patch and the local private-DSO packaging tool.
No proprietary decoder library or efuse contents are committed here.


Main/High profile and B-frame qualification (#183)
------------------------------------------------

Real VideoLAN x264 Main/High samples contain CABAC, B frames and (for High)
8x8 transforms. Independent libavc and FFmpeg agree on the I420 reference.
The worker bounds SPS parsing, admits only 8-bit 4:2:0 progressive video up to
1280x720, and handles the High scaling-list syntax without guessing layouts.

The original decoder defaults h264dec.simp_b_frame to a simplified B-frame
path. The initial Main sample differed in 86 bytes (maximum error 23) in one
frame; this was not accepted as rounding tolerance. Selecting the original
simp_b_frame=0 control removed the differences. The private VAL adapter now
selects only this quality control locally; other property reads retain normal
system/default behavior and DRVB/devinfo validation is preserved. No global
quality property is required. This is not a simulated decode or IRQ.

Codec2 must notify a current work's picture immediately, using an INCOMPLETE
clone followed by its terminal empty work. Merely filling the current work can
delay B2's callback until after finish() has delivered a later P3 reference
picture within the same transaction. The corrected callback order is tested
against the original AU PTS, not coded-input FIFO order.

Main and High each pass 24 frames at 128x96 and at 1280x720: all 96 pictures
are pixel-exact, display PTS increases correctly, and data/empty EOS, flush and
stop/reconfigure/start pass. Baseline's 48-frame lifecycle and the eight-frame
Mali external-texture/GLES readback regression also pass on #183. Evidence is
vcodec-profiles/*-final.txt, main-small-order-quality.txt, baseline-183.txt and
surface-183.txt. No interlaced, >8-bit, adaptive-resolution or zero-copy path is
claimed by these tests.

MPEG4/H263 native engine (#183)
------------------------------

Native type 1 and type 6 jobs use the same device-wide owner, with immutable
per-job engine selection and correct-engine ACQUIRE/WAIT/RELEASE checks.
VENC uses auxiliary status +0x67c, ACK +0x678 written with 1, and enable +0x668;
it does not use another SoC's AVC encoder +0x5c/+0x60 offsets. IRQ masking and
lock-external synchronize_irq follow the selected engine. Both paths retain
the same real ISP OFF proof and quarantine-on-unproven-stop contract.

GET_CAPS is a 16-byte read-only native query. It verifies the actual ARM DMA
mapping/domain and derives the port mask from this consumer's current IOMMU
fwspec IDs. The required LARB1 ports must be bound; a changed or missing binding
is rejected. The private adapter accepts only the recovered nonsecure virtual
M4U-array configuration and verifies this query, rather than pretending that
an absent legacy /dev/m4u was configured.

The first MPEG4 native probe produces eight 128x96 frames with eight real VENC
IRQs; the decoder IRQ does not advance. FFmpeg independently decodes eight
pictures, with input/output PSNR about 48--51 dB. No timeouts/cancellations,
faults/power errors or retained buffers remain. See venc-analysis/native-first.txt
and native-first-quality.json. This alone does not qualify the asynchronous
encoder worker or the Android encoder components; their later results must be
recorded separately. The owner's MT6580 AVC encoder selects the original
software backend, so it is never declared to be hardware AVC encoding.

The asynchronous encoder worker later passes two 200-frame runs, including
slot reuse and RESET/drain. A five-configuration qualification covers Simple
Level 0 at 32 kbit/s, Level 6 at 128x96 and 1280x720/30 fps, and H263 QCIF/CIF.
All forty frames independently decode with their original frame identity.
The wrapper's public level field is ignored by its backend: a per-instance
Open adapter invokes the real profile/level controls before Init and verifies
the generated VOS byte without rewriting the stream. Original OMX evidence
also establishes frame descriptor word 12 = timestamp high, word 13 = low;
correcting those words restores increasing MPEG4 VOP timestamps. The backend's
GetNextBitstream timestamp remains zero, so output association uses the
qualified no-B FIFO rather than claiming a returned timestamp.

The original FINAL shim passes partly uninitialized input to EncodeOneUnit.
The worker populates the complete recovered 27-word FINAL input from retained
and validated buffers/stride/ring state, then invokes the actual backend and
GetNextBitstream. It does not rely on stale stack data or patch codec code.
The legacy control adapter releases the last native job record before OAL
DeInit, preserving the native IRQ synchronization/OFF-proof cancellation path;
this prevents completed cookies accumulating across instance reconstruction.
Host-forwarded socket partial-payload failures and the initial FINAL failure
are retained as failures, separate from complete qualification runs.

Android Codec2 qualification subsequently passes 96 ordinary-shell SDK
inputImage frames across four configurations (MPEG4 L0/64k, L0/32k,
L6 1280x720/4M, and H263 QCIF). Every configuration exercises data/empty EOS,
stop/restart and release/recreate. Independent FFmpeg decoding verifies source
identity, dimensions, packet PTS and actual MPEG4 levels. Both encoders also
pass eight real Mali GLES input-Surface frames, with decoded RGB mean errors
at most 6/255 and 3/255. Vendor-store flexible format descriptors select the
qualified YV12 CPU buffer path; privileged MMIO remains in separate workers.
Final store regression passes High720 CABAC/B-frame exact decoding and the
H264-to-Mali Surface path. Native totals end at 160 jobs/IRQs with zero
faults/timeouts/cancellations/power errors and zero live buffer ownership.
Product boot/kernel use #183 and vendor contains the qualified components.
Sustained throughput/rate-control precision, zero-copy and retained codec
jobs across deep are not claimed. H263 raw temporal references remain the
stock backend behavior; use the original C2/container presentation timestamps.
