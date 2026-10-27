MT6580 runtime idle qualification
================================

The MT6580 BSP's mt_idle_cpu_criteria() admits deep idle only when CPU0 is
alone online and no hotplug is in progress. Its PCM is distinct from system
suspend. No MT6580 multicore MCDI program has been established by this port.
The driver therefore retains four online CPUs and uses WFI/DCM in that case;
it does not introduce an automatic CPU hotplug policy.

Firmware identity
-----------------

The 688-word pcm_deepidle_v14.0_new array from spm_v1/mt_spm_dpidle.c matches
the normal device kernel backup at reference-Image file offset 10810324.
Its SHA256 is
5e3d1f304df968de175261fe8ec94e85091c708f1109148a94b52f0a936c17dc.
The reference Image SHA256 is
31ad56ed4db3cdc3cf20cb4dcbf0d976fc6e9bcf30893862a7e462c1d5f4803a.
The suspend firmware remains the separate 725-word image. Vectors 4--7 use
registers 0x3d0--0x3dc; they are not contiguous with vectors 0--3 at 0x340.

Transaction and scope
---------------------

CPU0-DORMANT is available only with CPU0 alone online. The driver disables
it before bringing any secondary CPU online. SPM runtime_cpu_powerdown
defaults to true; setting it false is a diagnostic WFI-only override.
With CPU power-down disabled the same transaction exercises PCM and IRQ
handoff using WFI; it is accounted as WFI, never as a CPU reset.

CPU0 identity, the online count and both hardware sibling power-status
banks are checked. Active DISP/MFG or an enabled pending interrupt refuses
entry. CIRQ must cover all enabled SPIs; unrelated IRQ configurations also
refuse entry. Hotplug preparation disables the state before starting a
secondary CPU. The backend checks the topology again independently.

CIRQ latches external edges before direct GIC masking. Only priority-zero
SPM may terminate WFI during power sequencing, as in the BSP. GIC enable
bits, priorities and PMR are restored and checked before flushing CIRQ.
The generic GIC CPU/cluster PM notifier otherwise resets priorities to its
default, so the runtime path preserves them separately.

The CPU context helper proposes L2 retention, with CPU PM notification
outside the RCU idle interval. The retained-L2 path follows the BSP's L1
clean/invalidate followed by L2 clean, with a DSB between levels for Cortex-A7
erratum 814220 and CLREX before clearing ACTLR.SMP. The reset entry and
finisher need data at PoC even when L2 SRAM stays powered. CPU0's resume
vector, banked registers, physical timer and L2 reset policy are restored and checked. This mode does not run
the sibling-core recovery reserved for the full L2-loss system-suspend mode.

The initial profile keeps R0 disconnected and APSRC/F26M requested. It does
not park AXI, stop PLLs, change PMIC slots/voltage, or suspend devices. The
running watchdog is neither stopped nor fed/reprogrammed. A 655-tick PCM
timeout bounds each idle interval to about 20 ms; the generic broadcast
clock event and CIRQ can wake earlier. TIMER_STOP requires successful generic
broadcast admission before the CPU context can be lost. CNT must include
the PCM residency since F26M is retained; no suspend time compensation runs.

Qualification status
--------------------

The WFI-abort path must capture ACTLR using volatile assembly before cache
shutdown. get_auxcr() uses non-volatile assembly; Clang sank its read into
the abort branch after ACTLR.SMP was cleared. The saved value then kept
SMP clear while re-enabling the data cache, and the first later atomic
operation could stall. The fix also covers the existing full system-sleep
abort path. It is separate from the architectural 814220 workaround.

On #148, normal cpuidle execution completed 528 actual CPU resets and 46
WFI aborts without a failed transaction. After deleting temporary persistent
markers and a redundant early GIC restore, #149 completed 92 resets under
the normal menu governor, then reached 438 resets and 28 aborts including
forced-state qualification. RAM hashes, timer deadlines and four-core
coherence after re-onlining passed. Final #150 completed 83 resets and five
aborts under the normal governor, with no userspace enabling of the state
or power-down parameter; four-core restoration disabled it automatically.
Zero entries are never counted as a
successful power-down test. Evidence is under out/a1260-runtime-pm/dormant.

The final profile keeps four CPUs online. There is no automatic hotplug,
multicore MCDI implementation, runtime infra/DDR/PLL shutdown, or measured
power saving claimed here. System deep sleep remains a separate path.

Final #150 also passed two full system-deep cycles (9.007455474 and
9.084390442 seconds of stopped-counter time), with total suspend statistics
moving from 1/0 to 3/0 success/failure, matching RAM hashes and no late DCM
repair. Subsequent Mali rendering/readback, aligned 4 MiB eMMC write and
two direct readbacks, four-core coherence and cross-CPU clock checks passed.
Temporary tracing, alarms and wake locks were removed; default ADB is shell.
