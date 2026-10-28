# MT6580 integrated AHB Wi-Fi source

Imported from OpenWatchProject/android_kernel_mediatek_mt6580, branch android-8.1, commit `65340b8dd12c602802ebb3933aebfa4e8f9f8433`.
Original path: `drivers/misc/mediatek/connectivity/conn_soc/drv_wlan/mt_wifi/wlan`.
Repository: https://github.com/OpenWatchProject/android_kernel_mediatek_mt6580

Source files retain their MediaTek GPL-2.0 notices. Linux 6.18 port changes
are local and use the MT6580 AHB HIF, not the unrelated SDIO path.

The original A1260 Android 8.1 `wlan_drv.ko` SHA-256 is
`c2159cda06afad0fa5908bf13a92f50069bf222e9f7bc90503aa7311a0f68a12`.
REA/Ghidra checks of `glBusInit` and `glRegisterBus` agree with this source's
AHB platform registration and WMT callback structure. This is evidence of the
same driver family; it is not a claim that the original binary was built from
this exact Git revision.

The initial port uses the original PIO HIF path; PDMA is disabled. Firmware is
requested as `WIFI_RAM_CODE_6580`. Calibration is requested separately as
`mt6580-wifi-nvram.bin`, exactly 512 bytes from this device's factory WIFI data.
A zero factory MAC does not invalidate calibration: firmware/NIC-provided MAC
has priority, followed by a logged locally administered random address only
when neither factory nor NIC provides a usable address. Calibration is never
written back by this driver.

Only the station cfg80211 interface is advertised initially. The old optional
AP, P2P, GSCAN vendor commands, scheduled scan and TDLS are not advertised.
Changing the station MAC is explicitly unsupported; requesting the
already active address is allowed. Firmware download occurs on WMT Wi-Fi
function-on, after userspace is available, rather than in the built-in initcall.

Linux 6.18 compilation and a relocatable whole-driver link passed. Radio, firmware, scan and WPA2 association have since passed on A1260 #191;
selective wake, complete connected power collapse and sustained load require
additional qualification. Retained ANY-mode suspend results are recorded in
Documentation/arm/mediatek/mt6580-wifi.rst.
The old AHB platform suspend/resume callbacks alone do not establish firmware
ownership or DDR retention; CONSYS power management must retain the domain
until those transitions are qualified. ISR/transaction wakeup sources have
been converted to the current kernel API; they are not substitutes for that
power-domain requirement.

The cfg80211 PM bridge now exposes only the standard WoWLAN ANY trigger:
normal firmware operation is retained and its AHB HIF IRQ is a wake source.
It enables IRQ wake in suspend and balances it in resume (including PM abort
unwind) or IRQ teardown. It does not implement pattern/magic/GTK/ARP offload,
nor claim CONN or DDR can be powered down. The existing genuine firmware
sleep notification and ownership/readback protocol remains in use; no
ownership flag is forced. The legacy system-suspend helpers, including their
unsafe IPv6 address cast, are not called by the new bridge.

Android requests ANY through its supplicant overlay, so cfg80211 need not
disconnect a live STA merely because no WoWLAN configuration exists. The
read-only netdev `pm_state` reports IRQ arm/resume counts, interrupts observed
while armed, cached suspend-time WHLPCR/ownership and current cached ownership.
Its diagnostic reads do not perform MMIO or wake the firmware. Counts of
callbacks/IRQs alone are not proof of a completed system suspend or its wake
cause; correlate the system suspend statistics and SPM/wakeup evidence.
