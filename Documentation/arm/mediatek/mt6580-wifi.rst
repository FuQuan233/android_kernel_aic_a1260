MT6580 AHB Wi-Fi and BTIF connectivity
====================================

The A1260 uses the integrated MT6580 AHB WLAN interface, not SDIO. The GPL
Gen2 driver and WMT/STP core are ported from OpenWatchProject's MT6580
android-8.1 tree, commit 65340b8dd12c602802ebb3933aebfa4e8f9f8433.
Source provenance and shipped-artifact comparison boundaries are recorded in
the SOURCE.md files beside both drivers. No original firmware or factory
calibration is distributed in the kernel repository.

Resource ownership
------------------

SCPSYS owns the CONN power domain and the shared TOPRGU consumer owns MCU
reset bit12. AXI bus protection is established before reset/power-off.
The native platform uses MT6350 VCN18, VCN28 and separate BT/WLAN VCN33
consumers. Real firmware EFUSE selects 3.3/3.4/3.5/3.6V on the shared VCN33
selector, with readback and failure rollback. VCN28 is enabled during the
original initialization EFUSE window. The board's RF clock buffer remains
hardware controlled; another chip's CONNMCU_BUS gate is not substituted.

Two independent EMI MPU windows are mandatory even when PDMA is disabled:

* region5: reserved_base .. reserved_base+0x7ffff, permission 0xa2d;
  CONN may access WLAN firmware memory, AP/MD/MM may not.
* region6: reserved_base+0x80000 .. reserved_base+0xfffff, permission 0xa28;
  AP and CONN may access the shared WMT patch/debug window, MD/MM may not.

The MT6580 permission fields are D3/MM, D2/CONN, D1/MD, D0/AP. Each write
preserves its neighboring region4/7 permission field. Setup runs with MCU
reset held and checks the actual programmed bounds/permissions. It does not
clear violation evidence. These are the original two windows, not permission
to access all RAM. Reserved-memory sizing is validated before programming.

Failure evidence established why a ROM download ACK is insufficient:
MPUS=0x10470006, MPUT=0x3f880400 and MPUR=0x80 identified CONN writes blocked
by catch-all region7. The acknowledged LM patch was absent from reserved
DDR. Restoring region6 allowed WMT patch execution, EFUSE and RF calibration;
restoring the separate region5 allowed WLAN Ready and actual scanning.
The old region5 setup happened inside HifPdmaInit, but it is unrelated to
whether the new transport uses DMA or PIO.

Transport and userspace
-----------------------

The native BTIF path uses the real FIFO/IRQ protocol with a software RX
queue and process-context STP parsing. Software FIFO overflow and raw LSR
hardware indications are counted separately. WAKE must go low for 64--96us
then high, following the original BSP, before waiting for the real firmware
wake response. A repeated write of one fails after firmware idle.

After that real sleep acknowledgement, the transport synchronizes/disables
its RX IRQ, masks IER and waits at most 100ms for TX empty. Unread RX bytes
or pending/running parser work veto idle. Only an empty transport releases
its CCF clock vote. Wake restores the clock and IER/IRQ before the firmware
wake request; no FIFO reset is performed across this retained idle state.
Close handles both active and gated states without an unbalanced IRQ depth
or a gated-register access. Debug reads also refuse gated MMIO. All process
I/O and clock transitions share a mutex, but RX work is never flushed while
holding it because an STP callback may need to send an ACK.

The read-only ``btif_state`` attribute beside the native platform ``state``
reports cached transport ownership, gate/restore/wake counters, idle vetoes,
timeouts and RX overruns. Reading it does not wake BTIF or access its gated
registers. Check ``mt6580_btif_bus`` in CCF and TOPCG_CTRL1 bit12 separately
for the physical gate, rather than treating cached flags as hardware proof.

The source Android dispatcher configures BTIF and validates/responds to
real srh_patch requests. It does not take the original launcher's permanent
LPBK vote. Firmware/configuration loading occurs on function-on after vendor
filesystems exist. The factory WIFI record remains unchanged; an empty MAC
does not invalidate its RF calibration. A valid NIC address is preferred,
otherwise the fallback is explicitly a locally administered random address.
Only setting the already active address is supported, not a simulated
firmware MAC update. AES-CMAC is exposed only when the management-frame
protection key path is compiled in; the station bring-up configuration leaves
it disabled.

Android uses an AIDL service with a minimal truthful nl80211 legacy HAL,
wificond and a current wpa_supplicant. Supported capabilities are restricted
to 2.4GHz STA. Unsupported AP/P2P, PMF/WPA3, PNO, advanced vendor offload and
statistics are not advertised. The HAL retries the signed regulatory database
after /vendor mounts instead of hardcoding country/channel permissions.

Qualification and remaining scope
---------------------------------

On 2026-10-28, kernel #191 cold-booted the Android stack without manual MMIO,
reported real EFUSE level2 / VCN33=3500000uV and zero MPU violations. Both
raw nl80211 and Android delivered ten BSS scan results. Settings completed
WPA2 association and DHCP. DNS, default-route HTTPS200 and repeated gateway /
external ICMP traffic passed; steady five-packet samples had no loss and
netdev TX/RX errors remained zero. Firmware-idle Wi-Fi disable released CONN
and its radio supply; re-enable and scanning passed.

SPM refuses DDR/infra/MAINPLL removal while CONN is on. Complete connected
deep sleep, long throughput/pressure runs, persistent fallback MAC and advanced wireless
features remain separate work. No permanent test wakelock is required.

The final #192 image also tightens that cipher advertisement. MIUI HTTP and
HTTPS generate_204 probes are configured at the user request and actually
validate the Android network. After validation, a subsequent radio cycle
automatically re-associated, acquired DHCP and passed three gateway probes.
Apple time.apple.com SNTP returned a real cached time result; no test server
override was installed.

Kernel #193 follows the BSP TX-drain/BTIF CCF gate ordering. On a connected
radio, idle reported one successful gate, CCF prepare/enable=0/0 with
hardware=N and TOPCG_CTRL1 changed from 0x98a18a3c to 0x98a19a3c (bit12).
With that gate still closed, gateway and external ICMP samples each received
all five packets and default-route HTTPS returned 200. Initial probes made
before boot-time DHCP had no route; these are retained in the evidence and
not counted as successful connectivity tests. Radio disable restored BTIF
and issued a real wake pulse before releasing CONN and the radio supply;
re-enable associated and acquired DHCP, then gated BTIF again. No RX FIFO
overflow, raw hardware overrun, idle timeout or MPU violation was observed.

At the #193 checkpoint Android lacked disconnected screen-off scan fallback:
it requests unsupported hardware PNO and retries, while waking the screen
allows an ordinary scan and reconnect. Software PNO configuration/qualification
was still pending at that checkpoint. The later software fallback described
below does not advertise a nonexistent firmware offload.

The physical unplug/screen-off/replug experiment completed and restored a
validated network with five of five gateway replies, but it did not complete
system suspend: success=0, fail=3, including two device-suspend aborts with
``WLAN TX THREAD`` pending. With WoWLAN deliberately unadvertised,
``wiphy_suspend()`` calls ``cfg80211_leave_all()``; the disconnect OID wakes
the TX thread and aborts the transition. The inherited AHB platform
suspend/resume callbacks are no-ops. Integrating qualified cfg80211/WoWLAN,
firmware suspend/ownership and wake IRQ handling is required next. No claim
of retained connected suspend or deeper DDR/PLL collapse follows from this
experiment. BTIF remained gated with zero overruns/timeouts/MPU violations
and the kernel remained untainted after recovery.

Retained STA suspend
--------------------

The cfg80211 PM bridge implements only ``WIPHY_WOWLAN_ANY``. In this
standard mode the radio continues normal operation, and its ordinary AHB
HIF interrupt can wake the host. The supplicant overlay requests
``wowlan_triggers=any``; a configured cfg80211 suspend therefore keeps the
STA association instead of issuing the disconnect OID observed on #193.
The physical WLAN device is wake-capable. Suspend enables IRQ wake and
resume balances it, including unwind after another device aborts suspend.
IRQ teardown also releases an outstanding wake vote. Unsupported wake
patterns, magic packet, GTK rekey and ARP offloads are not advertised.

The bridge does not call the legacy IP-filter suspend helpers: their IPv6
cast is invalid, and enabling unqualified offloads would change the meaning
of ANY. Genuine firmware sleepy events and ownership handshakes/readback
continue in the original driver. Neither firmware ownership nor a firmware
sleep acknowledgement is manufactured. The cached netdev ``pm_state``
attribute records suspend/resume, IRQs observed while armed, and suspend-time
ownership/WHLPCR. Reading it does not perform MMIO. Check system suspend
statistics, SPM state and wake reasons independently of callback counts.

CONN and DDR remain powered under the existing SPM guard. This retained
mode is separate from complete connected DDR/infra/MAINPLL collapse.

Kernel #194 qualification (2026-10-28): the physical unplug/screen-off/replug
cycle completed with seven successful system suspends and seven balanced
cfg80211 suspend/resume callbacks. Each wake was recorded as IRQ99
``AHB_SLAVE_HIF`` by the kernel, not inferred from callback counts. Before
suspend, firmware-owned=1, sleepy=1, flags=0 and WHLPCR=0x1
agreed with the real firmware ownership protocol. SPM recorded state3,
cpu0_mode=0, fallback=1 and r0 blocker=0x4: the CONN guard correctly selected
retained standby instead of removing DDR/infra/MAINPLL. One unplug-time
``mt6350-usb`` wake event canceled suspend; no WLAN TX THREAD abort remained.
The same IPv4 address was present before and after all recorder snapshots;
there was no suspend-triggered carrier-off. After replug, the network was
validated, all five gateway pings returned and HTTPS returned 200, with zero
netdev TX/RX errors. IRQ wake was disarmed on resume, BTIF remained gated,
MPU violations stayed zero and the kernel remained untainted. ANY can wake
for ordinary radio traffic; selective wake/offload, long-duration power
measurement and complete connected deep sleep remain unqualified.
An additional radio off/on cycle released CONN/LDO, restored the saved STA
connection, re-enabled ANY on the newly registered wiphy and passed three
gateway probes without loss. The wiphy index changes across function cycles;
diagnostics must resolve the current netdev's phy80211 link instead of
assuming phy0. The first post-cycle diagnostic used that stale name and was
corrected; it was not a missing wiphy or failed WoWLAN configuration.

Android software scan fallback
------------------------------

The A1260 vendor RRO enables the Wi-Fi module's permitted
``config_wifiSwPnoEnabled`` customization. The one-shot shell initializer
defaults DeviceConfig ``wifi/software_pno_enabled`` to true only if unset.
Both gates are required by this framework. Its existing ``SwPnoScanState``
uses real single scans when firmware scheduled scan is unavailable, keeping
the framework's normal mobility/fast/slow backoff and alarm cancellation.
No firmware PNO capability, polling daemon or permanent wake lock is added.
The kernel remains #194 with retained ANY-mode suspend and the CONN guard.

On 2026-10-28 the RRO and both gates survived normal boot. With the screen
asleep throughout a normal radio off/on cycle, Android explicitly entered
Software PNO, associated and acquired DHCP; five gateway probes and HTTPS200
passed with no netdev errors. A separate physical unplug/AP-off/AP-on/replug
experiment verified loss detection, scheduled 20s and 40s retries and a
return to the same test network before USB was reconnected, while wakefulness
was Asleep. AlarmManager recorded two actual software-PNO wakeups and then
canceled the pending 300s retry after connection. The system completed six
retained suspends; two additional attempts were canceled by alarmtimer with
-EBUSY. The cfg80211 wake vote was balanced across all eight callbacks and
unwind paths. Gateway probes (five of five), HTTPS200, validation, MPU and
netdev error checks passed; taint remained zero. Other saved networks were
temporarily excluded from autojoin for isolation and restored afterward.
This is not qualification of complete connected DDR/infra/MAINPLL collapse
or long-term standby current.
