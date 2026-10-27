MT6580 EMI and inherited DRAMC dynamic clock management
======================================================

Reference and initial hardware state
------------------------------------

The MT6580 BSP mt_dcm.c implements EMI DCM separately from CPU/infra/peri
DCM. Its DRAMC programming example is under ``#if 0`` with the comment
"will set in preloader". The port keeps that ownership boundary.

Normal-device binary evidence is retained under
out/a1260-runtime-pm/memory-dcm/stock-emi.json and stock-emi.rea, Evidence
ev_5fc67063a7dc1c826d50cdd2a7696f8ffd3e9fe0994f4fae3534ad3a11a37d78.
The existing code-only stock-dcm.elf slice has SHA256
36f7260b92216159f0e63d629a6b2c55c1e83c09c1f704bfd012972c65df871f.
Its full source ELF SHA256 is
71fd8423c76ad1dad2af923988e8e5362ac9d314b6f436370b548c3200d784b6.
The slice retains original addresses but omits external data; mapped bank
addresses come from the BSP/DT, corroborated by device reads.

* dcm_emi at 0xc043790c sets debounce enabled/count15, EMI_CONH ratio3,
  then enables bits27/7. Debounce and enable phases each latch with bit0
  low then high, with ordered MMIO writes.
* dcm_emi_dbc at 0xc043788c uses command selector bit3.
* dcm_emi_slow_down_ratio at 0xc0437854 updates EMI_CONH bits1:0 only.

Before this change, #150 reported TOPCKGEN+0x0c=0 and EMI+0x38=0. DRAMC AO
PD_CTRL at 0x102071dc was 0xdb643842: dynamic COMB/PHY/non-freerun policy
bits31/30/25 were already set. DDR PLL-derived memory source was
266499673 Hz. This is the calculated source rate, not an instantaneous
measurement of the automatically divided/gated output.

Ownership and sleep transitions
--------------------------------

CONFIG_COMMON_CLK_MT6580_EMI_DCM opts in. TOPCKGEN owns its existing bank
and a named ``emi`` resource at 0x10205000. No duplicate mapping or lock is
introduced for TOPCKGEN. The shared clock lock serializes normal updates;
the single-CPU IRQ-disabled coherency-off sleep finisher uses the same
bounded masked writer without CCF, spin locks, allocation or sleeping calls.

Persistent policy fields are TOPCKGEN+0x0c bits27/7 and bits15:8, plus
EMI_CONH bits1:0. Selectors2/3 and toggle0 are the BSP command interface.
The on policy is debounce15/enabled, automatic DCM enabled and idle ratio3
(/32), yielding readback 0x08001f8d/0x00000003 on this board. CPU clocks,
full/idle frequency select fields, force-off, and unrelated EMI_CONH bits
are preserved. A forced-off state or foreign force-on/frequency command
selector refuses admission; readback mismatch fails the transaction.
Probe rollback/release restore the original functional policy, not an old
sampled latch edge. DDR PLL, DRAM timings and live DRAMC policy are not written.

Deep prepare checks the owned on policy. Before AXI parking or MAINPLL
power-down, the finisher marks partial EMI ownership, disables automatic
EMI gating/debounce and selects ratio0, then pauses CPU/bus DCM. Once PCM,
PLL and AXI clocks have recovered, CPU/bus DCM and then EMI policy are
restored before sibling/L2 and normal device recovery. Abort cleanup also
handles an EMI pause before AXI was parked. Unresolved EMI restoration stops
before normal device resume. A separate resume-noirq check reports late
repairs rather than counting them as early recovery.

CPU0-only runtime dormant leaves AXI/MAINPLL intact and keeps EMI DCM on.
DRAMC exposes its inherited policy and checks the three policy bits after
system resume; it does not attempt an unqualified firmware replacement.

Hardware validation
--------------------

#151 boots with EMI owned/valid, applies1/resume_fixes0, the expected
0x08001f8d/3 policy, and unchanged DRAMC 0xdb643842. Four workers verified
64 MiB across eight pattern rounds (1 GiB total read/write traffic). Two
system-deep cycles stopped the physical counter for 9.395571416 and
9.788576941 seconds; EMI pause/restore reached2/2, policy readbacks matched,
late repairs stayed0, and both DRAMC checks passed with no changed bits.
The 8 MiB retained RAM hash matched. A second memory-pattern run concurrent
with Mali rendering passed, followed by 4 MiB aligned direct eMMC write and
two matching direct readbacks, four-core coherence and clock-migration checks.
With EMI DCM still enabled, the normal governor then completed 1237 actual
CPU0 dormant resets without a failed transaction; retained RAM and four-core
coherence checks passed. Final cleanup is recorded in the artifact summary. These checks do not measure current or electrical savings,
and do not qualify memory frequency/voltage scaling or MCUSYS/L2 DCM.

During final cleanup, Android also completed an automatic system-deep sleep
with 55.747988563 seconds of stopped-counter time. Total suspend statistics
were3/0, with EMI early pause/restore3/3, DRAMC checks3/failures0 and still
no late policy repair. Default four-core schedutil/604500..1300000 kHz,
deep sleep selection and ordinary ADB shell were restored; test wake locks,
RTC alarm, trace instance and scripts were removed.
