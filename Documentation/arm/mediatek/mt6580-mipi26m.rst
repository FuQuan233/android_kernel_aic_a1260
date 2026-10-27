.. SPDX-License-Identifier: GPL-2.0-only

MT6580 MIPI reference clocks and board power evidence
====================================================

The missing reference-clock dependency
--------------------------------------

The DSI PHY previously consumed the fixed dummy26m oscillator directly.
The board BSP actually calls set_mipi26m when powering the DSI path:
TOPCKGEN CG_CTRL0 bit 4 opens before APMIXED AP_PLL_CON0 bit 5, and they
close in reverse order after the PHY powers off. Both are active-high
enables. TOP uses SET at +0x50 and CLR at +0x80, status +0x20; AP uses
masked RMW at +0x00. Adjacent TOP bits often have the opposite polarity.

The working device's old #126 display-on/off registers established the
missing control: TOP +0x20 changed bfcffdfe -> bfcfffff but bit 4 stayed
one; AP +0x00 stayed 000001b3, including bit 5. DSI itself reported
refs=2/enabled=1 -> refs=0/enabled=0. This is hardware evidence that these
reference routes stayed on after the Linux display had stopped, not a
measurement of their current consumption.

The normal kernel backup was independently inspected with REA/Ghidra:
set_mipi26m is at c042e39c. Evidence
 ev_4c698a8d2f7c12a92e82235a0f4485e23f7a58b93f07512af73a6103beee4b37
records the enable branch calling the clock enable before setting AP bit 5,
and disable clearing AP bit 5 before calling clock disable. Its clock
object at c0e46728 names MT_CG_MIPI_26M_DBG_EN and has mask 0x10. Object
layout and SET/CLR polarity are corroborated by the reference mt_clkmgr.h
and mt_clkmgr.c, including general_en_cg_clk_ops, not by another SoC's map.
The source ELF SHA256 is
71fd8423c76ad1dad2af923988e8e5362ac9d314b6f436370b548c3200d784b6.

CCF ownership and ordering
-------------------------

The board PHY now selects APMIXED_MIPI26M instead of dummy26m. Existing
clock IDs remain unchanged; TOP_MIPI26M=19 and APMIXED_MIPI26M=5 are added.
The CCF chain is::

  dummy26m -> mt6580_mipi26m_top -> mt6580_mipi26m -> mt6580_mipi_tx

Both new clocks use prepare/unprepare, with the existing bank IRQ spinlock.
The MIPI PLL powers up in its prepare callback: ordinary gate enable would
open the reference too late and close it before PLL unprepare. Parent
prepare opens TOP then AP before PLL setup; child/parent unprepare powers
the PLL down first, then closes AP then TOP. Enable verifies the written
bit, and an unexpected disable readback reports an error. AP RMW modifies
only bit 5, retaining other controls.

CLK_IGNORE_UNUSED preserves LK scanout until a real consumer takes over;
it does not stop normal last-consumer unprepare from closing the gates.
APMIXED registers before TOPCKGEN and its MIPI leaf can initially be an
orphan. The MT6580 PHY defers on a zero reference rate, and gate prepare
rejects an unavailable parent. No AP-to-TOP DT supplier link is introduced,
since TOP already requires the AP PLL roots; CCF resolves the parent name
when TOP registers.

MT6580 also skips the common PHY's three-word calibration-data read. That
layout feeds another backend's rt_code fields and is not consumed by this
MT6580 backend. The BSP's separate four-bit impedance calibration is inside
a disabled block; this change neither enables it nor substitutes a newer
SoC's bit layout. Other platforms keep their existing calibration behavior.

Hardware validation, build #127
-------------------------------

Both changed objects, the full ARM kernel and the DRM DTB built. The image
booted Android with ADB uid 2000, four CPUs and default deep. Three display
off/on cycles showed both new clock counts 1 -> 0 -> 1 and actual gate
readbacks Y -> N -> Y. Configured rates remained 26 MHz reference and
300 MHz DSI; frame counts advanced with zero RDMA underflows/abnormal events.
The user confirmed the physical picture and touch were normal.

Physical unplug followed by normal Android deep produced
42.626483335 seconds of stopped clock, CPU0/L2 cold entry 1, MAINPLL off/on
1/1 and SPM last_error=0. Replug restored both reference gates, Android,
USB and display; the user confirmed automatic screen-on and normal touch.
One first freezer entry was canceled by the pending bq24190-charger wake
source; the automatic retry succeeded. There was one success/one freezer
abort, with device suspend/resume failures zero.

The non-wakeup logger's screen_off snapshot was frozen until resume and
therefore contains post-wake Y states; it is not a sample taken during
CPU-off. The direct OFF-state proof is the three separate display cycles.
The older PLL clock has no direct hardware is_enabled callback, so its
clk_summary Y fallback is not used as physical PLL power evidence. The new
reference gates do read their MMIO status bits. No standby-current or power
saving value is claimed from these tests.

Evidence is under out/a1260-runtime-pm/mipi26m: target-identity.json,
stock-core.rea, stock-cg-object.json, old-hardware.txt,
old-screen-switch.txt, first-boot.txt, screen-cycle-*.txt, deep.log,
after-deep.txt and summary.json. The logger finished and was removed; it
set no RTC alarm or wake lock. Product boot.img/kernel were updated to #127.

Panel and touch supply mapping remains separate
----------------------------------------------

The selected FocalTech source requests vtouch at 2.8 V, and the old board DT
maps it to VGP1; the current vcc-supply already uses that regulator. There
is no separate IOVCC consumer in the old driver. A generic driver's dummy
IOVCC fallback is not proof of an omitted independent switchable board rail.
VIO18/VIO28 consumer wiring remains unconfirmed.

The owner's firmware directory is Firmware_8.1, without the prose suffix
"dump" in its name. Its lk.bin was compared to a fresh, read-only dump of
the current device's mmcblk0p6. The current partition is 393216 bytes with
SHA256 564b3ce36becfe1ead9d5da7fce7fa34cca1713056770a5e2682b545d94ab686.
The 512-byte header and 0x3de54 payload size agree; seven bytes differ in
two probe-code patches and disable_uart=1 -> 0. The relevant LCM code and
driver table remain byte-identical.

The LK's ARM startup explicitly relocates to 81e00000 and LCM entries are
Thumb pointers. The current JD9161 table at 81e3b554 has null init_power,
suspend_power and resume_power. Its init/suspend/resume use reset, delay
and DSI tables. The current init wrapper at 81e13558 only conditionally
calls driver+0x18 then driver+0x0c, and performs DSI status/handle updates;
no additional GPIO or LDO switch appears in that checked wrapper.

Current-LK Evidence IDs are
 ev_e5ac85323ed823b13d4f69fc035c15e401e92c5b3fb2adae3f30646ec4ea1d6c
and
 ev_b1b2b722ae504cbf05ca05b1be552290d7f016159b44ab0f0872264e20acd674.
Identity, matching ranges, explicit search boundaries and closed REA sessions
are indexed in out/a1260-runtime-pm/panel-lk/findings-index.json.
This does not exclude preloader or common PMIC initialization; neither
establishes which physical panel pins connect to VIO18, VIO28 or another
rail. The panel placeholder and shared always-on rails remain until that
wiring is established.

The DSI engine/digital gate physical parents also remain unknown: the BSP
marks their source invalid. D14 has documented NFI/MFG/SMI/SCAM candidates
and a D28 child, plus an explicit unused-cleanup exemption; no checked
source proves it is the DSI parent. Its existing firmware hold is retained.
