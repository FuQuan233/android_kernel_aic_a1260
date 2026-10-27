MT6580 PWRAP link checks and bounded recovery
===========================================

The MT6580 firmware wrapper still retains LK's MT6350 SPI setup. The
``mediatek,mt6580-firmware-pwrap`` binding deliberately does not promise
cold initialization. The changes here cover normal link qualification and
recovery of a late WACS2 read result, without resetting a live PMIC link.

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

Cold initialization remains separate
------------------------------------

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

Independent cold initialization, bounded calibration/cipher errors and
complete failure handling still need a separate implementation and boot
qualification. No live reset, forced CRC error or SI scan was performed
to claim those capabilities in this change.

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
