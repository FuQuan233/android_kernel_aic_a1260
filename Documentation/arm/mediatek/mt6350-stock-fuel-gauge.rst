.. SPDX-License-Identifier: GPL-2.0-only

A1260 factory MT6350 software fuel gauge
======================================

Cell label and independent charge limits
---------------------------------------

The owner read the installed cell label: 1850 mAh, nominal 3.7 V, 6.845 Wh.
The board now exposes 1850000 uAh design charge and 6845000 uWh design
energy through simple-battery. Nominal voltage is not a charge termination
setting: the existing 4.2 V limit and 500 mA USB input policy are retained.

Factory evidence
----------------

The input is the previously captured working device boot image, not the
locally ported BSP used as an early reference. Its boot SHA256 is
665623dfff88a86b71cae34618784015920bcdcc59cdd150721c08c05114830b.
The decompressed Image SHA256 is
31ad56ed4db3cdc3cf20cb4dcbf0d976fc6e9bcf30893862a7e462c1d5f4803a.
The kallsyms-reconstructed ELF used for address mapping has SHA256
71fd8423c76ad1dad2af923988e8e5362ac9d314b6f436370b548c3200d784b6.
These files remain in out/wt6580-reference, out/wt6580-display-rea and
out/a1260-keys-rea respectively.

REA/Ghidra inspected derived code-only ELF slices, retaining original virtual
addresses and symbols. External calls and data remain unresolved in such a
slice; in particular, indirect ADC calls have output-pointer side effects
which the decompiler does not reconstruct. The apparent constant 15 mV in
its oam_run pseudocode is not a voltage measurement. ADC outputs are supplied
by the real PMIC driver in the port. Assembly, full-image data reads and the
reference GPL BSP corroborate the equations and table layouts.

The core REA Evidence ID is
ev_e333b016adfe9c2e4859c033688a319f9d9da6a82341b4d399786be9303e1e51.
Complete JSON, additional UI/initialization evidence, native pseudocode,
assembly, source and slice digests are retained under
out/a1260-runtime-pm/battery-stock-1850. Native sessions were closed after
saving stock-core.rea and stock-extra.rea. Existing local Ghidra/JDK tools
were used through a separate public REA stdio process because the configured
MCP server did not inherit their paths; client configuration was not changed.

Recovered parameters
--------------------

The factory has four 102-point OCV and resistance tables, at -10/0/25/50 C.
OCV addresses are c0e73dbc/c0e740ec/c0e7441c/c0e7474c; resistance addresses
are c0e74a7c/c0e74dac/c0e750dc/c0e7540c. Each corresponding voltage matches,
so the generated header combines DOD, voltage and resistance without losing
a separate grid. tools/wt6580/extract_stock_battery.py checks the exact ELF
digest and extracts the file-backed arrays through ELF LOAD mappings.

Factory initialization at c067254c sets effective Qmax to
1600/1798/1838/1815 mAh at those temperatures. This characterized capacity
is distinct from the nominal 1850 mAh design value. No guessed scaling of
cold-temperature curves is introduced. The old 1397 mAh reference curves
and their three generic DT OCV tables have been removed from this board.
The 17-point 10 kohm NTC table matches the existing conversion. The
16.9 kohm pull-up, 27 kohm parallel resistor and 8 mV HW OCV trim remain.

The raw 102-point tables are used. table_init at c0673dfc builds temperature
profiles; battery_meter_initial calls it then oam_init. A bounded ARM BL
scan of the first 0x900000 image bytes found no direct call to the separate
2-percent profile rewrite helper at c06735fc; the checked initialization
and DT paths do not call it. That unused helper is not enabled in this port.

Ported calculation and persistence
---------------------------------

* The two OAM observers, five impedance-tracking iterations and 10 milliohm
  series resistance are restored. D2 supplies the raw SOC, not D5. Positive
  current means discharge, negative means charge; both are estimates.
* Profile construction first interpolates paired table entries in temperature,
  then looks up voltage or resistance. A 30-sample temperature mean triggers
  reconstruction using the current integer temperature, as in the factory.
  Temperature Qmax is selected at initialization.
* Active elapsed seconds integrate estimated 0.1 mA into 0.1 mAh, preserving
  per-step truncation. Sleep separately integrates the factory's fixed 4 mA
  estimate. A sleep longer than 1800 seconds, offline, with lower new HW OCV
  resets the observer seed/car as in battery_meter_resume at c0673324.
* Startup pauses charging for 50 ms, compares HW OCV and averaged VBAT using
  the recovered tolerances, and conditionally accepts stored RTC SOC.
  Charger mode restoration is retried after bus failures. LK warm-reset
  boot_reason 3..6 and recovery tag 2 override voltage consistency when those
  tags exist; the current bootargs do not supply them, so that override is
  implemented but not exercised by this firmware path.
* RTC_AL_HOU[14:8] retains UI SOC, with zero invalid and saved SOC at least 1.
  MT6350 exposes it through a one-byte NVMEM cell. Masked write, commit and
  readback share the RTC mutex with alarm programming, preserving hour bits.
  A successful nvmem_cell_write returns one byte, not zero.
* The UI tracks full charge by one point per battery tick and follows a lower
  raw SOC through the factory six-tick counter; offline rebound cannot raise
  accepted SOC. Low-voltage UI tracking uses 3450/3400 mV boundaries.

The legacy charging service is not copied: BQ24296's real Full status remains
charge-completion authority, and an unconfirmed charging UI is capped at 99
at the end of synchronization. Legacy code could infer BATFULL from UI=100.
This explicit integration policy avoids reproducing that feedback loop.
UI ticks use BOOTTIME with a ten-second minimum; failed suspend/resume cannot
advance them rapidly, and long sleep causes one update rather than replaying
all missed ticks. No new periodic wake alarm or suspend-prevention lock is used.

Safety and validation boundaries
-------------------------------

Duplicate voltage/DOD intervals return their first point instead of dividing
by zero. Intermediates and coulomb accumulators are 64-bit to avoid legacy
32-bit signed overflow. Other integer units and per-step rounding remain.
These are declared robustness changes, not a byte-for-byte binary clone.

Independent host comparisons covered 9742 observer states and 154224 lookup
values, temperatures -20..60 C, positive/negative current, duplicate points,
fixed initialization Qmax, 30-sample history and sleep boundary cases through
26495 seconds. ASAN/UBSAN passed. Evidence and the independent reference are
in battery-stock-1850/model-test. This validates arithmetic, not cell accuracy.

The sleep current is still an estimate; transplanting factory logic does not
measure present-day cell capacity or aging. The checked SW path has no active
caller for the standalone aging-update helper; hardware-FG aging is not
claimed. Actual runtime boot/RTC/charge/suspend results are recorded in the
board bring-up log and device TODO when available.
