# MT6580 connectivity transport

The GPL WMT/STP core comes from OpenWatchProject/android_kernel_mediatek_mt6580,
branch `android-8.1`, commit `65340b8dd12c602802ebb3933aebfa4e8f9f8433`,
`drivers/misc/mediatek/connectivity/conn_soc/common/{core,include,linux}`.
Original MediaTek GPL notices remain in the files.

`platform.c` and `btif.c` implement Linux 6.18 resource ownership, MT6350
regulator consumers, CONN runtime PM, shared TOPRGU MCU reset and BTIF PIO.
The BTIF register/FIFO protocol is also checked against the owner's MT6580
3.18 BSP. No legacy kernel module or proprietary firmware is embedded here.

REA checks of the A1260 Android 8.1 `wmt_drv.ko` and `wmt_launcher` establish
platform differences from the public source: CONSYS AHB clock hooks do not
program a gate, this board's CONNSRC clock buffer is hardware controlled,
and initialization holds VCN28 while reading RF efuse voltage selection.
The selected VCN33 voltage is 3.3/3.4/3.5/3.6 V for efuse levels 0/1/2/3.
These observations establish the protocol/board corrections, rather than
claiming that the public source is the exact source of the shipped module.

Userspace uses the source BTIF-only `mt6580-wmt-dispatcher` in the A1260
device tree. It configures the real HIF, validates the two owner ROM patch
headers, answers kernel `srh_patch` requests and acquires no loopback power
vote. Firmware and this unit's factory RF calibration are packaged privately
from owner-supplied files by the device tree's `prepare_stock_wifi.py`.

The initial radio path is station-only. BT, GPS, FM, P2P and AP are not
implemented/exposed by this port. BTIF PIO sleep retains its AP bus clock;
SPM retains the CONN domain and rejects DDR/infra/MAINPLL removal while the
connectivity MCU is powered. Complete Wi-Fi deep-sleep clock removal is a
separate qualification step, not implied by successful scan or association.

The native `dpidle` callback currently changes `SLEEP_EN` while retaining the
AP BTIF clock. This is a conservative transport state, not the BSP's BTIF
clock-gating implementation: that BSP drains TX before gating/restoring BTIF
and optional APDMA clocks, and its `btif_sleep_ctrl` helper is compiled out.
Firmware STP PSM remains enabled and requires actual sleep/wake events. The
AP wake signal follows the BSP's explicit WAKE bit 0 low interval of
64–96 microseconds followed by a rising edge; a successful register write
does not itself acknowledge firmware wake. This distinction limits the
claimed BSP match to the audited register/FIFO and wake-pulse protocols.
