.. SPDX-License-Identifier: GPL-2.0-only

WT6580 WE N / Android 17 kernel bring-up
======================================

Base and scope
--------------

This port uses Android Common Kernel ``android17-6.18``, initially pinned to
``64e87d26a115fd03e5fb17e3a56342605c3946ec`` (Linux 6.18.32). It is a custom
32-bit ARMv7 board kernel, not an arm64 GKI image. Android 17 userspace and
the vendor HALs have not been brought up.

The reference BSP is ``~/android_kernel_mediatek_k80``, project
``wt6580_we_n``. That tree contains local user changes and is read-only for
this port. Hardware information below was checked against the running
Android 10 / Linux 3.18.79 device; static vendor DTS defaults alone were
not used to determine RAM size or the screen model.

Confirmed board information
---------------------------

* Four Cortex-A7 cores; the first-stage configuration intentionally uses
  CPU0 only. The upstream MT6580 DTS has no supported secondary-core
  enable method. The vendor ``mediatek,mt6580-smp`` method is not implemented
  by this kernel.
* Runtime DT memory: base ``0x80000000``, size ``0x3fa00000``. This is a 1 GiB
  device with the top 6 MiB omitted by firmware, not the 512 MiB shown by
  the unmodified vendor DTS.
* ``/proc/driver/lcm_info`` reports ``jd9161_wvga_dsi_video``. Reverse
  engineering the verified normal boot backup confirms that driver name,
  480x800 RGB888, two DSI lanes, sync-pulse video and a 150 MHz PLL setting.
  Its initialization selects between two tables for IDs ``0x9161`` and
  ``0x0095``. Hardware reads with build #20 and the DRM panel driver with
  build #25 confirm the installed controller ID is ``0x9161``.
  The reference source's ``jd9161_wvga_dsi_vdo_hlt_boe40`` has different
  timings and commands and must not be treated as the identified panel.
  See ``wt6580-stock-display.rst`` for addresses, values and limitations.
* Touch controller reports ``FT6x36``. No touchscreen or I2C bus support
  has been connected in this first-stage DTS.
* UART0: ``0x11005000``; GPT: ``0x10008000``; watchdog: ``0x10007000``.
* eMMC boot partition: 16 MiB, ``/dev/block/mmcblk0p7``. The captured header
  is Android boot format v0, 2048-byte pages, kernel address
  ``0x80008000``, ramdisk address ``0x84000000``, tags address ``0x8e000000``.

Implemented
-----------

* MT6580 added to the ARM MediaTek machine compatibility table. Having
  a DTS without this machine match is insufficient to select the board's
  platform initialization and timer setup.
* Board DTS, build target and defconfig. Root address/size cells remain
  2/2 and ``/memory`` keeps its vendor path for LK fixups. All inherited
  MMIO ``reg`` properties are widened consistently. DTC may warn about
  the intentionally unit-less legacy ``memory`` node.
* GPT clocksource, UART0/earlycon, watchdog and persistent console
  configuration. No architectural timer is described: its boot-firmware
  setup and GPT6 gating need separate investigation.
* Vendor fixed RAM-console/minirdump/framebuffer reservations and dynamic
  modem/connectivity reservation sizes retained. The modem and connectivity
  drivers are not enabled; their exact runtime physical allocation still
  needs verification before those subsystems are used.
* A WT6580 DRM panel driver recovered from the normal boot backup,
  replacing the earlier BOE40 source-based implementation. It reads DCS
  ID register 04 and selects the 0x9161 or 0x0095 initialization table,
  preserving command order, payload lengths, generic/DCS packet selection,
  reset and suspend delays. Regulator, reset and backlight handling use
  DRM/GPIO/regulator APIs. The optional DRM DTS now instantiates it with
  MT6580 DSI/PHY, MMSYS routing and a dedicated MMSYS reset GPIO output.
  The PMIC supply and display power domain still retain firmware setup.
  The local experimental compatible string is
  ``wingtech,wt6580-we-n-panel``. A panel node under the DSI host needs ``reg``,
  ``power-supply`` and an active-low ``reset-gpios``; ``backlight`` is
  optional. A formal upstream binding and panel model identification
  remain to be completed after the hardware test.
  The mode uses the recovered porches and a provisional 23.900 MHz pixel
  clock inferred from the bootloader's 57.43 Hz tag. The 300 Mbit/s lane
  limit corresponds to the BSP's 150 MHz PLL setting; it does not program
  a host PLL. The MT6580 host now programs the recovered byte timings
  and the PHY rate through the existing MediaTek frameworks.
* Binder/binderfs, BPF and seccomp build options included as groundwork.
  This configuration is a diagnostic baseline, not a completed Android 17
  kernel configuration or compatibility certification.
* LK framebuffer handover: hardware-verified stable eight-color output
  on build #19 via fbdev, using the top OVL layer at 0xbfb77000,
  480 x 800, 32 bpp and pitch 1920. Lower firmware layers, the DSI/PHY
  configuration and backlight are retained. See ``wt6580-stock-display.rst``.
* Diagnostic DSI command access: build #20 reads panel ID ``91 61 00``
  and power state ``9c`` on hardware, confirming the JD9161 branch.
  Build #21 adds display on/off and an explicitly requested panel reset
  and initialization using the same recovered table as the DRM driver.
  Firmware display power, PHY configuration and scanout are retained.
* Optional MediaTek DRM/KMS path: RDMA0 memory scanout to DSI0, using
  the existing 6.18 MediaTek drivers with MT6580-specific routing, mutex,
  command transport, timing and PHY data. Build #27 modesets a new GEM
  framebuffer at 0xaf100000. The user confirms the corrected color bars
  appear normal after replacing the inherited MT8173 FIFO configuration
  with MT6580 WVGA settings. See the display analysis for limits.
* USB CDC ACM debug transport: real TOPCKGEN/APMIXEDSYS USB gate control,
  a device-only MT6580 PHY driver derived from the vendor register
  sequence, MediaTek MUSB in PIO mode, built-in ``g_serial`` and the
  ``ttyGS0`` diagnostic command interface. PHY slew calibration has a bounded timeout
  and the vendor default fallback. DMA and host/OTG switching are disabled.

DRM display build
------------------

The default build retains the LK framebuffer diagnostics. Select the
MediaTek DRM path explicitly::

  DISPLAY_DRIVER=drm EARLY_BOOT_TRACE=1 JOBS=8 \
    STOCK_BOOT=/home/fuquan/AIC/out/wt6580-reference/stock-boot.img \
    tools/wt6580/build.sh

This selects ``mt6580-wt6580-we-n-drm.dtb``, enables the existing MediaTek
DRM and MIPI PHY drivers, adds 16 MiB of lowmem CMA, and disables the LK
framebuffer driver and automatic fbdev emulation. Run ``drmtest`` over
CDC after boot to initialize the panel and modeset. ``drmoff`` followed
by ``drmtest`` tests shutdown and reinitialization. The host protocol tests
can be built with::

  cc -D__user= -D__EXPORTED_HEADERS__ -Iinclude/uapi \
    -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined \
    tools/wt6580/test_drmtest.c -o /tmp/test-wt6580-drm
  /tmp/test-wt6580-drm

The DMA buffers use the physical RDMA path; no GPU, IOMMU, overlay planes,
Android display HAL or PMIC/backlight power management is claimed.

USB CDC ACM debugging
---------------------

The diagnostic kernel binds a single CDC ACM port automatically; it does
not need Android init, configfs scripts, eMMC, Wi-Fi or ADB. The device-side
TTY is ``/dev/ttyGS0``. On a Linux PC it normally appears as
``/dev/ttyACM0``; on Windows use the corresponding COM port. The user confirmed enumeration on this hardware with build #10.

On a Linux PC, open the port with the included standard-library-only tool::

  python3 tools/wt6580/cdc_console.py /dev/ttyACM0 --log usb-debug.log

The tool configures a raw terminal at 115200 baud. CDC ACM baud rate is
virtual here; it does not change the physical USB link speed. Enter one
of these commands followed by Enter:

* ``help``: list commands.
* ``dmesg``: replay the available kernel ring buffer, including messages
  emitted before USB enumeration. Records retain the ``/dev/kmsg`` format.
* ``status``: kernel version and build information.
* ``cmdline``: effective kernel command line.
* ``interrupts``: IRQ counts, including the USB controller interrupt.
* ``meminfo``: memory usage.
* ``fbstatus``: captured LK display registers and framebuffer registration.
* ``fbtest``: save the current framebuffer and draw eight vertical color bars.
* ``fbrestore``: restore the saved framebuffer pixels.
* ``dsistate``: read live DSI, MIPI PHY and panel-reset registers.
* ``panelid``: read DCS ID 04 and power state 0a, then restore video mode.
* ``paneloff`` / ``panelon``: send display-off / display-on commands;
  power supplies and backlight are retained.
* ``panelreinit``: require ID 9161, pulse the MMSYS panel-reset output,
  send the recovered JD9161 initialization table and restore video mode.
  This does not initialize display supplies or the PHY from power-off.
* ``drmstatus``: probe the DRM connector and check for a 480x800 mode.
* ``drmtest``: allocate/map a DRM GEM buffer, draw bars and set the CRTC;
  repeated calls redraw the active buffer. Available in the DRM image.
* ``drmoff``: disable the diagnostic DRM scanout and release its buffers.
* ``reboot``: sync filesystems and request a normal kernel restart. The USB
  connection drops as the device restarts. A failure to invoke the restart
  syscall is reported on the command interface.
* ``reboot bootloader``: sync, set and read back the MT6350 RTC fastboot
  marker, then invoke RESTART2 with ``bootloader``. If the marker cannot
  be verified, the diagnostic interface cancels the reboot.

The default command line no longer selects the ttyGS0 kernel console.
It bypasses TTY newline conversion and its finite queue drops boot logs
before the host connects. Use ``dmesg`` to read the available kernel
ring-buffer history; UART and ramoops retain live kernel logging.
Device-side canonical input provides echo, DEL backspace and Ctrl-U line
erase. Disable local echo in third-party serial terminals. The bundled
host tool sends keys immediately, relies on device echo and restores the
local terminal on exit. Diagnostic replies
retain pending bytes across short writes, backpressure and disconnects.
Input and output are nonblocking so a disconnected PC cannot stop PID 1
from feeding the watchdog. Unknown or oversized commands are rejected.
This interface provides diagnostics, not a shell.

Bootloader mode uses the vendor MT6580/MT6350 protocol: PWRAP WACS2 at
0x1000f000 retains firmware initialization, RTC_PROT receives 0x586a and
0x9136 with write triggers, then RTC_PDN1 (0x802c) bit 13 is set with
other bits preserved and checked by readback. Every wrapper transaction
has a timeout; the RTC busy wait is bounded. The local bootmode driver's
write-only ``bootloader`` sysfs attribute lets PID 1 verify preparation
before rebooting. A reboot notifier also handles standard kernel
``bootloader`` restart requests. No register is written at driver probe.
The MT6580 watchdog restart path clears power-key bypass for this mode,
matching the vendor reset sequence. This temporary wrapper access driver
must be replaced when a full PMIC wrapper driver takes ownership.
The user verified that ``reboot bootloader`` enters LK fastboot, and the
host read product WT6580_WE_N, serial A12601A183202213 and boot partition
size 0x1000000 through fastboot. A subsequent fastboot reboot returned to
the diagnostic CDC device. Builds #19 through #21 were subsequently
flashed to the boot partition and booted from this host.

An additional CDC reconnect fault was found during host testing: PID 1
kept a hung-up ttyGS0 file descriptor when ``read`` returned zero. For
canonical nonblocking input an idle read is EAGAIN; EOF requires closing
and reopening the TTY. The command loop now does this and clears any
partial command on reopen. A regression test covers EOF midway through
a command followed by successful input after reopen, and a real Linux
PTY confirms the idle-versus-hangup return values. Device recovery after
USB host switching needs further testing; repeated CDC close/reopen from
this host works on builds #20 and #21.

The command loop drains up to 512 immediately ready operations per pass,
instead of sleeping 100 ms after each kmsg read and each USB write.
Pending transfers retry after 1 ms; an idle interface polls after 20 ms.
Short writes retain their pending bytes and backpressure ends the burst.
Watchdog feeds use monotonic time at five-second intervals, independently
of transfer speed, with heartbeats scheduled every 60 seconds. Protocol
tests cover draining 500 small records without per-record sleeps as well
as yielding on backpressure. Actual USB throughput still requires hardware measurement.

The USB drivers retain firmware PLL frequency and PMIC VUSB supply setup.
The clock driver reports an error if UNIVPLL is off rather than inventing
PLL parameters. The PHY does not yet manage the PMIC regulator. If LK
leaves either resource off, those dependencies must be ported before USB
can enumerate. The new USB PHY/clock compatible strings and clock IDs
are local to this initial port and need formal bindings before upstream
submission.

USB console output requires the controller, PHY, clocks and interrupts to
work. It cannot expose failures before those initialize, and a panic or
lockup can stop the workqueue/IRQ path used to transmit console output.
The persistent console and a physical UART remain useful fallbacks.

Persistent console layout
-------------------------

The vendor ramoops region is ``0x83f10000 + 0xe0000``. Its driver has an
extra 64 KiB boot-console zone, unlike upstream ramoops. The running
kernel reports record=4 KiB, console=64 KiB, ftrace=4 KiB, pmsg=64 KiB,
ECC=0. Therefore its regular console starts at::

  0x83f10000 + (0xe0000 - 2 * 0x10000 - 0x1000 - 0x10000)
  = 0x83fbf000

The new DTS places a console-only upstream ramoops instance at that
64 KiB address and reserves the rest of the vendor region separately.
This is intended to allow the vendor kernel to expose the previous
diagnostic console after a warm reboot. Cross-kernel recovery and
bootloader preservation of RAM have not yet been tested. A physical
UART console remains the fallback, especially for failures before
ramoops probes. Power removal can lose these logs.

Building
--------

Use Clang/LLD 15 or newer. The tested local toolchain is Android Clang 20
under ``~/buildtools/linux-x86/clang-r547379/bin``::

  LLVM_BIN=/path/to/clang/bin \
  OUT_DIR=/path/to/output \
  tools/wt6580/build.sh

The script compiles a static ARM EABI diagnostic ``/init`` without libc
and embeds it in the kernel. PID 1 mounts devtmpfs, proc, sysfs and pstore,
opens the watchdog, logs boot information, services USB debug commands
and writes a heartbeat every minute. This is a diagnostic process, not
a shell or Android init.

The build produces ``zImage``, ``mt6580-wt6580-we-n.dtb``, ``zImage-dtb``,
a separate diagnostic initramfs and ``manifest.json`` containing artifact
sizes and SHA-256 hashes. Output stays outside the source tree by default.

To also package a legacy diagnostic boot image using a complete stock
boot partition dump::

  STOCK_BOOT=/path/to/stock-boot.img tools/wt6580/build.sh

The packer retains the stock board's load addresses and header command
line, replaces both kernel and ramdisk, recalculates the legacy SHA-1
image ID and checks the image fits the captured partition size. It
supports only v0 images without separate DT payloads and does not sign
images or alter verified-boot settings. No build script flashes or
reboots a device.

Current limits and first hardware test
-------------------------------------

Compilation does not prove hardware boot. SMP, eMMC, USB enumeration, Wi-Fi, modem,
display controller, GPU, audio, camera, suspend and Android 17 userspace
are not operationally validated. Existing Android 10 vendor binaries
cannot be assumed compatible with a 6.18 kernel.

First test requires a working local recovery/bootloader route and a way
to obtain logs. USB fastboot ``boot`` may be usable if supported by this
device's LK. Network ADB alone cannot provide a console for this image:
Wi-Fi is not initialized, and CDC ACM replaces ADB over USB. Before a
test, verify the stock boot backup hash and arrange a physical way back
to the stock kernel. The expected first success marker is
``WT6580: DIAGNOSTIC_INIT_STARTED`` followed by ``DIAGNOSTIC_HEARTBEAT``.

Tracing the early boot stop
---------------------------

The 2026-10-22 hardware log from build #7 reaches the Linux banner, CPU
identification, FDT model, UART earlycon and dynamic reserved-memory
allocation, then stops after the modem reservation. This confirms entry
into ``setup_arch`` but does not establish a completed memory setup or
an operational timer, USB or display driver. The printed reservations
are not errors: connectivity occupies 0xbf800000..0xbf9fffff and modem
occupies 0xbe000000..0xbf7fffff, both within the reported RAM extent.
The last emitted line alone cannot distinguish a CPU stall from a lost
console mapping. Timestamps remain zero before timekeeping starts.

Build the next diagnostic image with::

  EARLY_BOOT_TRACE=1 STOCK_BOOT=/path/to/stock-boot.img tools/wt6580/build.sh

This appends ``arm_early_trace initcall_debug keep_bootcon``
to the built-in command line and records ``early_boot_trace: true`` in
the manifest. The local ``arm_early_trace`` parameter enables
``ARM_EARLY:`` checkpoints in ARM setup, memblock, paging and bootmem
initialization. It is off without the parameter. Consecutive checkpoints
bracket the reserved-memory scan, lowmem bounds update, ioremap reset,
page-table preparation, lowmem/kernel mapping, fixmap transition,
device mapping, memory-zone setup and device-tree expansion. The trace
uses the existing printk/earlycon path and can itself disappear if that
mapping stops working; it is not an independent hardware progress probe.

Capture the serial log from reset through its last output. The next
useful evidence is the last ``ARM_EARLY:`` marker plus nearby memblock
messages. This instrumentation does not claim to fix the boot stop.

Build #8 reaches ``paging: prepare page table done; map lowmem begin``
but never prints the matching completion marker. Its memblock dump exposes
a mapped 64 KiB ramoops island at 0x83fbf000 between two no-map regions.
On non-LPAE ARM, the preceding mapping ends at 0x83f00000 and installs a
1 MiB section at 0x83e00000. Both addresses share a logical 2 MiB PMD.
Mapping the unaligned ramoops island then enters ``arm_pte_alloc``, whose
``BUG_ON(pmd_bad(*pmd))`` rejects the existing section descriptor. This is
a source-level explanation consistent with the stop; the device log does
not itself contain a BUG report.

The ramoops node now also has ``no-map``, making the entire interval
0x83f00000..0x83ffffff absent from the linear map. Its physical address,
size and console format remain unchanged; the pstore driver creates its
own buffer mapping. The fix still needs a hardware boot test. The next
expected marker is ``paging: map lowmem done``. A host harness using the
four mapping functions extracted from this tree reproduces the BUG with
the build #8 ranges and passes with the ramoops island excluded. This
checks the descriptor collision, not execution on the device's MMU.

Build #9 confirms that lowmem and kernel mapping now complete on hardware.
It stops after ``paging: DMA remap done; early fixmap shutdown begin``.
The trace configuration used for builds #8 and #9 included
``memblock=debug``. This is unsafe during ARM's early fixmap transition:
``early_fixmap_shutdown`` clears the earlycon PMD and invalidates its TLB
entry before ``create_mapping`` allocates the replacement page table.
The allocator's debug printk can therefore access the unmapped UART before
its mapping has been restored. Remove ``memblock=debug`` from subsequent
trace builds, retaining the checkpoints on either side of the transition.
This addresses a fault path introduced by the diagnostic configuration;
hardware confirmation is still required. The next expected marker is
``paging: early fixmap shutdown done; device maps begin``.

Build #10 reaches ``DIAGNOSTIC_INIT_STARTED`` and produces heartbeats
through at least 181 seconds. Ramoops, the UART console, USB PHY, MUSB
and g_serial probe successfully; the user also confirms host-side USB
enumeration. This validates the diagnostic boot path, not Android
userspace or the display pipeline. The earlier deferred USB probes
resolve during the late initcalls. Duplicate UART lines result from
``keep_bootcon`` retaining earlycon alongside ttyS0.

After this test, the diagnostic PID 1 enables ``OPOST | ONLCR`` on ttyGS0
and uses LF consistently in its replies. The TTY converts both replies
and proc/kmsg dumps to CRLF for serial terminals. A subsequent screenshot
showed that direct kernel-console output bypassed this conversion and
that raw input had no echo. The interactive configuration described above
addresses both paths; CR, LF and CRLF commands remain accepted. Host protocol tests and a PTY
output-conversion check pass; device-side formatting needs retesting.

eMMC bring-up
-------------

The captured running-device DT locates MSDC0 at ``0x11120000`` with GIC
SPI 38, level low, an eight-bit non-removable bus and clock selector 7.
The legacy MT6580 driver confirms a 12-bit divider (CFG[19:8]), clock mode
at CFG[21:20], PAD_TUNE0 at 0xf0, synchronous command/CRC FIFOs and
PATCH_BIT1=0xfffe00c9. The existing ``mtk-sd`` driver now has an MT6580
variant for that layout; the asynchronous FIFO settings of MT2701 are
not used. The board's captured DAT read and CMD/write delay values are
all zero. The vendor initialization forces rising sampling edges for
non-SDIO cards regardless of the captured default edge properties.

TOPCKGEN exposes MSDC0's gate (CLK_GATING_CTRL0[17]) and retains the
firmware source selector (CLK_MUX_SEL0[13:11]). Source rates come from
the MT6580 vendor clock table. PLL retuning, a full pinctrl driver and
PMIC regulator control are pending. ``mediatek,firmware-pins`` explicitly
opts into retaining LK's pad setup, restricted to non-removable legacy
eMMC at no more than 26 MHz. A fixed always-on VEMC supply models the
vendor driver's 3.0 V setting; it does not program the PMIC. No DDR,
HS200, HS400, signal voltage switching or hardware command queue is
advertised. This is a bootloader-dependent bring-up configuration.

Diagnostic commands are ``mmcstatus`` (clock/register snapshot),
``mmcid`` (raw CID), ``partitions`` (kernel partition list) and ``mmcread``.
The last command opens only ``/dev/mmcblk0`` with ARM EABI
``O_RDONLY | O_DIRECT | O_LARGEFILE`` and reads its first MiB twice,
reporting both CRC32 values. Every 64 KiB read bypasses the page cache;
no block-write, erase or discard operation is requested. The build #30 diagnostic
PID 1 does not mount an eMMC filesystem. Host tests check ARM open flags,
CRC calculation, repeated reads, short I/O, EINTR and descriptor cleanup.
The MMC core still performs normal card initialization/mode selection.

Hardware builds #29 and #30 identify ``FN62MB``, 7.28 GiB, 22 GPT partitions,
two 4 MiB boot areas and a 512 KiB RPMB character device. Legacy timing
uses an eight-bit bus, a 208 MHz source and an actual 17.333333 MHz card
clock (25 MHz requested, rounded down by the divider). Two uncached
first-MiB reads both yield CRC32 ``77bb957a`` with host error zero. DRM
color-bar output remains active with zero RDMA underflow/abnormal counts.
These checks do not establish write reliability, full-media integrity,
Android filesystem mounting, suspend-to-RAM or high-speed tuning.

Filesystem and shell environment
--------------------------------

The default build now includes static BusyBox 1.37.0 and musl 1.2.5,
built from SHA256-pinned upstream source archives by
``tools/wt6580/build-userspace.sh`` using Android Clang and compiler-rt.
Sources, sysroot and binaries stay outside the kernel tree in
``../out/wt6580-userspace``. No cross GCC or device-side shared libraries
are required. Set ``USERSPACE=0`` for the smaller diagnostic-only image.
Both upstream license texts are packaged under ``/usr/share/licenses``.

PID 1 retains the CDC protocol, watchdog feeding, child reaping and reboot
commands. ``sh COMMAND`` runs a non-interactive BusyBox shell with stdin
from ``/dev/null`` and stdout/stderr streamed over CDC. It supports pipes,
redirections and command lists, up to the protocol's 511-character limit.
It is a new shell for each command; working-directory changes do not
persist between commands. Output uses a nonblocking pipe reader so idle
children and USB backpressure do not block PID 1. Completion is reported
as ``[WT6580 shell exit=N]``. Host integration tests use real fork/exec and
pipes to check long output, idle children, exit status and orphan reaping.

``storage`` maps the kernel's GPT PARTNAME values to
``/dev/block/by-name`` and the legacy MediaTek by-name path. Only labels
that are safe path components are accepted. Partition numbers are not
hardcoded. ``mountall`` probes the existing filesystems and mounts
``system``, ``vendor``, ``userdata`` and ``cache`` read/write under
``/mnt/system``, ``/mnt/vendor``, ``/mnt/data`` and ``/mnt/cache``.
Mounts are requested explicitly over CDC; boot does not mount them.
Ext4 ACLs, security xattrs and v2 quota support are enabled. The device's
userdata requires CONFIG_QUOTA and CONFIG_QFMT_V2; build #31's kernel
log explicitly identifies those missing options when mount returns EINVAL.

For example::

  storage
  mountall
  mounts
  sh ls -l /mnt/system
  sh grep '^ro.build.version' /mnt/system/build.prop
  fsrwtest
  umountall

``fsrwtest`` creates a new temporary directory on each mounted filesystem,
writes 262195 bytes of known content, syncs, drops the file cache and
compares the readback SHA256. The expected digest is
``2e6deb90b060da3bdd007d7a999786ca60eb7a19dac5bd04869fc1a43c205d84``.
Only the test file and its newly created directory are removed on exit.
This validates filesystem writes and readback, not power-loss durability.
``umountall`` syncs and unmounts these filesystems; reboot commands also
attempt to unmount them before requesting the watchdog restart. No mkfs,
partition-table rewrite or automatic filesystem repair is performed.

Hardware build #33 mounts all four ext4 partitions read/write and passes
``fsrwtest`` on each with the expected digest above. The existing system
partition's build properties identify Android 10; Android init is not
started by this diagnostic image. A test file written on userdata is
read back correctly after ``reboot bootloader``, fastboot reboot and
remount, then removed. New mount logs show no journal recovery during
that test. A 35-second shell sleep completes while the watchdog timeout
is 30 seconds, confirming PID 1 remains responsive to its keepalive work.
The host error field and DRM underflow/abnormal counters remain zero.
Existing ext4 mount-count/check warnings remain; this work does not run
filesystem repair or reset those filesystem health fields.

LineageOS 24 / Android 17 userspace
---------------------------------

The selected userspace is LineageOS ``lineage-24.0`` (Android 17), not
LineageOS 17.1 (Android 10). The checkout is ``~/AIC/lineage24``. Its
initial manifest commit is ``b51a0c449a3279ca55fb6d4d91d2143e42e2fff5``;
AOSP projects in that manifest use ``android-17.0.0_r1``. Repo is configured
with depth 1, current-branch sync, no tags and Git LFS. Shallow history does
not reduce the size of the checked-out source and prebuilt toolchains.

The dedicated device product is ``lineage_a1260`` in
``~/AIC/lineage24/device/aic/a1260``. It defines Cortex-A7 ARM32, legacy
header-v0 boot packaging, the appended board DTB and the actual fixed GPT
sizes. Product/system_ext content lives inside system; vendor is built
separately from source. A first device boot image has compiled; system,
vendor and Android hardware services are still being brought up. No
Android framework startup has yet been verified on this board.

``arch/arm/configs/wt6580_android17.config`` adds VFP/NEON, SELinux,
cgroup controllers, PSI, loop/device-mapper/verity and EROFS for APEX,
DMA-BUF system heaps, zram and basic Android networking interfaces.
The v1 cpuset and blkio controllers match Android 17's libprocessgroup
configuration; memory control remains available through cgroup v2.
``CONFIG_LSM`` explicitly activates SELinux instead of only compiling it.
Enable this diagnostic kernel profile with::

  ANDROID17_BRINGUP=1 DISPLAY_DRIVER=drm EARLY_BOOT_TRACE=1 \
    OUT_DIR=../out/wt6580-lineage24 STOCK_BOOT=/path/to/stock-boot.img \
    tools/wt6580/build.sh

The profile enables SMP infrastructure for cpusets and limits boot to
CPU0 with ``maxcpus=1``. It does not add a secondary-core release method.
It explicitly uses SELinux permissive mode while device policy is being
developed. This is a bring-up profile, not a production security policy
or a claim of Android kernel/VTS compliance. The default diagnostic
configuration remains available without ANDROID17_BRINGUP.

The profile's hardware build #3 boots to the diagnostic PID 1 with VFP,
NEON and CPU0 online. The v1 cpuset/blkio controllers, cgroup v2 and
selinuxfs mount successfully; SELinux reports permissive mode. All four
ext4 partitions pass the temporary-file read/write SHA256 test. Repeated
1 MiB eMMC reads agree, and the DRM color-bar modeset completes. These
checks validate the kernel prerequisites; they do not start Android init.

External Android ramdisk handover
---------------------------------

LK passes its own FDT in r2, including ``linux,initrd-start`` and
``linux,initrd-end``. Using an appended DTB previously discarded those
values. Merely enabling ATAG compatibility does not copy them from an
incoming FDT. The stock runtime tree records the ramdisk at ``0x84000000``.

``CONFIG_ARM_APPENDED_DTB_INITRD_FROM_BOOTLOADER`` is an opt-in ARM
compressed-boot option, depending on ATAG/appended-DTB compatibility. It
copies only the two initrd properties into the appended tree. Both 32-bit
and 64-bit encodings are supported; missing, incomplete and inverted
ranges are handled explicitly. The board's command line, memory map and
reserved regions stay in the appended tree. This avoids importing
legacy command-line settings such as ``vmalloc=496M``. Android uses the
SoC identifier ``androidboot.hardware=mt6580``; ``wt6580_we_n`` names the
specific board and remains in the board DTS and defconfig names.

The A1260 device product enables the option and builds an external Android
first-stage ramdisk without the diagnostic built-in initramfs. Hardware
probe build #6 retains diagnostic PID 1 while proving the handover: a file
present only in the external archive is readable after boot, and the two
initrd properties appear in the live tree. A statically linked Android 17
Bionic/toybox binary also executes successfully, including floating-point
``seq`` output. Android framework startup is not covered by these probes.

The host regression test exercises 32/64-bit addresses, malformed metadata,
insufficient FDT space and preservation of command line/reserved memory::

  cc -std=c11 -D_DEFAULT_SOURCE -O1 -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I scripts/dtc/libfdt \
    -I arch/arm/boot/compressed tools/wt6580/test_bootloader_initrd.c \
    scripts/dtc/libfdt/fdt*.c -o /tmp/test_bootloader_initrd
  /tmp/test_bootloader_initrd

References
----------

* https://android.googlesource.com/kernel/common/+/refs/heads/android17-6.18/
* https://github.com/afanetd/pixus-hipower-mainline
* https://github.com/eugenefil/mt6580-mainline/tree/mt6580
* https://github.com/mt6572-mainline/linux
