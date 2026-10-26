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

The USB drivers retain firmware PLL frequency. Build #42 adds the real
MT6350 VUSB regulator as the PHY core's ``phy-supply`` dependency.
The clock driver reports an error if UNIVPLL is off rather than inventing
PLL parameters. VUSB is enabled before the PHY power-on callback and released
after power-off by the generic PHY core. PLL initialization remains a firmware
dependency. The new USB PHY/clock compatible strings and clock IDs
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
the MT6580 vendor clock table. PLL retuning and complete pad control are
pending. ``mediatek,firmware-pins`` explicitly
opts into retaining LK's pad setup, restricted to non-removable legacy
eMMC at no more than 26 MHz. Build #42 replaces the old fixed 3.0 V VEMC
placeholder with the MT6350 regulator, preserving the actual firmware selector
of 3.3 V (0x052e[7]=1). It remains always-on until card power-cycle handling
is validated. No DDR,
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

Build #42 also exposes VUSB (3.3 V), VEMC (3.0/3.3 V), VIO18 (1.8 V) and
VIO28 (2.8 V). Their RG enable request is bit 14, while ``is_enabled`` reads
QI actual state at bit 15. VEMC selection is bit 7 of 0x052e. Boot and
screen-off registers match the pre-change values: 0x0500/0502/0508/0556
are 0xc000 and 0x052e is 0x03c9. VEMC is constrained to its existing 3.3 V;
MMC now correctly reports the 3.3--3.4 V OCR window, retaining 8-bit legacy
operation at an actual 17.333 MHz. Android boot_completed, a 4 MiB fsync/read
check and display-domain off/on checks passed; RDMA underflow/abnormal were 0.

Shared VIO18/VIO28 remain always-on because their consumers are not all
identified. The real normal-boot image's LCM_DRIVER has null init_power,
suspend_power and resume_power slots; its saved REA init evidence
``ev_b9409755d90e6cbeca8eb5d888009027ad278929be11da903136a719ab6b8e52``
only establishes reset/DSI initialization, not the physical supply wiring.
The panel placeholder and touch I/O dummy therefore remain explicit unresolved
dependencies; no speculative shared-rail switch-off is added. Evidence is in
``out/a1260-runtime-pm/pmic-rails-42-validation.txt``,
``pmic-rails-42-io.txt`` and ``pmic-panel-power-pointers.json``.

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
1397 mAh design capacity. The initial direct voltage-to-percentage lookup
produced 13% at 3715 mV and 50% at 3817 mV just ten seconds later when USB
was reinserted. Charging voltage was being mistaken for stored charge.

Capacity now uses the reference BSP's OAM second-observer approach: inverse
OCV at the retained fractional SOC, temperature/voltage-dependent internal
resistance, estimated current from (OCV - terminal voltage) / resistance,
and integration into charge. OCV and the 0/25/50 C resistance profiles are
interpolated continuously with temperature. Five BATSNS conversions are
averaged per sample. Boot initialization prefers the valid SWCHR startup
OCV in ADC9 (0x0726), with the BSP's 8 mV adjustment and voltage sanity
checks; an unavailable/implausible snapshot falls back to terminal voltage.
This startup value is used once, not re-read as a live OCV after each plug.

An external-power transition preserves SOC and does not apply its new
voltage to the preceding integration interval. Estimated charging current
is bounded by the configured charge limit; disconnected voltage rebound
cannot create charge. Connected net discharge remains possible. Full
anchoring requires 60 seconds of Full status above 4.1 V, and empty anchoring
requires 30 seconds below the board's minimum voltage while disconnected.
After suspend or a long sampling gap the integration timestamp is reset;
active current is not extrapolated through an unobserved sleep interval.

This is a software estimate, not measured current or a hardware coulomb
counter. ``gauge_state`` on the PMIC battery platform device exposes the
seed, model OCV, resistance and explicitly estimated current for diagnosis;
standard CURRENT_NOW/CHARGE_COUNTER are not fabricated. Installed-cell
capacity/resistance/OCV characterization, aging, retained SOC across reboot
and sleep-current accounting remain uncalibrated. The reference profiles
and startup-voltage fallback do not establish absolute SOC accuracy.

Build #41 reached Android boot_completed. In the physical USB test, the
last connected sample was 3853388 uV / 16% (SOC 164042 ppm). Disconnect
preserved SOC; over about 38 seconds the terminal voltage reached 3719882 uV
and SOC reached 163960 ppm, still 16%. Reinsertion preserved that state;
later charging reached 17% through integration. The charger returned to
Charging with a 500000 uA input limit; no gauge sampling error was observed.
The temporary wake lock was released and plugged-in stay-on restored to 0.
Replay also checked the original 3715/3817 mV transition, integration units,
discharge direction, rebound, temperature continuity and saturation.
Evidence: ``out/a1260-runtime-pm/battery-gauge-41-summary.txt``,
``battery-plug-41.log`` and ``battery-model-replay-result.txt`` in that directory.

The first hardware boot reports about 4.12 V and 27.3 C, presence true and
charging complete. Android's standard Health service discovers both supplies
and reports USB power and battery level. A real cable removal records
``online=0``/Discharging, followed by ``online=1``/Charging/Full on reinsertion.

The panel backlight is independent of the PMIC key. Its DISP_PWM at
``0x1100f000`` has the MT8173-compatible EN/COMMIT/CON0/CON1 layout, GPIO66
function 1 and a top clock gate at CLK_GATING_CTRL0 bit 0. The port retains
the firmware-selected 26 MHz or UPLL/12 source and connects ``pwm-backlight``
to the DRM panel, allowing panel disable/enable to switch the light off/on.
With no separate enable GPIO or regulator, ``pwm-backlight`` requests an
enabled zero-duty waveform during blanking. The MT6580 provider translates
that request to width 1 and PWM-disable in direct-update mode before
gating the source; leaving PWM enabled at width zero did not reliably blank
this board after display-domain power-off.
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

Build #35 replaces DISP's initial ALWAYS_ON policy with runtime PM and
SMI/LARB0 context restoration. Build #31 added Utgard runtime MFG gating, with real
render/resuspend and two display off/on cycles verified. Build #32 replaces
the temporary critical MPLL_D5 reference with the firmware-selected SMI,
USB, MFG, MSDC0 and display PWM parent relationships. The D5 descendants
D10/D20/D40 propagate shared references, and D5 uses the hardware SET/CLR
aliases. SMI holds D5 while DISP remains on; GPU power-off no longer removes
the last reference. PLL rates and mux selections remain firmware-managed; see
Documentation/gpu/mali-utgard-mt6580.rst for evidence and limitations.
The complete PLL clock tree, modem/connectivity domains and SPM deep sleep
remain unfinished.

Build #33 connects I2C0/1/2 and APDMA to the firmware-selected AXI parent,
instead of an independent 125 MHz fixed source. CLK_MUX_SEL0[7:4] is 4,
selecting MPLL_D12; CCF therefore reports 125.125 MHz. The I2C controller
retains its existing divide-by-ten timing. AXI carries a critical reference
for the CPU and unported infrastructure. AUXADC now has the BSP 26 MHz
parent. The DDR/MEMPLL AXI selection is rejected until a DDR clock provider
exists. Android boot, input registration, charging and thermal reads passed
on the device; evidence is in
``out/a1260-runtime-pm/peripheral-clocks-validation.txt``.

The MT6580 DSI handover explicitly enables engine/digital clocks before
stopping inherited video and polling registers. The earlier code depended
on firmware keeping both clocks enabled and timed out after late unused
clock cleanup. The normal PHY and display programming follows afterward.

DISP context restoration
~~~~~~~~~~~~~~~~~~~~~~~~

The MT6580 MMSYS node now owns three register resources: MMSYS at
0x14000000, LARB0 at 0x14016000 and SMI common at 0x14017000. Runtime
suspend waits for LARB0 to be idle before saving the BSP common-register
list, seven port OSTD settings, LARB control, virtual-channel settings,
MMU/security/domain registers, MMSYS gates, DCM and reset outputs. genpd
then asserts bus protection, powers down SRAM and removes DISP power.
Resume opens the COMMON/LARB gates before restoring context; DRM rebuilds
RDMA, mutex routing and DSI state. A future SMI/IOMMU driver must take over
this LARB ownership rather than map the same resources concurrently.

The display clock provider participates in runtime PM, so clock users hold
MMSYS active. LCM reset GPIO operations also acquire a runtime reference.
The panel requests host pre-enable before its prepare sequence, and DSI
holds its clocks throughout PHY power-on/off. DSI and RDMA quiesce their
IRQs before releasing clocks; probe and diagnostic register reads likewise
hold the provider powered. The MMSYS ``power_context`` attribute only reads
software counters and therefore does not wake a suspended display domain.

Build #35 reached Android boot_completed with normal on-screen rendering
confirmed by the user. Across four screen-off/on cycles, the two SPM power
status words showed DISP bit 3 clear while off, and SMI/D5 enable counts
were zero. Off-screen EGL rendering returned RGBA 0,255,0,255 without a GL
error, after which both DISP and MFG were off (status 0xbe45/0xbe41).
Reading DSI/RDMA diagnostic attributes while off safely resumed and released
the domain. Configuration counters reached eight saves/eight restores with
no busy rejection; subsequent RDMA underflow and abnormal counts stayed
zero. The user confirmed touch remained normal after these cycles.

That test exposed a separate backlight issue: the generic pwm-backlight
zero-duty request left PWM enabled and the light remained on after DISP
power-off. The BSP explicitly avoids HIGH_WIDTH=0, using width 1 with
PWM_EN=0 followed by COMMIT instead. Applying that sequence live fully
extinguished the backlight, as confirmed by the user. The MT6580 PWM
compatible now implements this zero-duty behavior and releases its clocks;
nonzero brightness continues through the normal PWM path.

Build #37 also flushes COMMIT and waits two programmed PWM periods before
gating the source (about 80 us for this board), allowing the output update
to settle. Raw register reads with the PWM clock gated proved unreliable:
some returned a value from a different offset. They cannot establish the
latched output state. The debug PWM interface temporarily enables its
clock, so checking the physical backlight is also necessary; a debug read
alone cannot prove that the preceding shutdown completed before gating.

Follow-up on build #37: the user reported that the screen could not be
woken after blanking. ADB still executed commands and Android logged the
physical power-key wake, an ON transition, then a timeout ten seconds
later. No kernel crash was observed in that capture. This is an unresolved
visible wake failure, not a passed backlight restore test or evidence of a
whole-system suspend hang. Preserve #32 as the standard product boot until
the failure is diagnosed. Captures are
``out/a1260-runtime-pm/wake-failure-37{,-later}.txt``.

Live diagnosis after reboot isolated the black-screen symptom to PWM output
updates. With the display held awake, DSI was running at 300 MHz and RDMA
had no underflows, but PWM_EN=1 and HIGH_WIDTH=1024 did not produce a high
GPIO66 input readback. Extending the COMMIT pulse did not change this.
Writing PWM debug 0x20=3, the BSP's immediate-update setting, and rewriting
the duty/enable registers changed GPIO66 from low to high. The user confirmed
the backlight and normal image returned. The provider now selects this
direct-update mode for both nonzero duty and shutdown instead of relying
on COMMIT; get_state does not change that mode as a side effect. The exact
missing shadow-latch trigger has not been established. Evidence is in
``out/a1260-runtime-pm/pwm-direct-live.txt`` and the physical user confirmation.

The earlier USB enumeration failure followed an adbd restart while the
screen was off. Restarting adbd with Android temporarily kept awake did
reconnect, without rebooting the kernel. USB/system-suspend recovery must
be investigated separately; the PWM finding does not establish its cause.

Build #39 validation: Android boot completed, an off/on cycle powered DISP
and MFG off and restored scanout without RDMA underflow or abnormal events.
The user then manually confirmed complete backlight extinction and normal
image/backlight recovery on the power key. The temporary plugged-in stay-on
setting was restored from 2 to its original value 0. Standard product
boot/kernel now use this build. This supersedes the #37 visible-wake failure;
USB re-enumeration across system sleep is still unresolved. Evidence is
``out/a1260-runtime-pm/pwm-direct-39-validation.txt``.

USB gadget handoff during screen-off
-----------------------------------

Build #42 reproduced failed re-enumeration after restarting adbd while the
screen was off. The retained local log shows six successful system suspend
cycles, zero suspend failures and PMIC wake IRQs. Replacement adbd/FunctionFS
initialization was repeatedly frozen after the old gadget released its link
wake reference; an init action took 59933 ms of wall time. The final UDC state
was not attached, FADDR zero. Keeping a temporary userspace wake reference for
five seconds around the same restart allowed re-enumeration, without a kernel
reboot or a PHY register change. This distinguishes the observed gadget setup
race from a demonstrated PHY restore failure.

Build #45 adds an MT6580 peripheral-only enumeration wake source. Gadget
startup and software UDC stop arm a maximum five-second handoff. RESET/RESUME
take the normal active-link reference before releasing this timed source.
SUSPEND/DISCONNECT continue to release the active-link hold; the enumeration
source expires independently. Its separate lifetime avoids turning an existing
indefinite active-link reference into a five-second timeout. It is not a global
charger wake lock and does not force the screen on.

Hardware validation: native screen-off adbd restart recovered to configured
with no additional system suspend and about 660 ms of handoff activity.
With the standard USB configuration deliberately held at none, the handoff
expired after 5014 ms, one actual s2idle/RTC wake completed with zero failures,
and restoring adb re-enumerated successfully. A direct UDC write was insufficient
for that negative test because FunctionFS automatically re-bound the gadget;
only the standard none/adb configuration test establishes timeout behavior.
Temporary userspace locks were released and plugged-in stay-on remained 0.
Evidence is in ``out/a1260-runtime-pm/usb-sleep-repro-42.log``,
``usb-sleep-handoff-42.log``, ``usb-sleep-native-45-validation.txt`` and
``usb-idle-timeout-45-validation.txt``. Bulk transfers, host-directed bus
suspend/remote wake and deeper PHY analog power-down remain separate work.

These are display/GPU runtime power-cycle checks, not validation of complete
system suspend with the new callbacks, DDR self-refresh or panel supply-rail
power-off. The latter still uses the firmware-powered fixed regulator.
Evidence for #35 is ``out/a1260-runtime-pm/disp-domain-validation.txt``.

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

Firmware PLL readback
---------------------

Build #46 replaces the fixed MAINPLL/UNIVPLL frequency declarations with
read-only APMIXEDSYS clocks. MAINPLL uses CON1[20:0] as a 7.14 PCW;
UNIVPLL uses CON1[6:0] as the integer feedback divider. CON1[26:24]
selects the post-divider. Enable state is read from CON0 bit 0. No PLL
tuning, enable, isolation or power register is written by these root clocks.
TOPCKGEN explicitly depends on both roots before publishing its consumers,
preventing an early consumer from seeing a zero-rate orphan.

Hardware reads at 0x10018124/0x10018144 were 0x800e7000/0x01000060:
with the 26 MHz crystal these produce 1501.5 MHz/1248 MHz, matching the
previous firmware settings. Android boot completed; USB bus/PHY, MSDC0,
GPU and SMI report 78/48, 208, 500.5 and 300.3 MHz respectively. A real
Mali GLES draw/readback passed. Screen-off powered DISP/MFG down and
released MPLL_D5; screen-on restored scanout with zero RDMA underflows
or abnormal events. Input devices, PMIC charging and thermal reads also
remained available. Captures are
``out/a1260-runtime-pm/pll-readback-46-{initial,validation}.txt``.

This is root readback, not full PLL power ownership. Most divider branches
still retain firmware gate state. In particular, the PMIC SPI mux can share
UPLL_D16 with USB and must acquire a real clock reference before that
divider may be gated. CPU frequency changes also temporarily use MAINPLL.
DSI engine/digital parent identity and the BSP's always-on RDMA clock
semantics need further evidence; an arbitrary parent or hardware gate must
not be invented for them.

PWRAP clock references
---------------------

Build #47 registers the AP PMIC SPI gate (TOPCKGEN 0x24 bit 20) and the
PMIC 26 MHz gate (bit 29). The SPI parent follows the existing mux at
0x00[25:24]: UPLL_D20, USB48M, UPLL_D16 or the 26 MHz crystal. This board
reads selector 3, using 26 MHz, consistent with the BSP wrapper's setup.
The full UPLL divide-by-two chains are described so alternate firmware
selections also propagate their references through the correct branches.
Their hardware divider gates still retain firmware ownership.

PWRAP acquires and enables the DT clocks before the first WACS2 read and
keeps them for its lifetime. This protects atomic RTC/power-off register
access and normal PMIC interrupt/regulator users without changing the
firmware mux, wrapper sampling, DCM or reset state. Older bring-up DTs
without clock properties retain firmware ownership. These references
must stay in place when divider gating is introduced later.

Hardware validation: Android boot completed; both clocks report 26 MHz
with one enable reference, and WACS2 register reads, battery status and
thermal sampling work. Holding USB configuration at none allowed one
successful s2idle cycle with zero failures, followed by a PMIC IRQ wake
with the RTC alarm armed. DISP/MFG were off, both PWRAP references remained
held after resume, and restoring adb re-enumerated automatically. Screen-on
restored RDMA with zero underflows or abnormal events. Temporary wake locks
and the RTC alarm were cleared; plugged-in stay-on remains 0. Evidence is
``out/a1260-runtime-pm/pwrap-clocks-47-{initial,suspend,final}.txt``.
This does not establish DDR self-refresh or full PLL power-down support.

PLL divider gate visibility and ownership
----------------------------------------

Build #48 reads the actual FREDIV_EN bits for the 16 MAINPLL and 12 UNIVPLL
divider gates. The previous fixed-factor declarations for 27 of these branches
could not reflect a cleared gate bit; MPLL_D5 already had hardware gate control.
Rates and parent chains are unchanged.
The enable indication describes the local gate bit, not a measured running
clock when an upstream gate or PLL is off.

The driver manages MPLL_D5 and now UPLL_D16/UPLL_D20 using the hardware SET/CLR
registers, followed by a read to flush the write. UPLL_D16 feeds the USB bus
and one PMIC SPI mux input; UPLL_D20 feeds another PMIC SPI input. Their
consumers have clock references, including the PWRAP references added in
#47. The other 25 divider gates expose read-only hardware status and retain
firmware ownership. Their unported consumers, such as UART/audio/camera/DDR
paths, must be modeled before those gates can be switched automatically.
MAINPLL/UNIVPLL tuning and power remain under firmware control.

On hardware UPLL_FREDIV_EN changes from 0xffffffff to 0xfffbffff: unused
UPLL_D20 bit 18 is cleared, while UPLL_D16 bit 24 remains set for configured
USB. PMIC SPI remains on the 26 MHz crystal, with both wrapper clock
references held and register reads/charging/thermal sampling working.
Android boot and a real Mali GLES draw/readback passed. An off/on cycle after
boot powered DISP/MFG off, cleared MPLL_D5 bit 2, and restored RDMA with zero
underflows or abnormal events. A USB-none/RTC check recorded two successful
s2idle cycles, zero failures and PMIC IRQ wakes, then automatically restored
configured ADB. PWRAP references and divider states survived. Temporary wake
locks and the RTC alarm were cleared, and plugged-in stay-on remains 0.
Captures are ``out/a1260-runtime-pm/divider-gates-48-`` followed by
``initial.txt``, ``validation.txt``, ``suspend.txt`` and ``final.txt``.

The display clock audit still finds only gate controls for DSI_ENGINE and
DSI_DIGITAL, with no source recorded in the BSP clock manager. RDMA0 is
explicitly AO there. Preserve these semantics until parent identities can
be established; the absence of a parent frequency in clk_summary is not
evidence that its gate or the DISP runtime power sequence is missing.

MSDC0 peripheral gate correction
--------------------------------

Build #49 corrects the MSDC0 gate from TOPCKGEN 0x20 bit 17 to 0x24 bit 17,
as specified by MSDC0_SW_CG in the BSP's CG_CTRL1 group. Previously the MMC
driver's runtime references did not control the real peripheral gate, which
remained in firmware state. The source, card clock, pads and voltage settings
are unchanged.

Hardware validation wrote and fsynced 4 MiB of random data from a RAM file
using page-aligned O_DIRECT buffers. Four subsequent O_DIRECT reads matched
the SHA256 of the original RAM data. Each read began with the controller
runtime-suspended, clock enable count zero and 0x10000024=0x98839a3c (bit 17
set), so each operation exercised real ungating and register restoration.
The active state reads 0x98819a3c (bit 17 clear). Android boot completed,
and the temporary files and maintenance wake reference were removed.
Evidence is ``out/a1260-runtime-pm/msdc-gate-49-io.txt``.

The initial Toybox dd O_DIRECT attempt returned EINVAL because its buffer
uses unaligned malloc; that attempt does not establish an MMC transfer
failure or a successful media write. The completed check used the existing
ARM musl toolchain to build the small page-aligned I/O helper in the capture
directory. This is still legacy 17.333 MHz operation, not high-speed tuning
or complete eMMC qualification.

UART gate and source ownership
------------------------------

Build #50 connects UART0/1 to TOPCKGEN 0x24 bits 10/11. The baud source
follows the retained firmware mux at 0x00 bit 0/19 respectively: 26 MHz
or UPLL_D24 (52 MHz). The DTS keeps the driver's existing single-clock
interface. UART1 remains disabled on this board.

The UART selected by stdout-path has a critical clock reference while
8250 console support is enabled: console writes can run with interrupts
disabled and must not depend on runtime PM. An enabled firmware UART
without an available Linux DT consumer is also retained explicitly.
These are clock references, not system wake locks. UPLL_D24 can now be
gated when neither UART source needs it. Its upstream divider gates retain
firmware ownership. Before deep SPM/PCM work, any additional autonomous
clock users must be included in that subsystem's clock dependencies.

Hardware validation: Android boot completed, both UART source muxes read
26 MHz, UART0 has its critical reference and UART1's gate is off. Unused
UPLL_D24 bit 25 is now clear (UPLL_FREDIV_EN=0xfdfbffff). A successful
s2idle/PMIC IRQ wake restored configured ADB and retained the console
reference. The first suspend attempt returned EBUSY from alarmtimer before
the successful retry, matching its near-deadline alarm guard; this capture
therefore records one success and one abort, not zero failures.

After resume, another 4 MiB page-aligned O_DIRECT write/fsync and four direct
reads matched the original RAM data; each read started from a suspended,
gated MSDC0. RDMA frame counts advanced after screen-on with zero underflows
or abnormal events. Temporary files, the maintenance wake reference and RTC
alarm were cleared; plugged-in stay-on remains 0. Captures are
``out/a1260-runtime-pm/uart-clocks-50-`` followed by ``initial.txt``,
``suspend.txt``, ``suspend-stats.txt`` and ``final.txt``. Physical UART1,
alternate 52 MHz selection and panic transmission were not exercised.

UPLL divider tree ownership
--------------------------

Build #51 gives all 12 UPLL divider gates SET/CLR operations, keeping their
BSP parent chains. The configured USB bus retains D2 -> D4 -> D8 -> D16;
MSDC0 retains D3 -> D6 only while its runtime clock is enabled. This also
allows unused D5/D10, D7 and D12 branches to stop, in addition to the D20
and D24 leaf gates already owned in earlier builds. The PLL itself remains
read-only and its separate 48 MHz outputs retain their existing consumers.

Before unused-clock cleanup, unported MSDC1, SPI and CAM leaves retain a
critical reference to their selected firmware parent if their peripheral
gate is open. The driver does not change those leaf gates or muxes. An
unknown active CAM mux is rejected instead of guessing a parent; an off
leaf needs no hold. On this board only MSDC1 needs a hold, on the 26 MHz
crystal. Future consumer drivers must replace these firmware holds with
normal runtime clock references. The remaining 15 MPLL divider gates stay
read-only; display and DDR dependencies are still incomplete.

Hardware validation completed Android boot and a real Mali GLES draw with
pixel readback 0,255,0,255 and no GL error. UPLL_FREDIV_EN is 0xfdf9fbf3
while eMMC has its source enabled and 0xfdf9f9f1 when D3/D6 are gated;
D2/D4/D8/D16 remain referenced by USB. A 4 MiB page-aligned O_DIRECT
write/fsync and four direct reads matched SHA256. Round 3 sampled MSDC0
runtime-suspended, its peripheral gate closed and both D3/D6 references
zero before a matching read. Other sequential samples overlap background
Android I/O and must not be treated as atomic clock snapshots.

With USB configured to none, RTC wake completed one s2idle cycle with zero
aborts. PMIC IRQ resumed the system, ADB re-enumerated automatically and
UPLL divider references retained their expected idle state. DISP/MFG were
off during screen-off; after wake, RDMA frame counts advanced with zero
underflows or abnormal events. Temporary wake references, RTC alarm and
I/O files were cleared; plugged-in stay-on remains 0. Captures are
``out/a1260-runtime-pm/upll-tree-51-`` followed by ``initial.txt``,
``render.txt``, ``io.txt``, ``off.txt``, ``suspend.txt`` and ``on.txt``.
Alternate firmware mux selections and full PLL power-down are untested.

MPLL consumers and memory clock dependencies
-------------------------------------------

Build #52 extends SET/CLR ownership to the remaining 15 MPLL divider gates.
The BSP's initialization explicitly exempts MPLL_D14 from unused-divider
cleanup, so ``mt6580_fw_d14`` retains it and D7. Its undocumented downstream
route is not assigned as a guessed DSI parent. The CPU's atomic ARMPLL
handover uses PLL2_CK from MPLL_D2; ``mt6580_cpu_handover`` keeps that branch
available throughout a rate transition. AXI retains D3/D6/D12; GPU and SMI
keep their selected normal runtime references. Firmware NFI, audio and
SCAM consumers retain their complete selected parent only while their leaf
gates are open. The board's active NFI uses 26 MHz and audio uses MPLL_D12.

The separate DDRPHY configuration mux is always referenced, on 26 MHz or
MPLL_D16 according to SEL1 bit 15. The live EMI mux follows SEL0 bit 1 and
is also critical. Neither its mux nor MEMPLL_D2 bit 31 is written. A new
read-only DDRPHY/DRAMC mirror provider derives the EMI memory source from
the BSP NCPO, divider, feedback and PLL mode fields, at one quarter of the
DDR data rate. The actual 1PLL configuration yields 266499673 Hz, consistent
with DDR-1066 and the independent frequency meter's approximately 266.6 MHz
EMI1X result. DDRPHYCFG measures 26 MHz. Build #55 adds the explicit crystal
phandle; it has the same rate calculation and no DDR register writes.

Seven CPU frequencies from 604.5 through 1300 MHz, a real Mali GLES draw,
and two screen off/on cycles passed on #52. DISP and MFG powered off while
their unused MPLL_D5 stopped, then RDMA resumed without underflow or abnormal
events. RTC wake completed one s2idle cycle with zero failures; a 16 MiB
RAM file retained its SHA256 and ADB re-enumerated. Evidence is
``out/a1260-runtime-pm/mpll-ddr-52-`` followed by ``initial.txt``,
``validation.txt`` and ``suspend.txt``; frequency meter results are in
``clock-meter-51.txt`` in the same directory.

These references close unused-divider cleanup dependencies while retaining
firmware policy for the live memory PLL and the undocumented D14 route.
They do not implement DDR self-refresh, PCM sleep or dynamic memory rates.
The DSI engine/digital gate parents remain undocumented; their hardware
gates, SMI domain dependency and MIPI PHY references are already controlled.

MSDC0 pads and 52 MHz high speed
------------------------------

Build #55 removes the board's firmware-pins bypass. Pinctrl now owns
GPIO41..50's MSDC0 function, the additional IOCFG_B SELGP selector, the
Schmitt/input buffers, 3-bit drive fields, PUPD/R0/R1 pulls and shared
TDSEL/RDSEL fields. GPIO51 uses an input with pull-up on the GPIO path, keeping reset
deasserted; SELGP and the GPIO mux follow the BSP pad selection. It is not driven low or pulsed. The pinmux callback
also applies to GPIO/EINT requests through the common register setter;
other MediaTek SoCs retain their existing path. Drive code 2 is represented
as 6 mA, with command/data/reset pull-up, clock pull-down, TDSEL=10 and
RDSEL=0. No I/O voltage switch or HS200/HS400 capability is enabled.

The first 52 MHz build #54 negotiated high speed and read the system, but
writes timed out at CMD12, aborting ext4 journals. It is not a qualified
image. The original board register_setting specifies falling-edge command,
read-data and write-CRC sampling. The initial legacy FIFO bring-up had
omitted these settings. The board now declares them explicitly; read-data
sampling uses PATCH_BIT0.RD_DAT_SEL for the synchronous FIFO, not IOCON.DSPL.
The driver saves the resulting IOCON=0x10a and PB0=0x403c000f across runtime
and system PM. The repaired boot was written from RAM with the running
card clock lowered; normal init fsck checked cache/userdata before mounting.
No partition was formatted. Logs for the failed attempt are retained as
``emmc-hs-54-dmesg.txt`` in the capture directory.

The final #55 boot completed with 8-bit MMC high speed at an actual 52 MHz,
up from legacy 17.333 MHz. A 32 MiB O_DIRECT write/fsync and eight full direct
reads matched SHA256; the last four rounds began with the real MSDC0 gate
and UPLL_D3/D6 all off. Timings include hashing and background Android I/O,
so they are not a controlled storage throughput benchmark. MMC command/data
CRC, timeouts and the remaining err_stats counters were all zero.

A further s2idle/RTC cycle succeeded with zero aborts, retained the 16 MiB
RAM hash, restored ADB and kept the 52 MHz sampling configuration. After
resume a new 4 MiB direct write/fsync and readback matched, all MMC error
counters remained zero, and RDMA frame counts advanced without underflow
or abnormal events. PMIC power key, remote and touch input devices remained
registered. Temporary I/O files, wake references and the RTC alarm were
cleared; plugged-in stay-on remains 0. Standard product boot/kernel now use
the exact tested #55 image. Captures are ``emmc-hs-55-`` followed by
``initial.txt``, ``pads.txt``, ``io.txt``, ``render.txt``, ``suspend.txt`` and
``resume.txt`` under ``out/a1260-runtime-pm/``. The high-speed work does not
establish HS200/HS400 operation or identify the fixed I/O rail wiring.

Utgard GPU clock scaling
------------------------

Build #57 adds frequency-only OPPs and standard devfreq simple_ondemand
control at 107.25, 214.5, 300.3, 416 and 500.5 MHz, matching the MT6580 BSP's
``mt_gpufreq.c`` table. CCF owns the MFG glitch-free mux and its separate
non-glitch-free input: sources are MPLL_D14/D7/D5/D3 and UPLL_D3. CCF enables
the new source before mux handover and releases the old source afterwards.
The non-GF mux refuses changes while selected by the GF mux. These are
source changes, not PLL retuning.

All BSP steps require 1.15 V and share VPROC with the CPU. GPU OPPs therefore
contain no voltage requests; the target callback checks the real shared rail
is at least 1.15 V and changes only the clock. Existing CPU OPPs retain that
floor and may require up to 1.3 V. GPU downclocking must never lower a rail
still needed by the CPU.

The 100 ms deferrable monitor measures intervals with queued or running DDK
GP/PP jobs, from enqueue through completion, independently of the legacy
DDK utilization timer. This is job busy time, not a hardware cycle counter.
Runtime suspend stops the monitor before gating clocks; resume restores
clocks, resets sampling and restarts monitoring. Disabling MALI400_DEVFREQ
retains the fixed-clock integration.

Hardware validation exercised all five steps and repeated source changes,
with a correct green GLES pixel readback at each step. CPU was held at
1.3 GHz during that check and VPROC remained 1.3 V throughout GPU downclocking.
With simple_ondemand, an eight-second 1024x1024 GLES draw loop automatically
rose from 107.25 to 500.5 MHz, completed 1804 frames and returned the expected
pixel with no GL error. After the load it fell to 107.25 MHz; MFG then powered
off and the devfreq transition/time counters stopped advancing.

One USB-unbound s2idle/RTC cycle succeeded with zero aborts, retained the
16 MiB RAM SHA256 and automatically restored ADB. Subsequent GLES drawing
and display restoration passed with zero RDMA underflows/abnormal events.
eMMC retained 52 MHz high speed and all controller error counters were zero.
The final image also completed Android boot and screen off/on with idle MFG
power-off. No new GPU fault, warning or Oops was observed. Standard product
boot/kernel use the tested #57 image. Captures under
``out/a1260-runtime-pm/`` are ``gpu-devfreq-57-`` followed by ``manual.txt``,
``auto.txt``, ``suspend.txt``, ``resume.txt`` and ``final.txt``. Temporary
CPU clamps, runtime overrides, wake references and RTC alarms were cleared.

SPM deep-sleep source audit
--------------------------

The old MT6580 BSP provides a 725-word GPL PCM program named
``pcm_suspend_v14.0_auxadc_workaround3_6`` in
``drivers/misc/mediatek/base/power/spm_v1/mt_spm_sleep.c``. Its four vectors
are 26M wake/sleep and APSRC wake/sleep. The flow coordinates UART sleep
acknowledgement, GIC masking, CIRQ recording/flush, PCM register/event setup,
PMIC wrapper commands and wake-status cleanup. CPU/infra power-down also
requires CPU dormant context save/restore, GIC/L2 handling and a working
CPU0 resume entry; secondary CPU hotplug alone does not supply that path.

Specifically for MT6580, the suspend PWRAP command phase clears/sets the
MT6350 STRUP_AUXADC_START_SW bit, preserving the rest of the register. It is
not the CPU voltage table. The deep-idle phase separately switches VCORE
control. Modern WACS2 CPU voltage writes do not implement these autonomous
SPM command slots or phase handoff.

The next stages are CIRQ hierarchy integration, shared SPM ownership and
bounded PCM setup with CPU/infra/DDR power retained, then verified wake
capture before adding CPU dormant or DDR power transitions. SPM is already
used by SMP and genpd, so no initialization may blindly reset their live
state. The current kernel still uses s2idle/WFI, not PCM deep sleep, DDR
self-refresh or cluster power-down.

CIRQ and retained-state standby
------------------------------

Build #59 inserts the MT6580 CIRQ v1 hierarchy between peripheral IRQs and
SYSIRQ/GIC. The BSP specifies 126 CIRQ lines at 0x10202000, covering SPI
32 through 157 inclusive. Physical and virtual timer PPIs remain directly
on GIC. The CIRQ driver uses the existing v1 register layout and wake-routing
policy of the SoC sleep controller, without requiring a per-line wake
operation. Peripheral polarity is configured before SYSIRQ inversion.

An explicit retained-state ``standby`` entry now lets the suspend core take
CPU1--3 offline and execute syscore PM, including CIRQ record/flush. CPU0
uses WFI with its context, GIC, infra and DDR retained. PCM is not reset or
started, and no new PLL, memory or supply power-down is performed. The
entry is not advertised as PM_SUSPEND_MEM deep sleep: ``mem_sleep`` remains
``[s2idle] shallow`` and Android's default suspend behavior stays s2idle.
This gives a staged way to exercise the syscore path before a PCM port.

Three RTC-woken standby rounds succeeded with zero failures, with measured
sleep intervals of 8.067, 14.892 and 9.257 seconds. Each restored CPU0--3,
kept the 16 MiB RAM SHA256 and returned with CIRQ control disabled. The final
trace includes syscore_suspend, machine_suspend[2] and syscore_resume. PMIC
IRQ 47 supplied the wake event and ADB automatically returned configured.
DISP/MFG remained runtime off while the screen was off. A subsequent 4 MiB
O_DIRECT write/fsync and two direct reads matched RAM; eMMC stayed at 52 MHz
with every controller error counter zero. Mali GLES readback, automatic
GPU governor, display output without RDMA errors, PMIC ADC and thermal
sampling all recovered. Temporary files, wake references, RTC alarms and
tracing were cleared. Standard product boot/kernel now use this #59 image.

Captures under ``out/a1260-runtime-pm/`` are ``cirq-59-`` followed by
``initial.txt``, ``standby.txt``, ``trace.txt`` and ``resume.txt``. This
qualifies the retained-state standby path only. CIRQ edge replay across
actual GIC/MCUSYS power loss, PCM sequencing, DDR self-refresh and CPU0
dormant resume still require their own implementation and hardware checks.
The BSP uses a software pending-bit replay during its deep-sleep flow;
that path has not been substituted for the generic driver's hardware flush
without testing the actual power-loss case.

PCM instruction loading and PWRAP suspend slots
---------------------------------------------

Build #62 adds an optional suspend child of MT6580 SCPSYS. Its lifetime is
managed with the suspend provider; failed binding clears the provider before
releasing its mapped registers or coherent DMA buffer. SCPSYS continues to
own MTCMOS domains. The fallback architecture standby remains available when
the staged SPM driver is disabled, and Android still defaults to s2idle.

The driver retains the BSP's unmodified GPL 725-word
``pcm_suspend_v14.0_auxadc_workaround3_6`` program and four event vectors.
Probe only allocates/copies a 32-bit coherent DMA buffer; it does not reset
or start a firmware PCM. Explicit standby refuses to proceed if the PCM is
not in its stopped/default state or power-output control is enabled.

Sleepable late preparation reads the actual MT6350 STRUP_CON10 (0x004e),
preserves all bits except AUXADC_START bit 4, and installs clear/set commands
in PWRAP slots 4/5 (0x104..0x110). These are the indices used by this PCM,
not normal VCORE slot 0. Only those two slots are saved/touched and the exact
previous words are restored after standby, including aborted entry. Staging
a command does not execute it or change a PMIC register. Normal WACS2
regulator, RTC and power-off accesses retain their existing serialization.

After device/genpd suspend and secondary CPU power-off, the atomic enter
callback saves the PCM controls, instruction pointer/length and four vectors.
It resets PCM and fetches the coherent buffer with bounded 10 ms polls,
then enters the already-qualified retained WFI standby. PCM_KICK is never
asserted, the PCM timer/watchdog and power outputs are never enabled, and
no DDR, PLL or MTCMOS control is written. After wake, another bounded reset
restores the saved controls and vectors, suppressing any sampled command
bits. This qualifies instruction fetch and restoration, not PCM execution
or deeper sleep. The phase ordering serializes these PCM-only accesses
against the completed genpd and CPU hotplug operations; it does not yet
provide arbitration for an autonomously running PCM.

Two #61 standby rounds and the final #62 round completed with zero failures.
The instruction-fetch FSM reached 0x02c48610 and restored to 0x02848490;
CON0/CON1, IM pointer/length and power IO matched their original values.
Each round saved/restored the slots successfully; the observed staged
values were 0x004e/0x0000 and 0x004e/0x0010, with all four original slot
words zero. The PMIC register itself remained zero. Four cores resumed,
16 MiB RAM SHA256 remained identical, PMIC IRQ 47 woke the system and ADB
returned automatically. Measured sleep intervals were 11.140, 11.345 and
11.865 seconds. Runtime-off DISP/MFG, subsequent GLES pixel readback and
zero-error RDMA output passed. On #62, a 4 MiB O_DIRECT write/fsync and two
direct reads matched RAM at 52 MHz, with all MMC error counters zero;
thermal and PMIC ADC sampling also resumed.

Read-only ``state`` and ``spm_phase`` diagnostics are documented in
``Documentation/ABI/testing/sysfs-platform-mt6580-spm``. Evidence under
``out/a1260-runtime-pm/`` is ``pcm-59-baseline.txt``, ``pcm-61-`` followed by
``initial.txt``, ``standby.txt`` and ``resume.txt``, and ``pcm-62-`` followed
by the same suffixes plus ``final.txt``. Standard product boot/kernel now
use the tested #62 image. Temporary files, RTC alarms and wake references
were removed; plugged-in stay-on remains zero.

Next work must add SPM IRQ ownership, actual PCM execution/wake-status
handling and UART/clock request coordination while retaining power first.
Only then should CPU0 dormant resume, GIC-loss edge replay and DDR/cluster
power transitions be enabled. No PCM sleep-power claim follows from an IM
fetch completing successfully.

PCM execution with retained power
--------------------------------

The SPM provider now owns IRQ0 (SPI 88) and its 26 MHz register clock.
The clock remains critical because SCPSYS and secondary CPU hotplug also
access this register block. The separate BSP fast clock was tried during
timer diagnosis and was not the cause; it is not added as a clock consumer.

Explicit standby now executes the same 725-word program after devices,
power domains and secondary CPUs have completed suspend. It synchronizes
R0/R7 from POWERON_VAL0/1, connects R7 for timer/control sequencing, and
keeps R0 crystal control disconnected. The retained flags (0x3bf), APSRC
and 26 MHz requests keep CPU0, GIC, infra, memory and supplies retained.
An optional 0..60000 ms PCM timer defaults to zero (external wake only).
Android's default remains s2idle; no CPU0 dormant or DDR power transition
is enabled by this change.

Both instruction fetch and PCM return have bounded polls. After WFI the
driver captures the timer, R11/R12/R13, return ISR, wake status, pending IRQ,
assertion and both power-status registers. An external IRQ may return WFI
before PCM completes, in which case CPU_WAKE requests its exit and the
forced return is not misreported as the original SPM wakeup. Cleanup
disconnects power output, masks/acknowledges the source, stops PCM and
restores controls, vectors and command slots. The AUXADC START bit is
restored independently of other PMIC state; command slots are released
even if the PMIC read fails.

The timer did not advance with both output banks disconnected, despite
correctly reading back its enable bit and compare value. Connecting R7
fixed the counter and produced an autonomous PCM return. R0 remains off;
no minimum-power claim follows from these retained-power tests.

Build #69 passed a final three-round sequence with the fast gate disabled:
two PCM timer sleeps of 2.000707 and 2.000699 seconds, followed by an
11.816427-second RTC sleep. The timer returned 65539 ticks for a 65536-tick
compare, R12=1 and wake-misc=0x002a0001, with IRQ59 pending and no forced
exit. RTC wake reported PMIC IRQ47 and the PCM return IRQ59; R12 recorded
the external sources and the timer remained zero. Every round restored
four CPUs, retained the 16 MiB RAM hash and matched both power-status
registers. R7 read 0x80, flags 0x3bf and retained source-request bits were
set. The stopped FSM restored to 0x02848490, power IO returned to zero,
and PWRAP preparation/restoration counts matched. An earlier single timer
round on the same build also passed. DISP/MFG were runtime off afterward.

Captures under ``out/a1260-runtime-pm/`` are ``pcm-run-69-`` followed by
``initial.txt``, ``first-standby.txt``, ``standby.txt`` and ``resume.txt``.
The AUXADC START commands were staged/restored, but the retained-power
path did not exercise their 26 MHz off/on event sequence; those snapshots
remained zero. UART/clock request coordination, CPU0 dormant context,
GIC-loss edge replay and DDR/cluster power transitions remain separate
work before enabling deeper sleep.

Post-wake #69 regression passed a 4 MiB O_DIRECT write/fsync and two direct
reads with identical hashes, eMMC 52 MHz with all error counters zero,
Mali GLES pixel readback and RDMA with no underflows/abnormal frames.
CPU policy remained 604.5--1300 MHz, GPU used simple_ondemand, and thermal,
battery ADC, charge status and 500 mA input limit recovered. The standard
product boot/kernel use this #69 image. Temporary RTC/timer settings,
maintenance wake references and I/O files were cleared; plugged-in stay-on
remains zero.

One charger PM race was captured during the explicit standby checks:
power_supply_changed_work tried to read BQ24190 current after i2c-0 was
suspended, producing ``Transfer while suspended`` and -ESHUTDOWN. The
resume and subsequent charge readings completed, but this workqueue/PM
ordering needs a separate fix; these checks do not establish warning-free
charger suspend. The full trace is ``pcm-run-69-dmesg.txt``.

Charger I/O across system suspend
--------------------------------

Build #70 closes the BQ24190-family SMBus entry points at the end of the
charger's system suspend callback. A mutex drains any active transfer and
serializes subsequent reads/writes with this gate before the parent I2C
adapter is suspended. System resume reopens access after the adapter's
noirq resume. Runtime status checks also respect the gate. Concurrent
power-supply notifications omit unavailable properties with -ENODATA
instead of reaching the suspended adapter; resume sends a fresh change
notification. Input-current-limit delayed work now uses the freezable
queue, matching the existing polling worker. This is local to the charger
and does not suppress the I2C core's suspended-transfer checks.

The final rapid sequence completed eight 250 ms PCM standby rounds,
advancing suspend success from 2 to 10 with failure count unchanged at 1.
Each timer reported 8195 ticks for an 8192-tick compare, four CPUs returned,
and the 4 MiB RAM hash matched. Status remained Full and the input limit
500000 uA on every resume; battery ADC and thermal sampling recovered.
A preceding two-round run was interrupted by a pending Android alarm:
alarmtimer returned -EBUSY on the third attempt before PCM entry. That
cancelled suspend and subsequent recovery are recorded, not counted as a
successful sleep or a new charger failure.

The final trace retained all 15179 events with no buffer overruns/drops.
Across eight charger suspend/resume pairs, 426 SMBus requests were outside
the closed interval; none occurred between charger suspend completion
and resume entry. Seventeen power-supply change workers ran, all outside
that interval, so this trace validates ordering but does not establish
that a notification actually raced the new gate during this sequence.
No suspended-I2C warning, property-report failure, Oops or charger read
error was captured on #70.

Evidence under ``out/a1260-runtime-pm/`` is ``charger-pm-70-`` followed by
``initial.txt``, ``first-check.txt``, ``check.txt``, ``trace.txt``,
``trace-summary.txt`` and ``dmesg.txt``.

Android's default s2idle then slept for 9.946200 seconds and resumed on
PMIC RTC IRQ47, increasing success to 11 with the earlier failure count
still 1. Charger status, 500 mA input limit and battery ADC recovered;
Mali pixel readback and zero-error RDMA also passed. This evidence is
``charger-pm-70-auto.txt`` and ``charger-pm-70-final.txt``. Standard product
boot/kernel now use #70. Tracing, filters, buffer/clock overrides, timer,
RTC alarm and maintenance wake references were cleared/restored; Android
plugged-in stay-on remains zero. UART/clock request coordination is next.

UART sleep-request coordination
------------------------------

The MT6580 UART now performs the BSP SLEEP_EN (0x48), SLEEP_REQ (0xb4)
and SLEEP_ACK (0xb8) handshake during system-suspend late, after the serial
core has stopped RX/TX and suspended console output. A bounded 10 ms poll
checks TX empty; sleep acknowledgment has a 1 ms bound. Temporary register
clock references stay held through the request and are released after
early resume clears it, waits for wake acknowledgment and restores the
original controls. An entry failure rolls back its request before returning
an error. A stuck wake acknowledgment retains its clock references and
reports the error instead of stopping the register clocks.

The new behavior is selected only for MT6580. A port left live by
no_console_suspend or tty wakeup skips the handshake and retains its clock
request. The console's critical clock and the PCM's existing APSRC/26 MHz
retention are unchanged. This qualifies UART handoff while system power
is retained; it does not yet permit crystal, infra or CPU0 power-off.
Cached request/restore/skip/error and ACK snapshots are documented in
``Documentation/ABI/testing/sysfs-platform-mt6580-uart``. Reading them never
accesses potentially unclocked UART registers.

Build #72 passed two PCM timer standby sleeps (1.000727/1.000709 seconds),
an RTC standby sleep (8.409366 seconds) and default Android s2idle
(8.658305 seconds). Each of these four entries obtained sleep ACK=1,
cleared it to zero on wake and restored EN/REQ to their original zero
values. A separate 1.000721-second PCM standby with console_suspend=N
left the console live and correctly incremented skips without increasing
requests. The final snapshot is requests=4/restores=4/skips=1/error=0,
prepared=0, with the UART clock count back at its original 2. The physical
controls also read EN=REQ=ACK=0 afterward.

The first timer attempt was cancelled by alarmtimer with -EBUSY before
UART late preparation, then retried successfully. Trace retains all 298
events and confirms UART late completion before each of the five machine
sleep entries and early restoration before normal serial resume. No UART
handshake timeout, new I2C warning or Oops occurred. Four CPUs and the
4 MiB RAM hash recovered; PWRAP slots, 500 mA input limit, thermal sampling,
Mali pixel readback and zero-error RDMA also passed.

Evidence under ``out/a1260-runtime-pm/`` is ``uart-sleep-72-`` followed by
``initial.txt``, ``check.txt``, ``trace.txt``, ``trace-summary.txt``,
``dmesg.txt`` and ``final.txt``. Standard boot/kernel use #72. Tracing and
its overrides, maintenance wake references, RTC/PCM timers and the live
console test setting were cleared/restored; console_suspend is Y and
plugged-in stay-on remains zero. Before allowing 26 MHz or APSRC requests
to drop, audit the other consumers and require UART handoff eligibility;
a live-console skip must continue to retain those requests. CPU0 dormant
context and DDR/cluster power transitions remain later work.

Source-request ownership
------------------------

The MT6580 BSP assigns SRC_REQ bits 0/1 to software APSRC/F26M requests;
bits 2/3 are CCIF event masks, not more clock holds. This board boots with
SRC_REQ=0x0c. The retained PCM path now adds APSRC (0x0d) when every enabled
UART completed its late-suspend handshake and both DISP/MFG are off in
both power-status mirrors. Otherwise it also adds F26M (0x0f). It preserves
any pre-existing firmware F26M request and every event-mask bit, and
restores the complete original SRC_REQ on exit. DT lists the UART suppliers;
missing enabled UARTs are rejected and the UART driver supplies a cached,
non-sleeping readiness query. A live-console skip cannot authorize release.

The source audit separates these dependencies:

* APSRC remains forced for CPU0/infra/DDR retention. Removing it requires
  separate CPU0 and memory sequencing work.
* The new policy only omits this driver's extra F26M override. R0 crystal
  control remains disconnected, POWER_ON_VAL0 stays zero and the qualified
  POWER_ON_VAL1=0x60015830 baseline is not changed. An unrecognized baseline
  also keeps F26M asserted.
* Hardware arbitration masks in CLK_CON/AP_STANDBY/MMDDR_MASK and the
  CCIF masks are retained. DISP/MFG state and the pre-run R13/clock/control
  values are captured for diagnosis.
* R13 on this board still has MD1_SRCLKENA, MD1_APSRC_REQ and AP_MD1SRC_ACK
  asserted (bits 3/4/5). A product with no telephony does not establish
  that the SoC's firmware modem requests are idle. MD1's domain remains on;
  its requests must not be masked without qualifying modem ownership.
* USB glue still keeps its clocks enabled, and PWRAP/SPM/console retain
  their existing references. The software-request change does not qualify
  physical 26 MHz stop, USB deep-power recovery or minimum standby current.

Build #74 verified three eligible standby entries with before=0x0c,
applied=0x0d and blockers=0: PCM timer sleeps of 1.000725/1.000731 seconds
and an 8.649511-second RTC sleep. A live-console standby retained F26M,
applied=0x0f/blockers=1, and slept 1.000719 seconds. Each restored the exact
0x0c source register. POWER_ON_VAL0/1 remained 0/0x60015830, the audit
captured CLK_CON=0x082f0000, AP_STANDBY=0x029f0000 and MMDDR_MASK=0, and
both power-status mirrors matched before/after.

Android then completed two default s2idle entries, with the last recorded
sleep lasting 6.476948 seconds and RTC IRQ47 waking it. Total suspend
success/failure was 6/0. The 313-event trace confirms UART late/early
ordering for all six entries without overruns/drops. The final UART
snapshot is requests/restores=5/5, skips=1, error=0 and clock count 2;
four CPUs and the 16 MiB RAM hash recovered. A subsequent 4 MiB O_DIRECT
write/fsync and two direct reads matched RAM at eMMC 52 MHz, with all MMC
error counters zero. Mali GLES pixel readback, RDMA without underflows or
abnormal frames, thermal and 500 mA charge-limit readings recovered.
No new UART/I2C warning, Oops or PCM error was captured. The firmware-hold,
unknown-baseline and powered-domain fallback branches were not injected
on hardware; only eligible and live-console policies were exercised.

Evidence under ``out/a1260-runtime-pm/`` is ``src-72-baseline.txt`` and
``source-74-`` followed by ``initial.txt``, ``check.txt``, ``trace.txt``,
``trace-summary.txt``, ``dmesg.txt``, ``resume.txt`` and ``final.txt``.
Standard product boot/kernel use #74. Trace overrides, RTC/PCM timers,
temporary files and wake references were cleared/restored; console_suspend
is Y and plugged-in stay-on remains zero. CPU0 dormant context can next be
qualified with these retained clock/memory constraints, before attempting
physical clock stop or DDR/cluster transitions.

CPU0 MMU-off resume-entry qualification
-------------------------------------

The optional cpu0_mode=1 standby path calls cpu_pm_enter and
cpu_cluster_pm_enter for the generic VFP, GIC and timer notifier state.
It saves the shared INFRACFG boot vector, physical timer control/compare
and counter, programs cpu_resume_arm, then calls ARM cpu_suspend. The
finisher starts PCM and waits for wakeup before soft_restart uses the
programmed physical vector with the MMU off. A zero cpu_suspend result
therefore exercises cpu_resume, rather than just returning from the WFI
finisher. The original shared vector is restored before secondary CPUs
may start; physical timer and PM notifier state and the original FIQ mask
are restored too. Default mode 0 and Android s2idle are unchanged.

Build #76 completed the initial two-second run with result=0/resumes=1,
vector 0x80113000 -> 0x8011e1b8 -> 0x80113000, unchanged timer control and
a monotonically advancing counter. Four CPUs resumed, the 16 MiB RAM hash
matched and PCM/PWRAP state restored. Evidence is under
``out/a1260-runtime-pm/cpu0-76-`` with ``initial.txt``, ``check.txt`` and
``resume.txt``. This retains CPU0, L2, infra and memory power: it qualifies
the MMU-off entry and restore path, not a hardware reset or Boot ROM path.
The PCM CPU-power branch also powers L2 down, so cache/cluster restoration
and the BSP's sibling-core erratum sequence must precede enabling it.

CPU0/TOP/L2 hardware reset and recovery
-------------------------------------

The explicit cpu0_mode=2 path clears only CPU_PDN_DIS in the qualified PCM
flags (0x3be). ARM cpu_suspend saves the MMU context, and the finisher
flushes L1/L2 and exits coherency before starting PCM. Unlike mode 1, this
finisher never calls soft_restart: a zero cpu_suspend return identifies
entry through the programmed hardware reset vector. A WFI return without
reset restores cache/coherency state and is counted separately as an abort.
The physical resume wrapper restores writable L2CTLR fields before generic
cpu_resume enables caches. Banked registers, physical timer, L2 reset policy,
shared boot vector and CPU/cluster PM notifier state are restored too.

The PCM return handshake finishes before running the BSP erratum 802022
sequence: temporarily start each sibling in a physical GIC-CPU-disable/WFI
stub, power it off, then permit normal Linux secondary bring-up. Those
bounded power-status polls must use readl_poll_timeout_atomic: timekeeping
and syscore resume have not run yet. Build #79 proved four cold entries but
its non-atomic helper caused ktime_get warnings. Build #81 completed three
cold entries with matching erratum counts, no WFI abort and no such warning.
The 16 MiB RAM hash, four CPUs, 52 MHz eMMC direct I/O, Mali rendering and
zero-underflow RDMA recovered.

A qualification-only 30-second reset watchdog is armed after device PM and
restored before normal watchdog resume. Modes 1/2 reject PCM timers above
20 seconds; timer=0 requires an external wake within the guard interval.
This temporary guard must be removed or replaced by a normal PM policy
before using unbounded CPU-reset sleeps. Its reset expiry has not been
intentionally exercised. The stopped watchdog may retain IRQ/dual-mode
bits (0x5c); the guard now temporarily clears them and restores the exact
original selection. An enabled IRQ/dual watchdog is still rejected.
Build #81 initially rejected the disabled 0x5c configuration; later trials
worked after normal watchdog resume changed it to 0x14. This rejection
happened before PCM_KICK and must not be counted as a sleep.

The default remains cpu0_mode=0 and Android s2idle. Infra, DDR, APSRC and
crystal control remain retained even in mode 2; this is not complete deep
sleep. MD1/USB ownership, physical 26 MHz stop, DDR self-refresh, MCUSYS
edge-wakeup replay and long-term stress still require separate work.
Evidence is under ``out/a1260-runtime-pm/cpu0-79-*`` and ``cpu0-81-*``.

Build #82 then completed three first-attempt cold entries, with sleeps of
2.001072/2.001055 seconds (PCM timer) and 8.556566 seconds (RTC).
Cold/erratum counts were 3/3, WFI aborts 0 and suspend success/fail 3/0.
The first watchdog round restored 0x5c -> 0x15 -> 0x5c; later rounds restored
0x14 -> 0x15 -> 0x14. The shared vector was
0x80113000 -> 0x801310d8 -> 0x80113000. RAM hashes matched, L2CTLR/cache
policy matched, and direct eMMC I/O, Mali pixel readback and RDMA with no
underflows recovered. No kernel WARNING/Oops was captured. Evidence is
``cpu0-82-multi.txt``, ``cpu0-82-dmesg.txt`` and ``cpu0-82-resume.txt``.
Standard product boot/kernel use #82. Qualification mode, RTC/PCM timers,
trace overrides and temporary wake references were cleared/restored.
