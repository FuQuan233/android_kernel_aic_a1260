.. SPDX-License-Identifier: GPL-2.0-only

WT6580 normal-kernel display analysis
====================================

Artifact and method
-------------------

Analysis date: 2026-10-21. The target is the previously verified complete
normal boot partition backup, not the user's reference source build::

  /home/fuquan/AIC/out/wt6580-reference/stock-boot.img
  size: 16777216
  SHA256: 665623dfff88a86b71cae34618784015920bcdcc59cdd150721c08c05114830b

Its decompressed ARM Image is 15286272 bytes::

  /home/fuquan/AIC/out/wt6580-display-rea/reference-Image
  SHA256: 31ad56ed4db3cdc3cf20cb4dcbf0d976fc6e9bcf30893862a7e462c1d5f4803a

REA 6.7.0 with Ghidra 12.1.4 decompiled a byte-identical code slice from
Image offsets ``0x4c5f00..0x4c6400`` (end exclusive). File offset zero of
that slice was mapped to ``0xc04cdf00`` as little-endian ARMv7. This uses
the inferred full Image load base ``0xc0008000``; the driver name pointer,
callback pointers, literal pools and instruction decoding corroborate it.
The slice SHA256 is::

  33ee39f7f82230ec98e652fe4cd8074d49eb968b5e7980801adaf51af84b738d

The source tree's 32-bit LCM header was used to interpret selected structure
offsets, not to substitute its panel implementation. Those offsets match
the recovered stores. The complete structure ABI is not identical: the
binary clears 0x404 bytes while that header describes 0x400 bytes.
Pseudocode and recovered field names are analysis, not original source.

Identity and selection
----------------------

The binary string at Image offset ``0xb9edfb`` is
``jd9161_wvga_dsi_video``. Its pointer ``0xc0ba6dfb`` occurs in an
LCM_DRIVER-compatible record at Image offset ``0xe4a330``. Callbacks are::

  set_util_funcs  0xc04cdfbc
  get_params      0xc04cdf18
  init            0xc04ce158
  suspend         0xc04ce200
  resume          0xc04ce1ec
  compare_id      0xc04ce050

``compare_id`` resets the panel, sends ``BF 91 61 F2``, requests a DSI
read response and reads two bytes from DCS register ``0x04``. It compares
``(byte0 << 8) | byte1`` with ``0x9161`` and ``0x0095``, setting the
selector at ``0xc0e50e14`` to 0 and 1 respectively. Its initial value
in the Image is -1. ``init`` calls this routine if the selector remains
-1, then chooses:

* Selector 1: 43 entries at ``0xc0e50e18``; starts with
  ``F0 55 AA 52 08 00``. The controller/module identity is unestablished.
* Otherwise: 25 entries at ``0xc0e51a30``; starts with ``BF 91 61 F2``.
  This is the JD9161-style table, with 22 DCS writes, two delays and an
  end marker. A failed ID match also falls through to this table.

The shared driver name does not distinguish the two physical panels.
The saved runtime LCM name and bootloader tag identify this driver, but
do not record the DCS ID or selected branch. A controlled hardware read
with build #20 returns ``91 61 00`` from DCS 04, confirming the 9161
branch on this device. HLT/BOE40 module attribution remains unsupported.

Recovered common parameters
----------------------------

The following are observed stores in ``get_params``; field meanings use
the matching offsets of the vendor header. H/V timing values are in pixels
and lines respectively. The PLL setting is not a DRM pixel clock.

============================  =============  ======================
Parameter                     Normal backup  Previous BOE40 source
============================  =============  ======================
Active size                   480 x 800      480 x 800
DSI lanes / pixel format      2 / RGB888     2 / RGB888
Video mode                    sync pulse     sync pulse
DSI PLL setting               150 MHz        170 MHz
Horizontal sync/back/front    10 / 10 / 10   30 / 30 / 38
Horizontal blanking field     60             0
Vertical sync/back/front      4 / 6 / 6      2 / 7 / 8
Packet size                   256            256
Intermediate buffers          2              2
============================  =============  ======================

The saved live ``chosen/atag,videolfb`` is a packed little-endian vendor
structure. It contains framebuffer base ``0xbfa00000``, connected=1,
fps=5743 (the BSP uses hundredths of Hz), VRAM size ``0x600000`` and
name ``jd9161_wvga_dsi_video``. These are bootloader-reported values,
not measured panel timing. Separate ``atag,videolfb-*`` properties still
contain stale ``nt35590_AUO`` defaults, 60 Hz and framebuffer 0x9e800000;
they must not be confused with that packed live tag. The BSP parser
prefers the packed tag when present. The port's former 0x9e800000
reservation derived from the separate properties; it has now been replaced
with the packed tag's 6 MiB allocation at 0xbfa00000. The live RAM range already
ends at 0xbfa00000, excluding the top 6 MiB framebuffer from normal RAM.

Firmware scanout handover
-------------------------

The first display bring-up image enables ``FB_MT6580_LK`` and the local
``wingtech,wt6580-lkfb`` device. The driver observes OVL0 at 0x14007000,
RDMA0 at 0x14009000 and DSI0 at 0x14012000. These addresses are confirmed
by the saved live DT; the MT6580 reference source supplies the register
offsets and field definitions. It does not substitute the panel table.
Probe does not program display controllers, panel reset, clocks, PHY or
panel initialization. Later diagnostic controls described below explicitly
request command transfers and panel reset.

The driver exposes ``/dev/fb0`` for the top full-screen memory OVL
layer or RDMA memory mode with a supported RGB format, 480 x 800 geometry,
a sufficient pitch and a scanout address entirely within the reserved
allocation. RGB565, RGB888 and both 32-bit layouts are decoded from the
current register values, including red/blue swap. Lower OVL layers may
remain active: the exposed buffer belongs only to the highest enabled
layer, and restoring it restores that layer's pixels. Flipping, color key,
reduced global alpha, nonzero layer offsets, YUV and addresses outside the allocation
leave the device in diagnostics-only mode. No IOMMU mapping is assumed.
The captured snapshot is available even when framebuffer registration
is rejected; it describes boot-time state rather than a live register poll.

The first hardware snapshot from build #18 shows OVL enabled with
``SRC_CON=0x9`` (L0 and L3), ROI 480 x 800, RDMA in direct-link mode,
and DSI in sync-pulse video mode with two lanes. Both active layers use
``CON=0x21ff``, pitch 1920 and full-screen size. L0 starts at 0xbfa00000;
L3 starts at 0xbfb77000, exactly one 1536000-byte frame later. The initial
single-layer restriction rejected this valid LK composition. The revised
handover selects L3 while retaining all other geometry and address checks.

Build #19 was flashed through fastboot on 2026-10-22. It registers fb0
at 0xbfb77000 with 480 x 800, 32 bits per pixel, pitch 1920, format 2,
swap 0. ``fbtest`` completes, and the user confirms all eight bars have
the expected colors and remain stable. This validates framebuffer
handover and pixel layout on this device. It does not validate a Linux
DSI/PHY initialization or panel power cycle. The recorded CDC exchange
is ``display-top-layer-hardware.txt`` in the build output directory.

Use these CDC commands after boot::

  fbstatus
  fbtest
  fbrestore

``fbstatus`` prints the captured registers and registration result.
``fbtest`` saves the original visible framebuffer bytes, then draws eight
equal vertical bars: white, yellow, cyan, green, magenta, red, blue, black.
Drawing starts only on request. ``fbrestore`` restores the saved pixels;
repeated tests retain the first snapshot until restoration. The diagnostic
program uses fbdev ioctls/read/write, not arbitrary physical-memory writes.
The snapshot remains in RAM only and is lost across reboot.

Host tests cover every pixel of the 16/24/32-bit pattern, partial I/O,
stride padding, bounds rejection and repeated drawing/restoration under
address/undefined-behavior sanitizers. A host check of the extracted driver
format decoder covers swap and invalid formats. These checks do not prove
that LK leaves scanout or backlight active, that the first handover image
displays anything, or that a later command-mode update can be triggered.
Full DRM/KMS, display clocks/power/PHY initialization, backlight control
and mode setting remain follow-up work. Panel identification and a
command transport retaining the firmware PHY are now available.

Diagnostic panel control
-------------------------

Build #20 implements bounded polled DSI reads while retaining LK's
PHY and video configuration. It stops video, disables controller
interrupts, selects command mode and the low-power clock state, sends
maximum-return-size and DCS read packets, and acknowledges BTA after
clearing read-ready. Command queue, clock-lane control, mode, interrupts
and start state are restored on success and error paths. The user
confirms stable color bars after reads. ID 04 returns ``91 61 00``;
power state 0a returns ``9c``.

Build #21 adds generic/DCS writes and saves/restores the complete
128-word command queue. ``paneloff`` sends DCS 28; ``panelon`` sends
29 00 and waits 10 ms. Hardware power-state reads change from ``9c``
to ``98`` after off and return to ``9c`` after on.

``panelreinit`` first requires a fresh ID matching 9161 and an asserted
reset-output level. It switches to command mode, pulses MMSYS offset
0x150 bit 0 high/low/10 ms/high/120 ms, sends the recovered 9161 table,
performs display-on and restores the controller state. The table is
shared with the DRM panel driver in ``include/video/wt6580-panel-init.h``.
REA decompilation of the normal Image's reset helper at ``0xc04ed754``
confirms a write to display-config base plus 0x150 followed by DSB.
The live register is ``0x00000001`` before and after the operation.

The first hardware reinitialization returns zero; subsequent ID and
power reads remain ``91 61 00`` and ``9c``. DSI video/PHY configuration
matches the pre-reset snapshot, and framebuffer writes complete. Visual
confirmation after reset must be recorded separately. This resets and
initializes the panel with firmware supplies and DSI PHY retained;
it is not a display power cycle or independent DRM modeset.

The CDC commands are ``dsistate``, ``panelid``, ``paneloff``, ``panelon``
and ``panelreinit``. Sysfs attributes live below
``/sys/bus/platform/devices/14007000.display/``. ``dsistate`` polls live
registers, while ``state`` retains the boot-time snapshot. Write ``1``
to ``panelid`` to perform reads; write ``off``, ``on`` or ``reinit`` to
``panel`` to request control. Reading either attribute reports its last
result. None of these commands runs automatically at probe.

Captured hardware exchanges in the build output are
``dsi-read-hardware.txt``, ``panel-control-before-hardware.txt``,
``panel-control-onoff-hardware.txt`` and
``panel-control-reinit-hardware.txt``. Host tests exercise actual C
transport code with short/long responses, malformed lengths, generic/DCS
packet sizes 1 through 64, payload packing, timeouts and full queue
restoration. They supplement the original table-sequence tests.

MediaTek DRM/KMS path
----------------------

An optional board DTS now instantiates the existing 6.18 MediaTek DRM
components instead of the LK framebuffer diagnostic driver. The initial
path is RDMA0 memory mode to DSI0, bypassing OVL and color processing.
The changes add MT6580 compatibles/data to the MediaTek DRM, RDMA,
MMSYS and mutex drivers. MMSYS routes RDMA0 SOUT at 0x06c to 2 and
DSI0 input at 0x064 to 1. Mutex RDMA0 module bit 8 and DSI SOF 1 follow
the MT6580 BSP. A one-line GPIO controller exposes the dedicated MMSYS
LCM reset output to the recovered DRM panel driver.

The existing MediaTek DSI host programs the 300 Mbit/s lane rate,
MT6580 PHY timing formula and continuous-clock word counts. At the
recovered 480x800 mode its four timing words are 03050302, 040a0308,
040e0100 and 00040b02, matching the firmware snapshot; HSA/HBP/HFP
word counts are all 20. MT6580 COM_CTRL bit 1 is not treated as an
engine-enable bit. The host uses the validated polled BTA path with
bounded waits, correct RX lengths and full command-state restoration.
The existing MT8173 MIPI PHY framework gains an MT6580 variant retaining
bandgap trims, enabling two lanes and using the BSP PCW/SSC settings.
The PHY PLL and signal outputs are shut down and reprogrammed during
DRM disable/enable. PMIC supply and display-domain power remain under
firmware control, and backlight control is not implemented.

Build #25 registers DRM card0, identifies panel 9161 and modesets a new
CMA-backed GEM buffer at 0xaf100000 with XRGB8888, pitch 1920 and size
480x800. This changes RDMA from direct-link to memory mode. The user
reports one or two misaligned bottom rows and a possibly wider white
bar. The first RDMA data reused MT8173's 8 KiB pseudo FIFO and generic
thresholds. Build #27 instead uses 4 KiB, a 512-byte output threshold,
GMC 06010816, issue threshold 221 and SODI 34/39 from the MT6580 BSP's
WVGA memory-mode configuration. The user reports the pattern now appears
normal. After an additional disable/enable cycle, hardware state reports
333 frame-end observations, zero FIFO underflows and zero abnormal EOFs.
Interrupt counts stop increasing when DRM disables unused vblank IRQs;
this does not imply that video scanout stopped.

Build the optional path with ``DISPLAY_DRIVER=drm`` in the normal build
command. The default remains the LK diagnostic image. ``drmstatus``
probes the connector; ``drmtest`` allocates/maps a GEM buffer and modesets
the eight 60-pixel bars; ``drmoff`` disables scanout and releases the
buffer. ``fbstatus`` and ``dsistate`` select the DRM diagnostics when
the LK driver is absent. Tests compare the client layouts/ioctls to
this kernel's DRM UAPI and cover resource enumeration, forced connector
probing, 4 GiB mmap offsets, pixels/padding and cleanup on failures.

Hardware evidence in the build output includes ``drm-hardware.txt``,
``drm-dmesg.txt``, ``drm-cycle-hardware.txt``, ``drm-fifo-hardware.txt``
and ``drm-fifo-cycle-hardware.txt``. Full power-off panel-supply control,
IOMMU/GPU/overlay support and Android display userspace remain unported.

Initialization and suspend differences
--------------------------------------

Compared with the superseded experimental BOE40 DRM driver, the backup's
JD9161 branch changes payloads for B3, B4, B8, C7, C8 and D8, adds
``C1 10`` on page ``BE 01``, and moves ``35 00`` before that page switch.
Examples, with command byte omitted::

  B3: backup 00 6D                 previous 00 AA
  B4: backup 00 79                 previous 00 B8
  B8: backup 00 BF 01 00 BF 01     previous 00 B7 01 00 B7 01
  C7: backup 00 01 32 0A 6A 2A 1B A5 A5
      previous 00 01 33 05 65 2F 1C A5 A5

The backup initialization drives reset high, immediately low, waits 10 ms,
drives high and waits 120 ms before selection/commands. Its table ends
with ``11 00``, 120 ms delay, ``29 00``, 10 ms delay. The former BOE40
port adds a 10 ms initial high delay and waits 20 ms after display-on.
The backup suspend table sends command 22 with zero payload, waits
100 ms, sends 28 with zero payload, waits 10 ms, sends 10 with zero
payload and waits 100 ms; the callback then drives reset low for 50 ms.
The former port's suspend commands, payload lengths and delays differ.

Port of the recovered driver
----------------------------

``drivers/gpu/drm/panel/panel-wingtech-wt6580.c`` now replaces the BOE40
implementation. Both initialization tables and the suspend sequence are
ported. Their common display-on tail runs in DRM ``enable``; the final
sleep-in/reset/power-off stage runs in ``unprepare``. These divisions
preserve the command order and delays across a normal DRM power cycle.
The first preparation includes the original extra compare-ID reset.
Recognized IDs are cached across later preparations, as in the binary.
Unknown IDs retain its JD9161 fallback, log the ID and retry identification
on the next preparation. Unlike the old callback API, transport failures
and short reads abort preparation with reset asserted and power disabled.

A further binary inspection of the DSI command routine at ``0xc04f8638``
establishes that commands above AF use generic packets and the remaining
commands use DCS packets, including the initial BF identification write.
Both video and command-mode paths implement this split (comparisons at
``0xc04f86a4`` and ``0xc04f8cb8``). The port preserves this packet choice.
The original identification queue word ``0x00033700`` sets maximum return
packet size to 3, then the callback reads two bytes from register 04.
The recovered table names ``dcs_write`` in the extraction JSON describe
LCM command rows; they do not establish the underlying wire packet type.

The new local compatible is ``wingtech,wt6580-we-n-panel``; Kconfig uses
``DRM_PANEL_WINGTECH_WT6580``. Pixel clock 23900 kHz is inferred from
510 x 816 x 57.43 Hz, not recovered as a pixel-clock register value. A
300 Mbit/s DSI lane limit records the BSP convention of twice PLL_CLOCK;
an actual 150 MHz PLL request and the host's byte timings still require
the MT6580 DSI/PHY port. The separate horizontal_blanking_pixel field
is retained as an analysis observation, not added to the DRM porches.

Evidence files and practical limits
-----------------------------------

All complete REA Evidence JSON and extracted tables are retained under::

  /home/fuquan/AIC/out/wt6580-display-rea/
    evidence-index.json
    rea-get-params.json / .c
    rea-compare-id.json / .c
    rea-init.json / .c
    rea-suspend.json / .c
    reference-lcm-assembly.txt
    jd9161_init.json
    alternative_init.json
    compare.json
    suspend.json
    initialization-differences.json
    reference-layout.c / .ll
    rea-dsi-command-packets.json
    reference-dsi-command-assembly.txt
    rea-reset.json
    reference-reset.bin
    verify-panel-port.py
    panel-port-harness.c
    port-verification.txt

Each table row records its original Image offset and virtual address.
Entries have a 72-byte stride: 32-bit command, 8-bit count and a 64-byte
payload followed by alignment padding. Command FE means delay in ms;
100 means end marker. Delay counts are not payload lengths.

The host verification compiles the actual driver's power/ID/command
callbacks with recording GPIO, regulator and DSI transports. For both
recognized IDs and an unknown ID it compares two complete power cycles
against the original Image tables, including packet type, payload length,
command order, delays and ID caching. Injected failures cover unlock,
maximum-return setup, ID read, initialization writes and sleep-in; short
ID responses and regulator-enable failures are also checked. These tests
verify software sequencing and cleanup, not physical panel operation.

The original binary analysis establishes a mismatch with the reference
source, not the cause of post-flash flicker. The experimental DRM panel
driver is instantiated only by the new optional DRM DTS. Its compiled
table alone does not show that its commands executed during the earlier
failed boot. The later diagnostic framebuffer driver now executes that table on an
explicit request, as recorded above. The later DRM path now initializes
DSI/PHY and memory scanout; PMIC supply control and the cause of the original post-flash flicker remain
unresolved.
