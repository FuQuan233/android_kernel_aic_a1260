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
AP, P2P, GSCAN vendor commands, scheduled scan, TDLS, and WoWLAN controls are not
advertised. Changing the station MAC is explicitly unsupported; requesting the
already active address is allowed. Firmware download occurs on WMT Wi-Fi
function-on, after userspace is available, rather than in the built-in initcall.

Linux 6.18 compilation and a relocatable whole-driver link passed. Radio, firmware, scan and WPA2 association have since passed on A1260 #191;
suspend behavior and sustained load require additional qualification.
The old AHB platform suspend/resume callbacks alone do not establish firmware
ownership or DDR retention; CONSYS power management must retain the domain
until those transitions are qualified. ISR/transaction wakeup sources have
been converted to the current kernel API; they are not substitutes for that
power-domain requirement.
