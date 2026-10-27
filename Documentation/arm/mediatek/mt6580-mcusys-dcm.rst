MT6580 MCUSYS and BIU bus DCM
===========================

Evidence and scope
------------------

The reference is android_kernel_mediatek_k80/drivers/misc/mediatek/base/
power/mt6580/mt_dcm.c, approximately lines424--600, plus its MT6580 DT.
CG1 is MCUCFG MISC_CONFIG+0x5c bit9; CG3 is BIU CON+0 bit12 at 0x1020d000.
Both are described as automatic bus DCM enables. The implemented /2 branch
sets these bits after selecting MCUCFG+0x60[4:0]=0x12. This port requires
that divider already be 0x12 and never writes it, nor CPU/L2 SRAM policy.

The BSP defines _DCM_ACLK_STRESS_. As the prior stock-binary inspection
established, default argument1 consequently does nothing in dcm_mcusys().
This is a deliberate new enable policy using its explicit /2 branch, not a
claim that the original normal boot had these gates enabled. Both enable
and disable code comment out CG2 SRAM DCM at MCUCFG+0 bit8; it is excluded.
On #157, both bus enable bits were 0, divider was 0x12 and MCUCFG+0 was 0x60.
High bits in MISC_CONFIG change during normal operation; they are not used
as constant readback expectations or restored from a stale full word.

Ownership
---------

CONFIG_COMMON_CLK_MT6580_MCUSYS_DCM opts into the policy. The existing
APMIXEDSYS CPU-clock owner holds two additional named resources: MCUCFG
0x1020005c size 8 and BIU 0x1020d000 size 4. These exclude CPU PM's MCUCFG+0
reset/retention register. Only bit9 and bit12 are changed, with masked
read-modify-write and readback checks under the shared clock spinlock.
All divider and unrelated live fields are retained. Initialization rejects
an unknown L2 divider before writing, and partial setup attempts restore
the original owned bits. Resource release likewise returns those bits.

CPUFreq pauses the two gates under that same lock before the existing
MAINPLL handover. It restores them after ARMPLL returns and before CPU
Mode1 is restored. Separate counters record pause, restoration and errors.
An admission/pause failure returns an error before changing the CPU rate.
After a committed rate change, a failed gate restoration instead verifies
both optional gates disabled, latches the DCM policy degraded/unowned and
reports the real committed clock rate as successful. OPP accounting must
not remain at the previous frequency while hardware already runs the new
one. If a full-clock safe state cannot be verified either, the callback
stops before a later regulator request. Runtime dormant is blocked after
degradation. These fault branches have not been physically injected.

System deep preparation snapshots both controls and the divider while
CPU0 alone has IRQs disabled. The coherency-off finisher marks ownership
before its first pause write, including partial failures before AXI moves.
It disables MCUSYS DCM before the existing EMI/CPU/bus pause and AXI/MAINPLL
transaction. After PLL and AXI recovery are checked, it restores the two
bits before CPU Mode1, L2 recovery, secondary CPU bring-up and device PM.
The early helper uses no CCF, spinlock, allocation or sleeping call. An
unrestored MCUSYS phase stops before ordinary device/CPUFreq resume. The
APMIXEDSYS resume-noirq check remains a separately counted fallback; a zero
fallback count is needed to attribute restoration to the early helper.

CPU0 runtime dormant retains L2 and bus/PLL supplies, but actual #160
qualification showed BIU bit12 resets during CPU0 power-down. Two system
deep cycles had restored both bits correctly; the subsequent runtime test
caught the missing BIU bit, rejected later idle/frequency admission and
left the device running. That failed qualification is retained separately,
not counted as runtime success.

The runtime path now snapshots and pauses both controls before CPU PM.
After PCM releases CPU power, its wait callback restores and verifies the
controls before architecture/GIC clients leave CPU PM. An idempotent outer
restore covers notifier or early-WFI aborts, including partial pause writes.
Admission and post-return checks remain; they do not substitute a late
repair for this early callback. Normal four-CPU WFI remains unchanged.

Diagnostics and limits
----------------------

APMIXEDSYS mcusys_state reports ownership, current policy, raw before/current
words, DVFS counters and late resume repairs. SPM state records early
MCUSYS pause/restore counters and snapshots. Runtime idle_state includes a
clocks admission-block counter. Only owned bits and the /2 divider determine
validity. Readback and workload/resume tests do not measure instantaneous
clock gating, external current, or rail voltage. SRAM CG2, MCUSYS divider
changes, automatic CPU hotplug/MCDI and DDR frequency scaling are excluded.

Hardware qualification, 2026-10-27
----------------------------------

The final ordinary image is #161. Seven CPU OPPs (604.5--1300 MHz), each
with four-core calculations, coherent_count 400000 and 400 monotonic clock
migrations, passed. Two 64 MiB/eight-round memory checks each moved 1 GiB,
one concurrent with Mali rendering 1539 frames and a correct green pixel
(GL_ERROR 0). Aligned 4 MiB direct eMMC write plus two reads matched hashes;
eMMC remained 8-bit/52 MHz with error 0. Both gates were owned/valid, divider
remained 0x12 and no frequency pause/restore error or degradation occurred.

Two explicit RTC deep cycles stopped CNT for 8.345361923 and 9.017725205
seconds, with successful/failed suspend counts 2/0. CPU0/L2 cold, infra/DDRPHY,
MAINPLL and PMIC hardware sleep ownership returned correctly. MCUSYS early
pause/restore was 2/2, applies 1/resume_fixes 0, with matching owned fields and
an unchanged 8 MiB RAM hash. No late resume repair was needed.

Runtime menu admission then completed 911 actual CPU0 reset/restores and
one early WFI return, with zero failures. The new MCUSYS runtime phase
reported 912 pauses/restores, active 1/paused 0, and both bit9/bit12 still set.
A separate 8 MiB RAM hash and subsequent four-core/clock checks passed.
Afterward a further 1 GiB memory check, Mali 1587 frames with green readback,
and aligned direct eMMC write/two reads passed. PWRAP retained 5aa5 with
zero transport or IRQ errors. The normal governor had reached 9015 matched
DVFS pause/restores with failures 0/degraded 0 at the final root snapshot.
Display output was active 480x800 with RDMA underflows 0/abnormal 0.

The #159 script rejection before deep and #160 discovery that runtime CPU0
reset clears BIU are retained as failed qualification evidence. They are
not included in the final successes. No physical clock-register failure
was injected. The ordinary product boot/kernel use #161; default shell,
four cores, schedutil 604500..1300000, default deep and stay-on 0 are retained.
Test locks, alarm, tracing, scripts and temporary data are cleaned up.
Artifacts: out/a1260-runtime-pm/mcusys-dcm/summary.json and its raw logs.
