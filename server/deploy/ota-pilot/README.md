# Vanex S3 OTA Release History

## Current Release

2.1.10 was published to all compatible registered `jiuchuan-s3` devices on
2026-10-07 at 12:11 Asia/Shanghai. Device auto-update opt-outs remain unchanged.
The single-device pilot is disabled. See `DEPLOYMENT-20261007-FLEET.md` and
`host_fleet_2_1_10.py` for the active release, verification and withdrawal command.

The 2026-10-06 2.1.9 procedure below and the earlier 2026-10-07 2.1.10 pilot
record are historical. Do not rerun their install/activate/rollback commands
against the current fleet release. All statements below describe that old pilot.

## Scope

- Target: `864479086586824s3`, MAC `7c:4f:ad:24:81:fc`.
- Immutable application: 2.1.9, 3,227,232 bytes.
- SHA-256: `202227aedb752c2a99a43e4a28426f54d7d2e2e148968f2a9e0ff9d2516edf2b`.
- Public `jiuchuan-s3` release remains 2.1.3. The pilot uses `pilot-jiuchuan-s3`.
- Configuration key: `server.ota_pilot`. Do not use `server.ota.pilot`, which
  collides with the existing scalar `server.ota` in the nested config builder.
- This is a version-check offer, not a forced device reboot or fleet push.

## Deployment

Upload the scripts, immutable app as `xiaozhi-2.1.9.bin`, API JAR, and its
SHA-256 in `api.sha256` to `/opt/xiaozhi-server/deploy-20261006-ota-pilot`.
Keep this directory owner-only. Execute these actions sequentially:

```sh
python3 /opt/xiaozhi-server/deploy-20261006-ota-pilot/host_pilot.py stage
python3 /opt/xiaozhi-server/deploy-20261006-ota-pilot/host_pilot.py install
python3 /opt/xiaozhi-server/deploy-20261006-ota-pilot/host_pilot.py verify-selection
python3 /opt/xiaozhi-server/deploy-20261006-ota-pilot/host_pilot.py verify-download
python3 /opt/xiaozhi-server/deploy-20261006-ota-pilot/host_pilot.py activate
```

`stage` backs up the API JAR and relevant DB rows before inserting a disabled
pilot. `install` restarts only the management API and rolls back on failed
health checks. Selection checks use disposable unowned fixture device rows,
not customer telemetry. Download checks verify full GET and Range requests,
SHA-256, If-Range behavior, repeated downloads, and ticket expiry through both
HTTPS and the legacy port 8002 entry point. Activation requires the matching
verification record and respects the target device's auto-update setting.

## Stop Or Roll Back

```sh
python3 /opt/xiaozhi-server/deploy-20261006-ota-pilot/host_pilot.py disable
python3 /opt/xiaozhi-server/deploy-20261006-ota-pilot/host_pilot.py rollback
```

Disabling stops new pilot offers; it does not cancel an in-flight device
download. Rollback also restores the API backup and checks health. It refuses
to replace an API JAR changed by another subsequent deployment. Pilot files
and metadata remain available for audit; the public release is unchanged.

## Device Acceptance

The target last reported 2.1.9 before this release was staged. That prior
manual installation is not evidence of OTA success. For a 2.1.0-to-2.1.9
test, the physical target must actually run the customer's batch 2.1.0 image.
Do not fake its reported version or remotely downgrade it.

Capture the boot version, board type, partition table, OTA response/download,
image validation, reboot, and the new 2.1.9 boot report. Confirm normal display,
network, user binding and audio behavior afterward. The initial download still
runs the 2.1.0 downloader; the new client's resume logic cannot repair an old
client until installed. Server smoke checks do not replace physical acceptance.

The immutable 2.1.9 TF fix downloads/validates `/sdcard/start.wav`; it does not
automatically replace the embedded activation sound with that file. The subsequent
2.1.10 adds that hook, using the existing SD playback queue once per boot after
validation, without interrupting another activity. It was released separately,
first to the confirmed device and then to the compatible fleet as recorded above.
