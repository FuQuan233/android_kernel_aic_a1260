MT6580 CPU and bus dynamic clock management
==========================================

Scope and evidence
------------------

CONFIG_COMMON_CLK_MT6580_DCM opts into the CPU Mode 1 and infra/peripheral
bus policy, implemented by the existing clock-bank owners. DDR/EMI block
configuration, MCUSYS/L2 divider and gating, USB/PMIC DCM and memory PLL
policy are outside the six CPU/bus fields described here. EMI is managed
separately; see mt6580-memory-dcm.rst. The additional CG1/BIU bus policy
is described in mt6580-mcusys-dcm.rst; it does not enable SRAM CG2. Four online CPUs use WFI/DCM; the
separate CPU0-only dormant path is described in mt6580-cpuidle.rst. System
deep sleep remains a separate path.

The reference sources are android_kernel_mediatek_k80/drivers/misc/mediatek/
base/power/mt6580/mt_dcm.c and mt_dcm.h. The actual normal kernel backup was
also inspected with REA, using a code-only ELF slice with original addresses:

* dcm_armcore at 0xc0437254: default Mode 1 sets INFRACFG+0x14 bit0, clears
  WFI/WFE Mode 2 bits at +0x10, enables ARMPLL gating at +0xa00 bit2 and
  clears the CPU-side EMI WFI gating-disable bit at TOPCKGEN+0x20 bit30.
  Mode 1 does not call the CPU mux/divider override used by other modes.
* dcm_infra at 0xc0437624: debounce enabled/count7, slow select /32, with
  automatic slow/off bits10/11. Full-rate select is retained.
* dcm_peri at 0xc0437764: analogous debounce/count7 and /32 slow select,
  with automatic slow/off bits26/27. The reference off mask only clears
  bit27; this port owns and clears both bits during a pause.
* dcm_mcusys at 0xc04374e0: the compiled stress-mode dispatch has no action
  for the default value 1. The seven 28-byte descriptors at 0xc0e47d24 all
  have default=1; dcm_set_default loads that value into r0 before blx.
  Therefore the default ON label does not establish active MCUSYS gating.

The reference ELF is out/a1260-keys-rea/reference-vmlinux-corrected.elf,
SHA256 71fd8423c76ad1dad2af923988e8e5362ac9d314b6f436370b548c3200d784b6.
REA Evidence ev_f2a038a593c008a10b2dab855a86381d0bbf2f54900724aebf22cd0d5bec9818
is retained in out/a1260-runtime-pm/dcm/stock-core.json and stock-core.rea.
The small slice omits external data, so the descriptor table was read from
the complete original ELF into stock-defaults.json. Disassembly resolves
the indirect-call argument which decompiled pseudocode omits. Pseudocode
also has an extraout_r1 artifact around the outlined ARMPLL gate helper;
it is not treated as a literal C implementation.

Control ownership
-----------------

The new code performs checked masked updates on only these six groups:

=======================  ====================  ===========================
Bank/offset              Mask                  Enabled owned value
=======================  ====================  ===========================
INFRACFG 0x14            bit0                  1 (CPU Mode 1)
INFRACFG 0x10            bits2:1               0 (Mode 2 disabled)
INFRACFG 0xa00           bit2                  1 (ARMPLL automatic gating)
TOPCKGEN 0x20            bit30                 0 (CPU EMI WFI gate allowed)
TOPCKGEN 0x28            bits11:5,27:21        0x0c000c00
TOPCKGEN 0x2c            bits19:8              0x0003cf00
=======================  ====================  ===========================

The two full-rate selects must already be /1 and forced slow/off bits must
be absent. The CPU must use ARMPLL. Unknown prerequisites fail before policy
writes. The existing INFRACFG+0x10 bit0 bus-ratio flag is preserved, matching
the reference normal functions, not enabled from an assertion/comment.
PMIC bclk DCM erratum bits1/2, USB bit0, bus-PLL selection, unrelated muxes
and gates are preserved. All shared clock-register accesses use the existing
clock lock; no second independent mapping or lock is introduced. TOPCKGEN
holds the existing APMIXEDSYS/INFRACFG supplier with a device link.

CPUFreq pauses Mode 1 and automatic ARMPLL gating around its existing
MAINPLL handover and restores them after returning the CPU to ARMPLL.
The CPU/L2 dividers and OPP policy are otherwise unchanged.

Deep-sleep handoff
------------------

The IRQ-disabled single-CPU prepare step captures the six fields. The
coherency-off finisher disables the owned automatic slow/off and CPU controls
before parking AXI on 26 MHz or powering down MAINPLL. Paused ownership is
recorded before the first write, including a partial failure before the AXI
switch. Existing abort/retry cleanup therefore also handles dcm_paused even
when AXI was never parked.

After PLL readiness and mux restoration have been verified, the finisher
restores debounce/slow fields and CPU controls, enabling Mode 1 last. Only
owned bits are restored; unrelated live bits are never overwritten by stale
saved words. Readback failure preserves the recovery context and fails the
clock transaction. There is no CCF, spinlock, allocation or sleeping call in
this finisher helper. The existing resume-noirq callback checks the policy
again and can repair it for retained/qualification paths that did not use
the AXI transaction. Such repairs have separate counters and are not used
as evidence of early restoration.

Validation and remaining work
-----------------------------

The initial #132 hardware snapshot had CPU Mode 1 off, WFI/WFE controls0,
ARMPLL gating0, infra/peri automatic controls off, bus full selects /1 and
TOPCKGEN+0x2c=0x80000006. MCUSYS/L2 bus DCM was off and L2 divider0x12.

A host harness compiles the actual masked writer, snapshot/match helpers
and pause-value code. ASAN/UBSAN pass 10000 randomized live/unowned-bit
preservation and simulated register-reset/restore cases, plus six partial
write failures and rollback. This covers software paths, not physical fault
injection or analog clock behavior. Runtime results are recorded in
wt6580-we-n.rst and device TODO.md, with artifacts in
out/a1260-runtime-pm/dcm.

Control-bit ownership and successful workload/resume checks do not measure
electrical savings. CPU-off voltage and board standby current still need
instruments. The separate MCUSYS/BIU bus policy is now documented in
mt6580-mcusys-dcm.rst; SRAM gating and memory frequency scaling remain
outside these policies.
Separate EMI and CPU0-only runtime idle implementations are documented in
mt6580-memory-dcm.rst and mt6580-cpuidle.rst.
