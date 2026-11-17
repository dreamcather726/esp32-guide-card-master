# Customer MQTT User Binding Bridge

The customer continues publishing this non-retained message to
`cabinet/cmd/{cabGuid}`:

```json
{"groupID":"9","userGUID":"DTkxHempvRmp"}
```

The bridge observes fresh `device/broadcast/online` telemetry, checks an
owner-approved MAC/cabGuid pairing via the management API, requests a server-issued
binding for that exact boot, and publishes the complete `bindUser` command on the
same command topic. The device verifies that command with the voice server.
Only a matching `bindingState=ready` report completes the flow. MQTT PUBACK or a
successful API call alone does not mean the device is ready to talk.

## First-Time Device Registration

In the management site's device-user-binding screen, register the physical device
MAC and its cabGuid once. This requires the device owner's login. It is a device
identity mapping, not a user assignment. Do not infer the MAC from the IMEI, the
USB RNDIS MAC, or another device. Subsequent online reports update the page
automatically; manual report import and full-command copying remain available.

Each boot still requires a new assignment from the customer. Old boot IDs,
retained assignments, and stale online reports are not reused to restore a user.
If an assignment arrives before fresh mapped telemetry, the bridge returns an
error and the customer should resend it after the device is online.

## Credentials and Access

- Dedicated service account: `AB12cd34`. Never use a device's clientId.
- Credential request: POST `http://mqttak.suji.work:8443/api/credentials`,
  JSON `{"imei":"AB12cd34"}`.
- Temporary MQTT username/password are held only in memory. The bridge refreshes
  a 3600-second credential after 2880 seconds, then reconnects/resubscribes.
- Subscriptions: `device/broadcast/online` and `cabinet/cmd/+`.
- Publish permission: `cabinet/cmd/+`; QoS 1, retain false.
- Default `iot/+/msg` in the credential response does not prove business-topic ACL
  access. `probe.py` checks SUBACK and observes traffic without publishing commands.
- WSS uses normal certificate/hostname verification. The supplied credential API
  is HTTP and exposes credentials in transit; `ALLOW_HTTP_CREDENTIALS=1` explicitly
  opts into this existing endpoint. Migrate the issuer to HTTPS when available.
- The Baidu endpoint negotiates TLS 1.2 RSA AES-GCM cipher suites; these are
  explicitly enabled in addition to ephemeral-key suites. Certificate verification
  stays enabled and TLS versions below 1.2 are excluded.
- The backend service secret is a read-only file, never a browser field or log.
  The deployment calls the API on host loopback, not public plaintext HTTP.

## Retry and Identity Rules

- SQLite stores pending intent, boot, user, request ID and public session/version
  only. It does not persist the binding token or MQTT credentials.
- Backend idempotency returns the same issued command for retry; the encrypted
  short-lived server cache must still match the active session and current boot.
- New user assignment supersedes an older pending intent for that device.
- A supplied producer `cmdID` is remembered durably and rejected if reused for a
  different boot/user/group. The original two-field format remains supported,
  but without a producer ID or boot ID a freshly republished old payload is
  indistinguishable from an intentional new assignment. The publisher must send
  a new assignment only for the current user, without retain.
- Pending intents expire after 180 seconds. Retries use bounded backoff; a restart
  requires new non-retained telemetry before resuming an intent.
- Echo responses, full bind/unbind commands, group-only commands and retained
  messages never generate another assignment. This prevents shared-topic loops.
- MQTT 3.1.1 clears the retain bit for live forwards to existing subscribers.
  The retain guard rejects subscription-time retained replay; enforce retain=false
  at the publisher (or broker ACL) rather than claiming detection of every live
  retained publication.
- `bindingAuto` responses have states `requested`, `dispatched`, `ready`, `error`.
  Consumers must ignore these responses as commands. No tokens are in responses.
- For 2.1.7 compatibility, the bridge also sends a separate group-only message.
  2.1.8 saves group independently and reports a legacy user request as pending.
- Saving a profile does not silently replace a running session's profile snapshot.
  Rebinding creates a new snapshot, using the existing verified binding workflow.

## Deploy

Build and test with Python 3.11 or later:

```sh
python -m unittest -v test_bridge.py
docker build -t vanex-mqtt-binding-bridge:20261004 .
```

Create `bridge.env` from `bridge.env.example` and put the existing management API
service secret in `api-secret`. Both files should be mode 0600, owned by the
deployment administrator. Neither belongs in Git. Mount a persistent `state/`.
Start only one instance per MQTT service account:

```sh
docker compose -f compose.yml up -d
docker compose -f compose.yml ps
docker logs --tail 80 vanex-mqtt-binding-bridge
```

`probe.py` also requires `STATE_DIR` pointing to the same mounted state directory
as the service. An exclusive account lock prevents it from displacing the running
bridge. Stop the bridge first to probe with this account; otherwise use a separately
provisioned account. Never run the same account in another MQTT debugging client.

Health checks verify the event loop, MQTT connection and successful business
subscriptions. They do not certify an individual physical device or API mapping.
`host_admin.py` is an explicit site-specific deployment helper. It backs up the
JAR, frontend and database before an API/frontend replacement. It does not run
automatically and does not publish fleet OTA updates.

## Acceptance and Rollback

1. Confirm the MAC/cabGuid pairing on the management page.
2. Boot one physical device; wait for a fresh report with a new bootID.
3. Send the original two-field command, without retain.
4. Check group update, issued command, matching ready report, and automatic page
   update; speak and confirm server-side transcript attribution.
5. Repeat the same assignment: no new session/version should be created.
6. Change users: old personal context must not be reused. Reboot: old retained
   commands must not bind the previous user.

Stop the bridge to disable automatic dispatch. Manual full verified commands
remain usable. Restore the backed-up API JAR/frontend only if those components
need rollback; the additive database tables can remain. Do not overwrite the
live database with an older dump without accounting for newly stored transcripts.
