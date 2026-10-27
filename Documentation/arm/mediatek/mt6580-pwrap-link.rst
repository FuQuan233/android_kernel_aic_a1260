MT6580 PWRAP link checks and bounded recovery
===========================================

The legacy ``mediatek,mt6580-firmware-pwrap`` binding retains LK's MT6350
SPI setup. The new ``mediatek,mt6580-pwrap`` binding additionally supports
initializing an unconfigured serial link before exposing PMIC children.
The native binding also supports bounded online link reconstruction after
consumer access and SPM voltage/command staging have been excluded. The
legacy binding cannot reconstruct the link without native suppliers.

Source evidence
---------------

The exact reference is the user's android_kernel_mediatek_k80 tree,
drivers/misc/mediatek/pmic_wrap/mt6580/pwrap_hal.c and pwrap_hal.h:

* wait_for_state_idle(), approximately line 445, checks INIT_DONE on every
  poll and clears WFVLDCLR left by a previous timed-out read. The previous
  new-kernel driver only checked its initial status snapshot. A result
  arriving later during the idle wait could consequently block it.
* wait_for_state_ready(), approximately line 502, also checks INIT_DONE on
  every poll. Losing initialization must return an error, not a result.
* pwrap_read_test(), approximately line 1245, reads DEW_READ_TEST 0x018c
  and expects 0x5aa5. This fixed signature is safe to read repeatedly.
* is_pwrap_init_done(), approximately line 1438, skips the entire old
  initialization if INIT_DONE2 is already set. No resume callback in this
  source resets or recalibrates the retained wrapper.

These are source observations, not a claim that the original firmware
binary's entire PWRAP initialization has been decompiled and compared.

Implemented behavior
--------------------

Every idle/read/write wait checks INIT_DONE. Waiting for idle before a
new command clears any VALID result, including one arriving after the
first poll. Waiting for the current command never discards its result.
Each wait remains atomic and bounded to the existing 10 ms delay budget;
there is no dependency on running timekeeping, sleeping mutexes or IRQs
in the transport. A shared atomic regmap lock still serializes all public WACS2 users,
including final atomic power-off. A separate short spinlock protects only
diagnostic counters, never a bus transaction.

No failed command is retried. A write timeout does not prove the write
failed to commit; a read can consume interrupt status or other side effects.
Cleanup before the next command cannot reconstruct the failed command's
result and therefore does not turn that earlier error into success.

The fixed DEW signature is checked before exposing PMIC children at probe,
before staging any suspend voltage/PCM changes, and after normal suspend
recovery has restored VPROC ownership, AUXADC START and PCM slots. Recovery
errors are preserved; the check does not replace those restorations. The
read-only link_state sysfs attribute provides another explicit check plus
transport error counters, described in ABI/testing/sysfs-platform-mt6580-pwrap.

Serial-link initialization
--------------------------

The old pwrap_init() gates TOPCKGEN clocks, asserts wrapper SWRST and
TOPRGU bit11, manually resets the SPI slave, scans 24 SI_CK/SIDLY points,
configures register clock/dummy cycles, enables DIO, establishes cipher
state, performs a write test, then enables CRC, arbitration, watchdog/
status updates and autonomous ADC before setting INIT_DONE0/1/2.

That sequence cannot run concurrently with regulator, RTC and PMIC IRQ
transactions or live SPM command slots. It changes both ends of the bus,
can discard other masters' requests, and the reference has incomplete
failure rollback. In particular, a cipher-ready timeout can continue
looping, and several DIO/cipher failure exits do not restore arbitration.
The actual reset is TOPRGU bit11, not the commented-out INFRA reset bit7.
The old file also does not establish every board pin or initial PMIC rail.

The native compatible now implements that sequence at probe when INIT_DONE
is absent. An already configured bus is retained unless the read-only
boot/module parameter ``mt6580_pmic_wrap.force_init=1`` explicitly requests
a qualification run. The parameter is not a sysfs trigger and never resets
an already registered set of PMIC consumers. It has no effect on the legacy
firmware-only compatible. All required reset/clock suppliers are acquired
before any hardware reset; the new binding requires them explicitly.

On this LK, appending the parameter to the Android boot header command line
does not reach Linux. The qualification image sets it in a temporary copy of
the appended DTB's /chosen/bootargs; the source DT and ordinary image do not
contain the parameter. A boot reporting source=firmware with no parameter in
/proc/cmdline is not evidence that the initialization path executed.

TOPRGU uses the existing watchdog reset-controller implementation, with only
the verified MT6580 bit11 exposed; other bit numbers are rejected. The
TOPCKGEN supplier owns the four SET/CLR gates and serializes these writes
with other clock users. The pre-consumer reset interval saves their actual
enable states and restores them, forcing AP on as in the BSP, without
dropping CCF references. The PMIC SPI source is a real two-bit CCF mux and
is selected to the reference oscillator through clk_set_parent(), not an
untracked write into a firmware-snapshot alias. PMIC bclk DCM remains
disabled for BSP erratum WHQA_00014186; unrelated DCM bits are retained.

Only successful, completed 0x5aa5 reads qualify a sampling point. Bus errors
abort rather than changing timing under an in-flight command. Empty,
disjoint or out-of-range sampling windows fail explicitly. The center of a
single nonempty 24-point window is selected and rechecked. Both AP readiness
polls and the PMIC cipher-ready loop terminate; the latter permits at most
1000 checked transactions with 10 us delays, each transaction itself bounded.
This is a bounded attempt budget, not a promise of a 10 ms total wall time.

CRC, status updates and autonomous ADC addresses follow the MT6580/MT6350
layout. Probe masks AP wrapper interrupts before registering its handler,
including the retained-firmware path: hardware #151 revealed INT_EN was
actually 0x7ffffffd in that path despite earlier documentation saying it was
masked. SPI 108, level high, comes from the old MT6580 DT and request_irq
flags. The new handler enables the BSP mask only after installing recovery
infrastructure and exposing consumers. MT6350 peripheral IRQs through
EINT17 remain a separate existing driver.

INIT_DONE0/1/2 are published only at the final stage, followed by the normal
signature check. The initialization bypass of INIT_DONE is limited to private initializer
transactions while public clients are excluded, and is cleared on exit. Failure after reset quiesces
manual/wrapper requests, arbitration, status updates, timer/watchdog sources,
interrupts and INIT_DONE. Probe fails without exposing children. This is a
terminal safe failure state, not restoration of the previous two-ended
protocol; no failed PMIC command is blindly replayed. A gate-preparation
failure before reset restores gates and returns without changing the link.

For a forced run on an existing readable firmware link, VPROC control,
selectors and transition registers are compared before/after. Serial-link
initialization must not change these supply settings. No rail initialization
is added, and board bootstrap supply/pin setup still comes from earlier boot
stages. Qualification by resetting an LK-configured bus demonstrates link
reconstruction, not removal of all firmware dependencies in the entire SoC.

Online recovery
---------------

AP IRQ handling records INT_FLG, INT_RAW, WDT_FLG, SIG_ERRVAL and WACS2,
masks the source and acknowledges its flags. It never sends a serial
command or resets the bus in hard IRQ context. The same recovery worker is
queued by transport failures, failed DEW signatures and raw error flags.
Only bit0 is identified as watchdog by the reference; other raw bits are
retained as generic errors rather than assigning an unverified CRC layout.

A freezable worker holds the SPM phase mutex and a temporary wakeup source.
It does not rebuild before SYSTEM_RUNNING or across a live SPM voltage
bank/command phase. Pending faults reject new suspend preparation and
infra power-down; finish queues deferred work after restoring ownership.
The atomic bus lock waits out a complete public regmap operation, then
makes further requests return EAGAIN (or EIO for a latched failure). The
worker alone uses private raw accessors. No second lockless regmap is
exposed to PMIC children or debugfs. Final power-off remains atomic and
no mutex is added to ordinary PMIC access.

A delayed VALID is acknowledged and the fixed 0x5aa5 signature checked.
A healthy link with at most WDT flags can be resynchronized without reset.
Repeated incidents in a one-second burst escalate after four attempts.
An unhealthy link or a non-WDT fault reconstructs both protocol ends using
the native reset, calibration, DIO, cipher and CRC sequence. An automatic
full reconstruction is limited to once per minute; a repeated failure
latches EIO rather than looping resets. Failed original reads/writes
always retain their error and are never replayed.

Before reset, the worker saves arbitration, status/ADC routing, watchdog,
timer/DCM policy and all eight autonomous DVFS slots. It stops new status
requests and bounds shared-wrapper drain waits; a broken bus that cannot
drain is recorded but can still be reconstructed after software clients
and SPM are excluded. New automatic requests stay disabled throughout
initialization, until their saved configuration is restored and verified.
No sampled data or queued requests are replayed. A cached pre-reset
configuration survives a failed rebuild for an explicit later retry.

After bus exclusion, 300--500 us allows an already completed regulator
write to settle before the pre-reset VPROC snapshot. This exceeds the
BSP's maximum 127-step down-ramp budget of 259 us. When the bus is readable,
six control/selector/transition values must compare exactly before/after.
If the broken link prevents the before snapshot, that error is retained;
a readable post-reset snapshot and signature are still required. No
voltage rail is initialized or rewritten as part of link reconstruction.
The check does not measure analog voltage or prove physical power faults
can be repaired. A failed rebuild leaves AP IRQs masked and public PMIC
requests failed until an explicit successful recovery or reboot.

The root-only recover attribute permits an asynchronous inspection (1) or
full reconstruction (2). The latter is an administrator qualification
operation that bypasses the automatic one-minute limit; ordinary sysfs
reads never force a reconstruction. recovery_state describes the last IRQ,
recovery counters, failures and VPROC comparisons. Driver removal/shutdown
masks the source and drains its worker before releasing resources.

Validation
----------

A temporary host harness compiles the actual new driver poll/transaction
functions and this tree's atomic polling macros against scripted MMIO
with VALID latched until acknowledgement. Nine scenarios cover normal
read/write, a late prior result, read timeout followed by a late result,
write timeout without retry, idle timeout, and loss/absence of INIT_DONE.
The old transaction function from the parent commit reproduces the late
VALID timeout with the same model. Address/undefined-behavior sanitizers
pass. This is simulated fault coverage, not injected hardware failure.

Artifacts are in out/a1260-runtime-pm/pwrap-link. Runtime results are
recorded with the tested build in wt6580-we-n.rst and the device TODO.

The cold-initialization harness in out/a1260-runtime-pm/pwrap-cold compiles
the actual initialization, transaction and polling functions with a scripted
two-ended protocol model. It exhaustively checks all 16777216 24-bit masks
(300 valid continuous windows), complete initialization, eight stage faults,
empty/disjoint windows and AP/PMIC cipher-ready timeouts under ASAN/UBSAN.
The prior nine normal-transport scenarios are rerun because the probe-only
INIT_DONE bypass changed those functions. The model checks ordering and
bounded exits but cannot validate actual reset wiring or analog sampling.

Online hardware qualification, 2026-10-27
----------------------------------------

Build #156 registered Linux IRQ56 (CIRQ line76, DT SPI108) and retained a
healthy firmware link at normal startup. Two administrator-requested live
reconstructions at 1300 MHz/1.30 V and 604.5 MHz/1.15 V completed with
window 0x0000ff, sample 3 and signature 0x5aa5. All saved AP registers/slots
compared equal. Each readable six-register VPROC snapshot matched exactly;
watchdog/IRQ raw flags and transport errors remained zero. One soft
inspection then completed without reconstruction. All seven normal CPU
OPPs subsequently passed, with battery and RTC access still working.

Two RTC-triggered deep cycles stopped the system counter for 9.701235326
and 9.746584370 seconds. Both used CPU0/L2 cold recovery, infra/DDRPHY power
control, MAINPLL off/on and PMIC hardware sleep ownership; successful/failed
suspend counts were 2/0. VPROC, AUXADC, autonomous slots, CPU/infra DCM and
EMI DCM restored without late repair, and an 8 MiB RAM hash was unchanged.
PWRAP attempts stayed 3 (two full, one soft), with no AP IRQ or bus error.
ADB restoration required manual host reattachment; init logged an adbd
start lasting 45.741 seconds while device execution and health continued.
This USB issue is recorded separately, not counted as a PWRAP failure.

A third full live reconstruction ran with GPU rendering and four RTC
readers. Of 400 reads, 399 completed and one explicitly failed during bus
exclusion; recovery_state counted one blocked callback and no transport
error. Subsequent RTC reads succeeded. Mali rendered 1624 frames with green
pixel readback and GL_ERROR 0. A 4 MiB aligned direct write plus two reads
had identical hashes, eMMC remained 8-bit/52 MHz with error 0, and four CPU
workers produced coherent_count 400000 with 400 monotonic clock migrations.
No physical CRC fault, stuck wire or hardware error IRQ was injected:
these tests qualify actual online reconstruction and client exclusion,
not physical fault coverage. Artifacts are in
out/a1260-runtime-pm/pwrap-runtime.

With only CPU0 online and display/GPU quiesced, normal menu admission
completed 1218 actual dormant resets/restores, one early WFI return and
zero failures. A second 8 MiB RAM hash matched, and four-CPU coherence and
400 clock migrations passed after CPU1--3 returned. The script reached
COMPLETE, but its final USB/cleanup receipt was not captured; the subsequent
normal boot clears all transient locks, RAM files and tracing. Default ADB
is a Windows-owned server: USB/IP attachment to WSL hides the device from
that server, and detaching it restored default access. This observation is
separate from the earlier measured adbd startup delay.

The final build #157 additionally retains both masked IRQ status and raw
flags when recording an incident. Its normal boot again started as shell
with a retained firmware link, then repeated both high/low OPP live resets
and a soft inspection without errors. No fault parameter or periodic reset
is enabled in the ordinary image. Product boot.img/kernel now use #157;
root was used only on demand for qualification and default shell restored.
