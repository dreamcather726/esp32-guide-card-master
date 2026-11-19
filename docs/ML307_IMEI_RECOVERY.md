# ML307 identity recovery (2.1.7)

## Root cause

The 2.1.6 boot log reached RNDIS GOT_IP, but continuously reported
`physical IMEI is not ready; credentials request postponed`. The network worker
skipped all USB AT initialization when the modem's existing data session survived
an ESP32 reset. With an empty IMEI cache, MQTT credentials were never requested.
Without MQTT, the online report containing cabGuid/bootID was not published, so
saving a user profile in the management UI did not bind this device boot.

This log does not demonstrate an HTTP 403. A 403 after credentials are requested
is a separate API authorization/configuration issue.

## Changes

- Keep the existing 8-second RNDIS grace window in the background network worker.
- Obtain an AT control port and IMEI even if RNDIS is already online. Do not issue
  CPIN/CREG/MDIALUP on the already-online path or regress its UI state.
- Retry failed control/identity setup after 2, 4, 8, 16, then 30 seconds, capped at
  30 seconds. Wake maintenance on IP, USB connect/disconnect and control-port loss.
- Preserve ordinary cold-start dial-up. Retry an accepted dial without an IP after
  30 seconds instead of waiting forever for a new USB enumeration.
- Only probe vendor serial or CDC ACM control interfaces. Exclude RNDIS, CDC data,
  storage and unrelated USB classes even if their interface numbers are preferred.
- Parse complete OK/error lines and standalone IMEI responses, not arbitrary digit
  sequences in unsolicited notifications.
- Invalidate RAM identity on USB session changes; reject old-session responses.
  Use cached NVS identity only in WiFi mode, where USB is intentionally disabled.
- Serialize explicit CDC close with automatic disconnect cleanup, including list
  traversal and handle validation, to avoid concurrent close/free during retries.

No GPIO, LCD, SD, audio, LED, partition or user-binding protocol changes are included.
USB DM/DP remain GPIO19/GPIO20; this does not repurpose GPIO43/GPIO44 for modem AT.
The previous OTA resumption and boot-scoped user-binding behavior is retained.

## Automated verification

Run on a host with Python 3 and a C++17 compiler:

```sh
python3 tests/run_rndis_host_tests.py --cxx g++
python3 -m unittest discover -s tests -p 'test_*source.py' -v
```

The host runner uses the actual production worker and identity method definitions
with simulated USB/RTOS/NVS boundaries: 12 flow scenarios plus 35 parser/interface/
retry assertions. Source guards cover call ordering, callback scheduling and CDC
lifecycle locking. These checks supplement, not replace, ESP32 hardware testing.

ESP-IDF 5.5.4 build passed. Application version 2.1.7 is 3,224,592 bytes, with
22% space remaining in the 0x3f0000-byte application slot. Image checksum and hash
are valid. The bootloader, partition table, initial otadata and assets files are
unchanged from 2.1.6. The 30 OTA download and 10 Flash/NVS recovery simulations
also passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled.

Application SHA-256:
`0FA58ACD1A067C4956147C2D42FBF21A9FC3FDD3A452732011B149D8D01A53D5`.
No production server settings or fleet OTA release records were changed for 2.1.7.

## Hardware acceptance

1. Cold boot with blank NVS and a valid SIM. Confirm the actual modem IMEI is read,
   credentials POST uses IMEI+s3, and device/broadcast/online contains this bootID.
2. Leave ML307 powered and online; reset only ESP32 several times. Expect
   `ML307 IMEI parsed` and `RNDIS online; preserve data session, IMEI=ready; no dial-up`.
   Confirm AT+MDIALUP is absent on the already-online path.
3. Delay USB AT readiness, then restore it. Confirm bounded retry logs and eventual
   MQTT connection without another ESP32 reset. LCD and local UI must remain usable.
4. Disconnect/reconnect the modem during IMEI reads and AT probing. Check for
   recovery, absence of crashes, and no publication under a replaced modem's IMEI.
5. Import the fresh online report, send the binding command for that exact bootID,
   wait for bindingState=ready, then test a spoken question and server-side archival.
6. Recheck WiFi fallback and OTA on one device before broader deployment.

Missing SafetyReminder.mp3 and credentials HTTP 403 are separate faults. This
release does not create missing customer audio files or change API authorization.

## USB enumeration at power-on (2.1.26)

The 2.1.25 log shows `Root port reset failed` before BLE initialization and
before the CDC client starts consuming device events. The 4G modem had not
enumerated by the end of the capture, so neither IMEI nor DHCP was ready.

On ESP-IDF 5.5.4, the CDC driver still reserves the USB host early but keeps
its root port unpowered until the board and RNDIS callbacks are registered.
The Jiuchuan S3 build allows 500 ms for USB connection debounce and 100 ms
after bus reset. After 12 seconds without enumeration, the network worker
logs `[USB_BOOT] ML307 has not enumerated` and displays the existing modem
detection status while remaining subscribed to device events. It does not
reset an already enumerated modem or bypass the IMEI check.

For an on-board check, capture `[USB_BOOT] USB root port powered after client
setup`, `ML307 connected`, `ML307 IMEI parsed`, `AT+CSQ` and `USB RNDIS
GOT_IP` from a cold boot. Verify that a late attach continues through QR and
MQTT startup. The host tests exercise a late attach after the timeout but
cannot confirm electrical USB timing or modem power.

## AT endpoint allocation with RNDIS already online (2.1.27)

In ESP-IDF 5.5.4, `EP Alloc error: ESP_ERR_NO_MEM` can come from
allocating endpoint objects or the USB DMA descriptor lists. Exhausted USB
hardware channels report `No more HCD channels available` and return
`ESP_ERR_NOT_SUPPORTED`. The log alone does not identify the exact allocation
that failed.

In 4G mode, let the existing USB worker open and probe the AT port before
starting BLE. The AT port reads the receive transfer in its callback, so it
no longer allocates software rings or submits a notification transfer.
RNDIS retains its data and notification handling. Partial CDC allocations are
released once, and a failed AT probe does not close the already-open RNDIS
device. Open failures log `dma_free`, `dma_largest` and `internal_free`.
Wi-Fi mode still starts BLE at boot.

On the board, check for `ML307 AT control interface detected`, followed by a
valid IMEI and `AT+CSQ`. Then confirm that `BleBeacon` reports, RNDIS and both
MQTT sessions coexist. The BLE start depends on the AT port in 4G mode, so
missing or nonresponsive modules leave BLE unstarted until an AT probe succeeds.
