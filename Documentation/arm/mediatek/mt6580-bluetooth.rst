MT6580 Bluetooth over STP
========================

The A1260 uses the Bluetooth task of the shared WMT/STP connectivity MCU.
``CONFIG_MT6580_CONSYS`` now builds an exclusive ``/dev/stpbt`` transport.
The original GPL WMT/STP core supplies function power and framing; the new
character device uses modern read/write/poll semantics. Android uses a
source-built AIDL HCI HAL in device/aic/a1260/bluetooth. The original Android
8.1 Bluetooth userspace libraries are not loaded into Android 17.

Opening powers the BT function, registers reset/RX callbacks and selects
the raw STP BT task. Close unregisters callbacks and releases BT alone.
Existing CONN regulators, MPU windows, clock gating and PSM remain shared
with Wi-Fi; see ``mt6580-wifi.rst``. The CONN SPM guard stays enabled:
radio-on suspend retains DDR/infra/MAINPLL. Full connected deep power-off
and Bluetooth remote wake from system suspend are not qualified here.

Original initialization
-----------------------

REA comparison of this unit's shipping libbt-vendor.so,
libbluetooth_mtk.so and bt_drv.ko establishes the five-command MT6580
consys preload. Each command must return its actual successful completion:

* FC1A: Bluetooth address, reversed six-byte order.
* FC79: six RF bytes from calibration offsets 12..17.
* FC93: three TX power bytes from offsets 27..29.
* FC7A: seven sleep bytes from offsets 18..24.
* 0C03: HCI Reset.

The original BT_Addr record is 64 bytes with an optional two-byte trailer.
Calibration comes only from this unit's private backup, never a generic
sample. Its address is replaced with the validated factory proinfo+0x68
address handed to the HAL by early read-only init. The HAL verifies real
Read BD_ADDR and Read Local Version replies before reporting ready.
Factory partitions are never written. Original PSM ioctls are stubs on
this build; no extra PSM disable/enable sequence is invented.

Compared artifact SHA256 values, retained for reproducibility:

* libbt-vendor.so:
  ac39d4ca51a8578040514c94efdc0e7d57e14effaa6035dbf7b3723fa3c0bfe8
* libbluetooth_mtk.so:
  2a22acae088a8c0c51f671bd4a54881784948d3f6ca445272400bc249dd52b1d
* bt_drv.ko:
  720ac9a2c3edb8b2dff37f41925679a7745ab93f3c050fa7e53367962c8ad782

Android firmware compatibility
------------------------------

Three adaptations are restricted to this real controller tuple: HCI 8,
manufacturer 70, revision 0x1400, subversion 0x0403.

* Supported Commands claims 0C5A, but a real preload probe returns
  UNKNOWN_HCI_COMMAND. Only this false capability bit is removed.
* LE features reports encryption alone. After verifying that bitmap and a
  successful legacy mask command, LE Set Event Mask is limited to 0x1f.
  Newer unsupported bits otherwise produce real error 0x20.
* Failed Sniff Subrating (0811) returns Command Status instead of the
  required Command Complete. The HAL adapts only that failed envelope,
  preserving the actual error, command credits and submitted handle.
  Actual status 0x1a after audio idle is preserved; success is never
  synthesized. Sniff Subrating support is not removed: an invalid-handle
  probe returns error 0x02, confirming that the opcode is implemented.

Hardware checkpoint (#199, 2026-10-28)
-------------------------------------

BT-only open received the exact real Reset response
``04 0e 04 01 03 0c 00``. Classic discovery, LE scan, phone SSP pairing,
headphone reconnect and AAC stereo playback through Android AudioTrack
passed. The user heard the tone; 264600 frames were submitted with zero
AudioTrack underruns. HFP control delivers headphone battery reports and
Settings displays the actual percentage. SCO/microphone/call audio is
not qualified by that battery/control result.

BT disable preserves working Wi-Fi. Disabling both radios powers CONN
off; BT-only cold initialization and subsequent Wi-Fi coexistence pass.
Five gateway pings and HTTPS 200 pass with both radios active, network
error counters and kernel taint zero. Before the adapters, Android
asserted on the two mismatched HCI error envelopes. With the adapters,
the real idle error is handled and no further crash occurred during the
power-cycle/audio checks. A normal reboot automatically enables the
configured profiles, reconnects the headphone and reports zero Bluetooth
crashes during the observed boot. Long audio, HFP audio, HID and actual
radio-on system-suspend wake remain follow-up work.
