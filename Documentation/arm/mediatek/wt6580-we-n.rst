.. SPDX-License-Identifier: GPL-2.0-only

WT6580 WE N / Android 17 kernel bring-up
======================================

Base and scope
--------------

This port uses Android Common Kernel ``android17-6.18``, initially pinned to
``64e87d26a115fd03e5fb17e3a56342605c3946ec`` (Linux 6.18.32). It is a custom
32-bit ARMv7 board kernel, not an arm64 GKI image. Android 17 first- and
second-stage init run, and 40 APEX packages activate on hardware. Framework
startup and the Android vendor HALs remain under bring-up.

The reference BSP is ``~/android_kernel_mediatek_k80``, project
``wt6580_we_n``. That tree contains local user changes and is read-only for
this port. Hardware information below was checked against the running
Android 10 / Linux 3.18.79 device; static vendor DTS defaults alone were
not used to determine RAM size or the screen model.

Confirmed board information
---------------------------

* Four Cortex-A7 cores. The diagnostic UP defconfig retains CPU0 only;
  the Android SMP profile uses ``mediatek,mt6580-smp`` to release CPU1--3
  through the non-secure INFRACFG boot vector and SPM power controller.
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
* Touch controller uses the reference BSP's FT6336U configuration: I2C1
  address ``0x38``, reset GPIO2, GPIO5/EINT5 falling edge, 480x800.
  The device owner confirms accurate taps and dragging with the new 6.18
  driver; two-finger input is unverified.
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
* GPT clocksource, UART0/earlycon, watchdog and persistent console.
  The ARM architectural timer supplies per-CPU clockevents; GPT6 is
  enabled in free-running mode at 13 MHz, matching the vendor BSP.
  ``arm,cpu-registers-not-fw-configured`` selects physical timers and
  counters because secondary CPUs bypass firmware initialization. The
  ARM vDSO code also disables its CNTVCT fast path for this property.
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
  The panel supply still retains firmware setup. DISP is explicitly kept
  powered until SMI/LARB context restoration is implemented.
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
* The Android profile includes netd's IPv4/IPv6 raw/filter/mangle/NAT
  tables, connection marks/state, REJECT, TCPMSS, quota2, IPsec policy
  matching, policy routing and XFRM userspace control. These are required
  even on this Wi-Fi-only unit: netd initializes the chains at startup,
  and failures cause its ``onrestart`` hook to restart zygote. FUSE is
  enabled for Android emulated storage. These additions address source
  prerequisites identified after the network BPF loader succeeded;
  on-device netd startup still needs verification.
* ARMv7 little-endian BPF JIT atomic word/doubleword operations use
  LDREX/STREX and LDREXD/STREXD retry loops with memory barriers. Android
  Tethering/netd programs use atomic 64-bit packet/byte counters; upstream
  ARM32 JIT rejected opcode 0xc3 and fell back to the interpreter, which
  Android 17's loader rejects. ADD/AND/OR/XOR, fetch variants, exchange and
  compare-exchange are emitted with register preservation. ARMv6 and
  big-endian builds retain interpreter fallback. The host test
  ``tools/wt6580/test_arm_bpf_atomic.py`` needs Python Unicorn and a host C
  compiler; it compiles the actual emitter functions and executes their
  output in an ARM emulator. It covers 13,200 cases and 4,320 simulated
  failed exclusive stores, both frame layouts, offsets, overflow, aliases
  and compare failures. This does not replace on-device BPF validation.
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

The profile enables four CPUs with the MT6580 non-secure SMP boot method.
INFRACFG ``0x10001000 + 0x804`` bit 31 selects the programmable boot vector
at ``+0x800``. The vector points directly to ``secondary_startup_arm``;
the reference BSP's entry wrapper performs no additional work for cluster 0.
SPM ``0x10006000`` powers CPU1--3, waits for both power status registers,
restores L1 SRAM and removes isolation/clock gating before releasing reset.
Polling has bounded timeouts so a failed power acknowledgement is logged
instead of hanging the boot CPU indefinitely. CPU1--3 hot-unplug now flushes
L1/coherency, waits for STANDBYWFI, isolates the core and powers down its
L1 SRAM and power rails. CPU0 cannot be offlined. Re-online uses the same
reset vector and SRAM power-on sequence as boot.

``tools/wt6580/smp-check.c`` is a native userspace hardware check. It pins
four workers to CPUs 0--3, compares their integer results, verifies a shared
atomic counter and reports CPU time versus wall time for parallel execution.
It also checks monotonic time across repeated migrations between all cores.

The first SMP hardware boot (#17) released all four CPUs in 27 ms, but
virtual-counter timekeeping then stalled: at GPT sched-clock time 0.211 s,
RCU observed 1,625,463,745 elapsed jiffies. This confirms secondary boot,
not a working Android SMP system. The physical-counter revision (#18)
boots Android with all four CPUs online and local timer interrupts on
each core. Four pinned workers complete identical integer calculations
with 2.457 s total CPU time in 0.618 s elapsed (3.98-way parallelism).
The shared atomic counter reaches exactly 400,000 and 400 cross-CPU
migrations show no monotonic-clock reversal. ADB, desktop rendering and
touch registration remain functional; RDMA underflow/abnormal counters
are zero. This verifies SMP execution, not DVFS, hot-unplug or suspend.
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

Android APEX payloads still use dm-verity when boot/system AVB is disabled.
The Android profile enables ``CRYPTO_SHA256`` and ``CRYPTO_SHA512`` to
register hash implementations with the Crypto API. The ``CRYPTO_LIB_SHA*``
helpers alone do not satisfy ``crypto_alloc_shash()``. Without SHA256,
APEX activation fails with ``Cannot initialize hash function (-ENOENT)``;
the missing Tethering APEX then triggers ``netbpfload-missing`` reboot.

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


FT6336U touch integration
------------------------

The reference BSP touch driver and its board parameters are confirmed correct
by the device owner. The 6.18 port uses ``edt-ft5x06`` with the FT6236 two-slot
protocol for ``focaltech,ft6336u``. It does not flash touch firmware. I2C1 at
``0x1100a000`` uses APDMA channel ``0x11000200``, GPIO59/SCL and GPIO62/SDA
(function 1), controller interrupt SPI41 and a 100 kHz bus. MT6580 has the
auxiliary transfer-length register at 0x6c. Register reads use one WRRD
transaction even though MT6580 has no multi-message auto-restart engine;
separate starts would make the first DMA wait indefinitely for the second
transaction. The hardware DCM register is explicitly disabled as in the BSP.

The Paris pinctrl backend describes the vendor's 93 pins and IOCFG register
fields, including 12-byte GPIO bank strides. GPIO0..15 and the dedicated PMIC
route are exposed; the other GPIO banks share interrupt channels and still need
routing support. GPIO5 uses function 6/EINT5; GPIO2 is an active-low reset. The EINT
summary interrupt is SPI94, level high. Special reversed MSDC pull fields are
not advertised as generic pull-select controls.

MT6350 VGP1 supplies 2.8 V. A hardware read before integration found CON0 at
0x050a was 0x0000 (disabled), and CON1 at 0x0530 was 0x00a1 (selector 5).
The regulator uses enable bit 15 and selector bits 7:5, with the BSP's voltage
table. Firmware-initialized WACS2 access is shared through one regmap between
VGP1 and the existing RTC reboot marker, rather than assigning the MMIO block
to two independent drivers. PWRAP initialization, other MT6350 regulators and
suspend/resume remain unfinished. Battery and PMIC keys are described below.

The Lineage device kernel fragment enables these drivers and evdev. No Android
system/vendor change is required for standard direct multitouch input events.

Hardware validation: the input device registers at about one second as
``/dev/input/event0``, with the upstream EDT identification name ``EP0110M09``
(the firmware vendor-ID heuristic, not an independently identified EDT panel).
It advertises ``INPUT_PROP_DIRECT``, X=0..479, Y=0..799 and two MT slots.
VGP1 remains enabled at 2.8 V. Actual interaction produces EINT5 interrupts,
I2C completion interrupts, coordinate updates, tracking IDs and balanced
BTN_TOUCH down/up events. Android InputReader classifies it as internal
``TOUCH | TOUCH_MT``. The user confirms taps and swipes with correct coordinates; two-finger
operation is unverified. Display RDMA reports zero underflows/abnormal events.
A transient display misalignment was reported and recovered without changes;
the captured screenshot was normal, so its cause is not established.

The RTC marker algorithm is unchanged, while register transport now uses the
shared regmap. The diagnostic sysfs path is
``/sys/bus/platform/devices/1000f000.pmic-wrap:reboot-mode/bootloader``.
Normal Android reboots are verified. Re-entering fastboot after the shared
transport refactor has not been hardware-tested.


MT6350 battery, charger and power key
-----------------------------------

The hardware CID register reads ``0x2050``. The MT6397 MFD core now recognizes
MT6350 ID ``0x50``, using the existing two-bank threaded IRQ controller with
enable registers ``0x0160/0x0166`` and W1C status ``0x0172/0x0174``. Its
interrupt clock is enabled through TOP_CKPDN0 bit 10. The legacy DT names
GPIO13, but the vendor IRQ translator maps GPIO13/function 1 to built-in
EINT17. The new DT uses hardware interrupt 17 directly and the pin descriptor
uses function 1, preserving that route instead of shared EINT13/function 6.

The standard ``mtk-pmic-keys`` driver uses status bit 1 at ``0x0142`` and
PMIC IRQ 5 for KEY_POWER. MT6350 requires toggling INT_MISC_CON bit 4 after
each press/release to select the next transition. Hardware reports balanced
KEY_POWER DOWN/UP events, and the owner confirms Android can turn the screen
off and back on. Evdev numbers change as devices are added; they are not an ABI.

I2C0 at ``0x11009000`` uses GPIO58/SCL and GPIO61/SDA, APDMA channel
``0x11000180`` and SPI40. The board's BQ24296 at address ``0x6b`` uses the
existing BQ24190-family charger driver. The original DT has no independent
charger IRQ, so an explicit 2000 ms polling mode reads actual status/fault
registers and emits power_supply changes. No register reset is performed.
The charger watchdog is disabled as in the mainline driver's host mode;
the SoC watchdog remains active. Charge voltage/current limits are 4.2 V
(hardware rounds to 4.192 V) and 512 mA. The board sets a 500 mA input limit,
which must be restored after insertion because BC1.2 detection overwrites it.

``mt6350-battery`` reads AUXADC BATSNS channel 7 and BATON1 channel 5. BATSNS
is a 15-bit conversion with a 1.8 V reference and a factor-four divider.
Temperature uses the reference BSP's 10 kohm NTC table, 16.9 kohm pull-up and
27 kohm pull-down. ADC requests are serialized and timeout after 30 ms;
failed conversions are reported as errors rather than fabricated values.
Presence uses the enabled BATON detector's undetected bit at ``0x000e[12]``.

The device tree carries the reference BSP's 0/25/50 C battery curves and
1397 mAh design capacity. Capacity is an estimate from measured voltage and
the nearest temperature curve, with the charger's real full status taking
precedence. This is not the old software fuel-gauge integration or a hardware
coulomb counter: charge counter/current readings and load compensation are
not provided, and the percentage can jump when USB is inserted or removed.
The battery characterization still needs calibration against the actual cell.

The first hardware boot reports about 4.12 V and 27.3 C, presence true and
charging complete. Android's standard Health service discovers both supplies
and reports USB power and battery level. A real cable removal records
``online=0``/Discharging, followed by ``online=1``/Charging/Full on reinsertion.

The panel backlight is independent of the PMIC key. Its DISP_PWM at
``0x1100f000`` has the MT8173-compatible EN/COMMIT/CON0/CON1 layout, GPIO66
function 1 and a top clock gate at CLK_GATING_CTRL0 bit 0. The port retains
the firmware-selected 26 MHz or UPLL/12 source and connects ``pwm-backlight``
to the DRM panel, allowing panel disable/enable to switch the light off/on.
With no separate enable GPIO or regulator, ``pwm-backlight`` intentionally
keeps PWM enabled at zero duty during blanking so its output is reliably low.
The Composer interface defines brightness 0.0 as minimum visible brightness
and a negative value as off. The device HWC mapping therefore maps the active
range to hardware levels 4..255; the separate ``bl_power`` path still blanks
to zero duty. Android's framework brightness range is unchanged.
SPM deep sleep and PMIC rail shutdown remain separate work. Standby s2idle
and removal of the temporary Android wake lock are described below.


Physical remote keys on AW9523 and AW9106
---------------------------------------

The original device DT configures AW9523 on I2C1/0x5b (GPIO72 reset,
GPIO4 active-low INTN) and AW9106 on I2C2/0x5b (GPIO73 SHDN, GPIO6 INTN).
I2C2 uses GPIO60/SCL2, GPIO63/SDA2, APDMA 0x11000280 and SPI42; its clock
gate is CLK_GATING_CTRL1 bit 16 (unlike I2C0/1 bits 3/4). I2C1 continues to share its bus with touch.

The stock 3.18.79 kernel is the authority for the key topology and codes.
The later, untracked 8x7 QWERTY source in the reference tree is unrelated.
The stock AW9523 handler at 0xc0657384 reads INPUT0 and INPUT1 and reports
16 independent active-high inputs; no matrix columns are scanned. Its key
array at 0xc0a81868 is, in GPIO order::

    106 60 103 28 59 105 108 158 139 63 62 61 115 114 102 64

The AW9106 handler at 0xc0657f90 combines (INPUT_B & 0x0f) | (INPUT_A << 4).
Its six-element array at 0xc0a81bf8 is 0,0,0,66,65,0: OUT3 and OUT4 are
active-high keys, while remaining channels are reserved for LED functions.
These are static findings, not confirmation of the physical button labels.

AW9523 uses the existing pinctrl/GPIO driver; its reset descriptor uses the
existing driver's raw-high convention. AW9106 has a six-line GPIO driver
using OUT0..5 numbering, independent reads of both read-to-clear input banks,
threaded nested interrupts and masks updated under a mutex. Only requested
pins switch to GPIO, after being made inputs; LED channels are not configured by the GPIO driver.
AW9106 SHDN is asserted low and released for at least 5 ms before I2C access.
No pull-up/down is fabricated: the board supplies the input bias externally.

The common gpio-keys device is named a1260-remote and uses 16 ms software
debounce. Android's a1260-remote.kl preserves the original mtk-kpd.kl mapping,
including Linux 60 -> HOME, 61 -> BACK, 28 -> DPAD_CENTER, 62/63 -> channel
up/down, 158 -> F2 and 102 -> F3. Remaining function keys retain their
original Android F-key identities; their factory IR/app actions are not
implemented by merely reporting the keys.

Reverse-engineering evidence is retained locally in out/a1260-keys-rea,
including stock image identity, exact function slices, recovered symbols,
raw key arrays and REA Evidence IDs. No stock kernel binary is committed.
The owner confirms the physical responses and positions. The recording covers
16 distinct key codes with balanced presses/releases, including both AW9106
keys. After the I2C2 gate fix, both chips and gpio-keys register automatically
on reboot. The existing AW9523 regmap now uses single-register reads to avoid
the MT6580 adapter's combined-read limit; IRQ defaults are left unspecified
until the GPIO consumer selects both edges.


CPU frequency and standby sleep
-----------------------------

The four A7 cores share one cpufreq-dt policy and one ARMPLL clock.
The supported rates follow the BSP level-0 (1.3 GHz segment) table:
604.5, 754, 903.5, 1001, 1105, 1209 and 1300 MHz. The first four use
1.15 V; 1105 MHz uses 1.19375 V and 1209 MHz uses 1.225 V. The BSP's
1.30625 V request at 1300 MHz was observed to plateau at selector 0x60
(1.3 V), despite target 0x61. The board therefore requests the measured
1.3 V and caps its regulator constraint there. No undervolting or
higher speed segment is enabled. The GPU's independent MPLL is unchanged.

The CPU clock uses the BSP's MAINPLL handover, PLL2 gate, ARMPLL PCW and
post-divider settings and 20 us PLL settling time. It preserves unrelated
INFRACFG bits. MT6350 VPROC reports the actual hardware voltage at 0x0224
and respects firmware's software/hardware voltage selection at 0x0216.
The DT constrains VPROC to 1.15--1.3 V; cpufreq-dt increases voltage before
increasing frequency, and decreases frequency before lowering voltage.
Schedutil is the default governor, with shared frequency statistics and
Android UID time-in-state accounting. All CPUs have equal capacity.
RT group scheduling defaults to disabled, retaining global RT throttling,
so Android task-profile groups without an RT runtime allocation can still
run permitted SCHED_FIFO tasks. A7 SMT scheduling is disabled.

The MT6580 cpuidle driver uses WFI for runtime idle and system standby.
Its suspend-to-idle entry allows the generic core to freeze the per-CPU
ticks and timekeeping; the physical counter continues across this light
sleep and supplies elapsed time on resume. This is s2idle, not SPM cluster
power-down or DDR self-refresh. Those deeper states require a separate
SPM/PCM and CPU context-save port.

The MT6350 RTC at 0x8000 uses the existing MediaTek RTC driver, PMIC IRQ20,
the factory 1968 year origin and the two-key RTC write unlock. Its hardware
calendar covers 1968--2095. Alarm and time writes preserve RTC reboot and
factory scratch fields. PMIC wake masks are now applied in the parent late
suspend callback, after the power-key and RTC children arm their IRQs. The
old PM notifier applied an empty mask before those child callbacks ran.
The PMIC parent EINT remains a wake source and normal masks are restored
before the children resume.

Hardware validation includes all four frequencies with matching register
readback and integer/coherence checks, 6000 cross-CPU migrations without
a monotonic-clock reversal, and three rounds of CPU1--3 power-off/on.
Both SPM status registers confirmed all three core power bits clear while
offline, and the four-core check passed after every restart. Device and
noirq suspend/resume checks passed, followed by three RTC-triggered
s2idle cycles and an Android SystemSuspend-triggered cycle. The final
build eliminates repeated input-wakeup aborts by disabling the FT6336U
IRQ before sending its hibernate command; automatic suspend then succeeds
on its first attempt, with no reported suspend failures. ADB reconnects
and the KMS framebuffer survives;
the owner confirms power-key wake from actual standby, with display and
touch intact. The physical-counter suspend-to-idle entry freezes timekeeping
correctly, so Android counts sleep separately from suspend overhead.
The final build completes two automatic suspend cycles (power-key and RTC
wake), with success=2 and fail=0. Android accounts 61.382 seconds of sleep
separately from 0.753 seconds of total suspend overhead.

Android's existing stay_on_while_plugged_in setting was 15. It is now 0,
with screen_off_timeout retained at 60000 ms; the product default was
already false. The temporary a1260-bringup kernel wake lock has been
removed from the vendor init script after the wakeup tests. Holding an
explicit maintenance lock during a host update is not a boot-time policy.


Calibrated SoC thermal monitoring and USB standby policy
------------------------------------------------------

The thermal controller at 0x1100d000 controls AP AUXADC channel 11 via
AHB. Its one bank contains three MCU sensors, selected with mux values
0, 1 and 2; the MT2701 ABB mux value 16 does not apply. TOPCKGEN gates
CLK_GATING_CTRL1[1] and [30] supply the thermal and AUXADC clocks.
The sensor buffer is TS_CON0[7:6] at APMIXED+0x600, while sensor selection
uses TS_CON1 at +0x604. The BSP gain, ADC offset, temperature, sensor
voltage and signed slope conversion retain their integer rounding.

The read-only generic MediaTek eFuse provider supplies the thermal cell
at offset 0x194, length 12 (BSP devinfo indices 36--38). Live eFuse reads
match the original device backup; no per-device calibration words are
hardcoded in the DTS. The thermal zone reports the maximum of all three
sensors. The board currently uses a conservative 75 C passive trip with
5 C hysteresis and cpufreq cooling, and a 105 C critical shutdown trip.
These are bring-up board policy limits, not characterized silicon limits.
Critical shutdown has not been induced on hardware. Suspend pauses the
thermal AHB master, waits for idle and switches off the analog buffer;
resume restores channel 11, the buffer and all three sampling points.

An active MT6580 peripheral USB bus holds the MUSB device wakeup source,
as the BSP USB connection lock did. Bus RESET/RESUME acquires it;
SUSPEND/DISCONNECT and stopping/unbinding the gadget release it. A PC
connection can therefore keep ADB available with the screen off. This
is not a permanent boot-time lock or a blanket charging lock. USB host
suspend/resume preservation remains a separate hardware requirement;
the current PHY forces device VBUS/session validity. Screen locking and
backlight shutoff remain under Android control; stay_on_while_plugged_in
is zero and the current screen timeout is 60 seconds.

Initial thermal/DVFS validation reads approximately 39--58 C over idle,
Android startup and the short CPU checks. Lowering only the passive trip
from 75 C to 35 C temporarily reduced the policy cap from 1001 to
903.5 MHz; restoring 75 C restored the cap. Two rounds over all seven
OPPs and repeated 604.5/1300 MHz transitions pass integer hashes,
400000 shared atomic updates per run and 7200 total cross-CPU clock
migrations without time reversal. PLL PCWs and PMIC actual selector
readbacks match the selected frequency and the voltage plateau above.
These checks are short bring-up validation, not long-term qualification.

With build #24, screen-off Android state is Asleep, the display suspend
blocker is released and actual backlight PWM is 0/39347 ns. The USB
wakeup source remains active and automatic suspend success stays at zero
while ADB shell commands continue to complete. This distinguishes an
intentional USB activity lock from forcing the display to stay on.

The first unplug regression on build #26 exposed a separate FT6336U
empty-frame bug: TD_STATUS reported zero contacts, while the following
point bytes were all zero (only the last byte was 0xff). The generic
parser interpreted those as DOWN at (0,0), leaving BTN_TOUCH pressed.
Five of six aborted suspend attempts named event2 (the touchscreen);
the initial abort named the charger during the cable transition. The
retry backoff consumed nearly the entire RTC alarm interval, leaving
only 0.133 seconds of actual sleep. Sensor sampling and ADB recovered,
but this was not a clean long-enough standby validation.

The FT6336U match now handles zero-contact frames by releasing all slots,
and rejects point IDs outside the two allocated slots. Other controller
matches keep their existing parser behaviour. This complements quiescing
INTN before hibernation; it does not make touch a wake source or suppress
legitimate input wakeup accounting.

On build #27 the same zero-contact raw payload leaves BTN_TOUCH released.
The owner confirms tapping and swiping; captured events contain normal
tracking-ID termination and balanced BTN_TOUCH DOWN/UP. The owner clarifies
that two-finger operation has not been tried, so it remains unverified.

The subsequent build #27 unplug regression passes with success=1, fail=0,
no backoff and no event2 suspend abort. The test retains a maintenance
lock for two seconds after unplug so the legitimate charger-transition
event can settle, then releases it for Android SystemSuspend. Android
records 19.210 seconds including 0.390 seconds overhead: 18.820 seconds
of sleep. The RTC wakes through PMIC parent Linux IRQ47, thermal sampling resumes
(MONCTL0=7, three nonzero MSR readings and TS_CON0=0), and temperature
reads 43 C immediately after resume. Replug restores ADB and reacquires
the MUSB activity lock. The temporary maintenance lock is released at
the end; the normal schedutil policy and 1300 MHz cap remain active.

Clock ownership, MTCMOS and PMIC power-off
----------------------------------------

The Android product no longer passes ``clk_ignore_unused`` or
``pd_ignore_unused``. The diagnostic UP profile keeps its existing settings.
MT6580 DISP uses TOPCKGEN +0x20 bit 9 (SMI_MM_SW), SRAM power-down bits
11:8 with acknowledgement bit 12, and AXI protection bit 1. ISP uses
SPM +0x238, SRAM bits 11:8 and acknowledgement bits 13:12, with no bus
protection in the BSP sequence. MFG keeps its existing index 0; DISP and
ISP are indices 1 and 2. Unused ISP is eligible for automatic power-off.

DISP has an explicit ALWAYS_ON policy: its SMI/LARB register restore is
not implemented. This permits generic clock/domain cleanup without losing
firmware bus context. Build #31 adds Utgard runtime MFG gating, with real
render/resuspend and two display off/on cycles verified. Build #32 replaces
the temporary critical MPLL_D5 reference with the firmware-selected SMI,
USB, MFG, MSDC0 and display PWM parent relationships. The D5 descendants
D10/D20/D40 propagate shared references, and D5 uses the hardware SET/CLR
aliases. SMI holds D5 while DISP remains on; GPU power-off no longer removes
the last reference. PLL rates and mux selections remain firmware-managed; see
Documentation/gpu/mali-utgard-mt6580.rst for evidence and limitations.
The complete PLL clock tree, modem/connectivity domains and SPM deep sleep
remain unfinished.

The MT6580 DSI handover explicitly enables engine/digital clocks before
stopping inherited video and polling registers. The earlier code depended
on firmware keeping both clocks enabled and timed out after late unused
clock cleanup. The normal PHY and display programming follows afterward.

MT6350 RTC now registers a final SYS_OFF_MODE_POWER_OFF handler. It repeats
the BSP PROT unlock (0x586a, 0x9136), disables RTC 32 kHz export only when
PDN1 bits 12:8 have no users, then writes BBPU=0x4309 and WRTGR. Other RTC
variants are unchanged. This path retains alarm power-on support and does
not unconditionally reboot into a charger mode. Actual charger-attached
power-off behavior still depends on the PMIC and boot firmware.

The MT6580 WACS2 regmap uses a spinlock and bounded atomic MMIO polling,
so the final power-off callback can operate after interrupts and secondary
CPUs have stopped. It does not take the RTC mutex or call a sleeping helper.

Hardware validation with build #29: Android boot_completed=1; the command
line contains neither global ignore flag. Late init logs clock and domain
cleanup, genpd reports ISP off and DISP/MFG on, and ISP control reads
0x3f12 with bit 5 clear in both SPM status registers. Two screen-off/on
cycles restore DSI video at 300 MHz with zero RDMA underflows or abnormal
interrupts. Build #28 exposed the missing DSI clock acquisition described
above; #29 corrects it.

Battery-only shutdown was exercised next: the device-side record detects
USB power removal at uptime 249.48 s and requests Android's normal shutdown
at 252.60 s. The owner reports the unit powered off and could be started
with the power key. ADB returned on a fresh #29 boot; DSI video, zero RDMA
underflows/abnormal interrupts, charging detection and calibrated thermal
sampling recovered. No temporary maintenance wake lock survived the boot.
This validates the observed battery-only shutdown/power-key startup cycle;
charger-attached shutdown and off-state current measurement remain untested.
