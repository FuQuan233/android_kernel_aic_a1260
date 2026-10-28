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

The current BTIF light-idle path retains its AP bus clock. It is not the
original BSP's complete TX-drain/CCF-gating state machine. SPM refuses
DDR/infra/MAINPLL removal while CONN is on. Complete connected deep sleep,
long throughput/pressure runs, persistent fallback MAC and advanced wireless
features remain separate work. No permanent test wakelock is required.

The final #192 image also tightens that cipher advertisement. MIUI HTTP and
HTTPS generate_204 probes are configured at the user request and actually
validate the Android network. After validation, a subsequent radio cycle
automatically re-associated, acquired DHCP and passed three gateway probes.
Apple time.apple.com SNTP returned a real cached time result; no test server
override was installed.
