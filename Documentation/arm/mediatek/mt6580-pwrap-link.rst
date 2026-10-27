MT6580 PWRAP link checks and bounded recovery
===========================================

The legacy ``mediatek,mt6580-firmware-pwrap`` binding retains LK's MT6350
SPI setup. The new ``mediatek,mt6580-pwrap`` binding additionally supports
initializing an unconfigured serial link before exposing PMIC children.
Neither mode resets a live link as an automatic timeout recovery action.

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
in the transport. The regmap fast_io lock still serializes all WACS2 users,
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

Probe-only serial-link initialization
-------------------------------------

The old pwrap_init() gates TOPCKGEN clocks, asserts wrapper SWRST and
TOPRGU bit11, manually resets the SPI slave, scans 24 SI_CK/SIDLY points,
configures register clock/dummy cycles, enables DIO, establishes cipher
state, performs a write test, then enables CRC, arbitration, watchdog/
status updates and autonomous ADC before setting INIT_DONE0/1/2.

That sequence is not safe as an automatic timeout retry while regulator,
RTC and PMIC IRQ clients are running. It changes both ends of the bus,
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
layout. AP wrapper interrupts remain masked because no wrapper IRQ handler
is installed; copying the BSP's INT_EN mask would enable an unhandled source.
MT6350 peripheral IRQs through EINT17 remain a separate existing driver.

INIT_DONE0/1/2 are published only at the final stage, followed by the normal
signature check. The initialization bypass of INIT_DONE is limited to this
pre-consumer interval and is cleared on exit. Failure after reset quiesces
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
