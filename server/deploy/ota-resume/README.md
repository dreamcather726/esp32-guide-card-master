# OTA Resume Deployment (2026-10-04)

Firmware implementation, log analysis and on-device acceptance checklist:
`firmware/docs/OTA_RESUMPTION.md` at the repository root.

## Live Changes

- Java `OTAMagController` delegates range/ETag/SHA-256 responses to `FirmwareDownloadResponse`.
- The Redis ticket still expires normally; individual ranges no longer consume a three-download quota.
- Host Nginx inherited `proxy_cache cache_one`. The new exact download prefix disables caching/compression and forwards Range/If-Range to the API. Other paths remain unchanged.
- `nginx-download-location.conf` is installed under the deployment directory and included by the port-8002 server; a copy is installed at `/www/server/panel/vhost/nginx/extension/robot.suji.work/ota-resume.conf` for the public domain.

Deployment directory: `/opt/xiaozhi-server/deploy-20261004-ota-resume`.
Backups: `backup/api.jar`, `backup/xiaozhi-api-proxy.conf`.
Deployed JAR SHA-256: `2043093fd75eac93f7ccec1327ced154ec13e142b20a14af4248dadaf24e4b4f`.

The API container was restarted. The Python voice container, database and Redis were not restarted. No database migration was added for this change. The firmware release table still contains 2.1.3 and 2.1.2; 2.1.6 has not been published to the fleet.

## Verification

`smoke_download.py` runs on the deployment host, creates a temporary Redis download ticket for the existing 2.1.3 image and removes the ticket in `finally`. It does not call the device registration/version POST endpoint or change device identities. It reads the Redis password from container metadata without logging it.

```sh
python3 /opt/xiaozhi-server/deploy-20261004-ota-resume/smoke_download.py
OTA_SMOKE_BASE=http://127.0.0.1:8002 python3 /opt/xiaozhi-server/deploy-20261004-ota-resume/smoke_download.py
```

Both routes passed: range 206, byte-for-byte assembly and SHA-256 match, more than three requests, mismatched If-Range returning 200, unsatisfied range returning 416, and no ticket TTL extension. Frontend and OTA GET health checks also passed.

Local Java verification: 50 non-database tests passed. Five pre-existing database integration tests fail because the local development MySQL credentials do not match; they are not counted as successful.

## Rollback

Stop `xiaozhi-esp32-server-web`, copy the backup JAR over `/opt/xiaozhi-server/manager-api/xiaozhi-esp32-api.jar`, then start the same container. Restore the backed-up port-8002 Nginx configuration and remove only the newly added `extension/robot.suji.work/ota-resume.conf`; run Nginx configuration validation before reloading. Database restoration is not required.
