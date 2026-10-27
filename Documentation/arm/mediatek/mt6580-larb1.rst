MT6580 LARB1, ISP gates and decoder preparation
=============================================

Ownership and reference
-----------------------

The reference is android_kernel_mediatek_k80, MT6580 clock manager,
drivers/misc/mediatek/m4u/mt6580 and smi/smi_configuration.c. The SMI Makefile
selects SMI_R for this SoC. Its normal profile supplies COMMON+0x108=0x1001,
LARB1 virtual channel 2 and eleven OSTD entries of 1. Tables selected for
other SoCs are not used. The eleven ports cover camera and shared encoder/
decoder traffic; their presence does not instantiate a camera or codec.

IMGSYS at 0x15000000 owns CG_CON/SET/CLR +0/+4/+8, LARB1 bit0 and VCODEC
bit9. Its CCF provider uses runtime PM in the ISP domain. LARB1 at
0x15001000 links to COMMON, and is registered with the existing M4U. The
upstream codec clock routing remains unconfirmed; no CAM or PLL parent is
guessed. COMMON's supplier link retains the verified SMI bus source.

LARB1 saves nineteen control/QoS/security offsets, including all eleven
OSTD entries and the two domain-nibble groups. LARB0 keeps its original
six-port/fifteen-offset layout. Only declared Linux translated ports are
made nonsecure/domain3. Diagnostics read cached values without waking ISP.
Initial context acquisition resumes the complete device/supplier chain,
not just the local gate. No decoder device or video node is declared.

Deep-resume failure and correction
---------------------------------

Build #169 passed active LARB1 gating, but its first real deep cycle was
followed by an ISP SRAM handshake timeout. The second attempt fell back to
retained power with ISP active; this is not counted as two deep passes.
The retained trace shows generic genpd restoring ISP during noirq even
though the COMMON consumer remains runtime suspended. ISP previously had
CLK_NONE. Matching the BSP's COMMON-before-ISP dependency, #170 adds the
real CLK_SMI reference to ISP power sequencing, including SRAM handshakes.
No generic genpd behavior is changed.

Evidence is in out/a1260-runtime-pm/vcodec. sleep-169.txt, trace-169.txt,
power-169.trace and dmesg-169-after-sleep.txt retain the failure. Only the
first raw word in isp-failed-169.txt is ISP_PWR_CON; its next two words
were accidentally sampled from TOP, not SPM, and are not power evidence.

Build #170 qualification
-----------------------

Two complete deep cycles stopped the counter for 9.488459386 and
9.595393211 seconds. CPU0/L2 cold recovery counted two, RAM's 8 MiB hash
matched, early clock/PMIC recovery passed, suspend success/fail was 2/0,
and no ISP handshake timeout recurred. Four subsequent runtime on/off
rounds read LARB1 gate enabled (CG=0x3fe), VC=2 and all eleven OSTD=1;
release suspended LARB1/provider, dropped clock references and powered ISP
off. VCODEC bit9 stayed off: codec functional gating/DMA is not qualified.

Post-resume Mali rendered 1802 frames with green readback and GL error 0.
Aligned 4 MiB direct eMMC write and two reads matched SHA256
8332d6ad8e87a40172371f4744d8fdf9c6acff5d52468f8cc3d7b630d5a61f0f;
eMMC retained 8-bit/52 MHz/error0. M4U faults/TLB timeouts, RDMA underflow/
abnormal and kernel taint were zero. LARB1 save/restore was 5/4, busy0,
MMU mask0 (no DMA consumer). Final MD1/ISP/DISP/MFG domains were off;
SPM status readbacks at 0x1000660c/610 were 0x0000be44/0x0000be40, ISP bit5
clear. devices_deferred and temporary wake locks were empty. See
sleep-170.txt, deep-170.txt, dmesg-170.txt and final-root-170.txt.

Stock decoder investigation: remaining work
-------------------------------------------

The user's 8.1dump/system/{vendor/lib,lib} provides the stock ARM32
libvcodecdrv.so and dependencies. REA/Ghidra evidence below is retained
locally; proprietary binaries are not part of this repository. A private
/data/local/tmp loader with coherent old C++ dependencies loaded the real
library. Its temporary performance-hint shim explicitly returned ENOSYS;
it is not installed as PowerHAL. The chip metadata query returned success
and 0x12 (MT6580), and format capability queries completed. These queries
do not create a codec or decode a frame. No Android media registration,
production library replacement or codec job was performed.

Relevant decompilations and evidence identities:

* rea-init.json, ev_57af6be63e2b9f70c52f2df2d6305b74e7568379a217f387c14bdb0cc39d49ed:
  initialization opens /dev/Vcodec and uses the legacy EMI-user ioctl.
* rea-eVideoLockHW-resolved.json,
  ev_7248ef8cd2d89198224504df56f8b2632e5eedb2742c2b663a7e3a94d507e244:
  lock uses ioctl 0x40044d0d and the old VAL lock structure.
* rea-eVideoAllocMVA.json,
  ev_706c2c118845834ecf50e0c183897a8acd9a34509fddf2abb028d999501a5cf5:
  buffer mapping delegates to the old VcodecM4u interface.
* rea-querycap-resolved.json,
  ev_db9cae35d0da8ff7d50fc089533358930d7eba605df8565cd51b14f3cd51533d:
  query 3 reports chip 0x12; query 1 is metadata capability reporting.
* rea-create.json,
  ev_5d5d7a2eb2a68b8ba01057e24d1d17b4fced04c8aae91de4eeafda7960310fdc:
  format 0x100 selects the hybrid H.264 interface, requiring allocation.
* rea-memalloc-resolved.json,
  ev_7a29731d470b8eed256a4688b30505ce1fd905815b2b30c655db3ba04201b1c0:
  the 96-byte VAL memory descriptor uses ION allocation/share/mmap and
  private MM configuration/address queries. malloc cannot substitute DMA.

The current kernel has a real system DMA-BUF heap, but no /dev/ion or
/dev/Vcodec. A complete integration still needs owned/imported buffers
mapped to real M4U IOVAs, cache synchronization, legacy lock/shared-OAL/IRQ
translation, validated register access and job ownership. The BSP hybrid
IRQ copies register results to registered shared context and unlocks the
hardware; returning success from a dummy wait would be incorrect. Then a
known bitstream must be decoded and frames checked, before integrating a
modern Android codec service. MT8173 V4L2 compatibility is not assumed.
Physical current, fault injection and actual video decoding remain untested.
