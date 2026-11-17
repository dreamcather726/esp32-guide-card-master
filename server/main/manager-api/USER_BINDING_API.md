# Boot-scoped user binding

All paths below are relative to the existing `/xiaozhi` context path. Responses
use the existing `{code,msg,data}` envelope: code 0 is success. Existing handlers
often return HTTP 200 for application errors; callers must inspect `code`.

## Operator API

These routes use the existing login token and `sys:role:normal` permission. Every
device operation checks its owner against the logged-in account. Profiles are
keyed by `(ownerId,userGUID)`; a bare userGUID is never a public lookup key.

Profile GET/PUT (including bounded prefix queries) also accept a dedicated `X-API-Key` instead of a login
token (never both). It is fixed to one account's profiles, with no device,
history, internal-service or admin permissions. Keys are randomly generated,
stored only as SHA-256 digests, revocable and invalid when the owner is disabled.
API-key userGUIDs use 1..128 ASCII letters, digits, underscores or hyphens.
Profile responses disable caching. See `server/deploy/profile-api-key/README.md`
for provisioning and integration. All other operator routes still require login.

- `GET /user/profile/{userGUID}`: data is `{userGUID,profileVersion,profile}` or null.
- `GET /user/profile/{prefix}?match=prefix`: data is an array of at most 30 profile
  views for this account. Matches literal, case-sensitive prefixes (including exact
  matches); exact first, then userGUID ascending. No match returns `[]`. The prefix
  must be nonempty and at most 128 characters. Underscores are literal, not LIKE
  wildcards. No pagination; refine the prefix when more than 30 users match.
  This also accepts the dedicated profile API key. No user, device or session is
  changed. Omitting match or using match=exact preserves the original GET contract.
  Other match modes return an error. PUT with match=prefix is rejected.
- `PUT /user/profile/{userGUID}`: body is the profile object. Replaces the original four
  fields; omitted original fields become null. Returns the same shape as GET and increments
  profileVersion atomically. Edits take effect on the next binding, not the current session.
- Profile fields are optional nullable strings: nickname (64), language (32),
  interests (1024), explanationStyle (512). Limits are Java string lengths. There
  are no enums.
- Since 2026-10-10, `relationship` is an optional string (64), for labels such as
  self, spouse or father. It is not an agent/role ID or an instruction for the AI.
- `usageCount` is an optional JSON integer from 0 to 2147483647, supplied by the
  customer. It is stored as an absolute value, never incremented on binding or playback.
  Strings, floating-point numbers, booleans, negatives and out-of-range values are rejected.
- For backward compatibility, missing/null `relationship` or `usageCount` preserves
  the existing value during updates. Use `relationship:""` to clear the label and
  `usageCount:0` to reset the count. New profiles default to null relationship and zero count.
  Preservation happens in the same SQL upsert, without a read-modify-write race.
- GET reflects saved values immediately. A new binding snapshots both new fields;
  current/historical sessions retain their original snapshot. Older four-field
  snapshots still load and may return null for both added fields. These fields
  do not alter voice prompts, playback limits or automatic usage accounting.
- `POST /device/{macAddress}/user-binding`: body `{bootID,userGUID,profile?}`.
  Both IDs are nonblank strings up to 128 characters without surrounding spaces.
  The authenticated operator supplies the actual bootID from the customer's MQTT
  online event. This service does not subscribe to or authenticate that broker.
- POST data: `{userGUID,bootID,bindingVersion,usageSessionID,bindingToken,profile,
  profileVersion,agentId,expiresAt,command}`. Command is
  `{cmd:"bindUser",cmdID,userGUID,bootID,bindingVersion,usageSessionID,bindingToken}`.
  Publish this command on the customer's existing MQTT channel. Firmware must
  check its live bootID and increasing version before applying it.
- `GET /device/{macAddress}/user-binding`: the same metadata without token or
  command; null when no valid current binding exists.
- `DELETE /device/{macAddress}/user-binding`: revokes the server token, reserves
  the next version under the same lock, and returns command data directly:
  `{cmd:"unbindUser",cmdID,userGUID:"",bootID,bindingVersion}`. No token or session
  fields are included. No previous active session means data null.

POST intentionally issues a fresh session/token each time. Reconnects must use
the previously issued token, not POST again. The raw 256-bit random opaque token
is exposed only in its creation response, is not recoverable via GET, and must
not be logged or stored in browser storage. Usage sessions store only SHA-256;
the optional MQTT bridge below keeps a short-lived AES-GCM encrypted retry response.
`user-binding.token-ttl-seconds` defaults to 86400, permitted range 1..604800.

## Trusted Resolve

`POST /config/user-binding/resolve` inherits the existing server Bearer filter.
Body: `{macAddress,userGUID,bootID,bindingVersion,bindingToken}`. Data:
`{userGUID,bootID,bindingVersion,usageSessionID,profile,profileVersion,agentId,expiresAt}`.

Call at hello and before every voice turn for firmware explicitly opting in with
`features.user_binding`. Legacy hello and management APIs stay unchanged. There
is no implicit strict-mode switch or MQTT consumer in this service. The optional
MQTT bridge below records trusted boot observations separately.
The manager verifies the token-bound boot, current binding version, token hash,
expiry, device identity/owner, and assigned agent ownership. Firmware verifies
the *live* boot. Without a new operator binding, this service does not independently
discover a reboot. Resolve/current binding and chat records use the same immutable
profile data/version from binding creation. Repeated resolves retain the same usageSessionID.

## Chat Reporting

`POST /agent/chat-history/report` retains the existing server Bearer filter and
legacy fields. Optional fields: messageId (128), usageSessionId (36), bootId (128),
roleId (128), turnId (128), deliveryStatus, bindingVersion, textTruncated (boolean).
Aliases userGUID, usageSessionID and
bootID are accepted for userGuid, usageSessionId and bootId respectively.

Bound reports require messageId, usageSessionId, bootId, userGuid, a positive matching
bindingVersion, chatType 1 or 2, nonempty content (at most 16000 characters), plus the
existing sessionId. Whitespace-only chunks are retained verbatim. Legacy content
still must be nonblank. Text is
stored regardless of chat-history or memory configuration. Raw audio is off unless
`user-binding.store-raw-audio=true` is separately configured. Agent/user/owner/
profile snapshots are taken from the validated usage session, not from caller-
supplied agent data. The transport sessionId remains informational; idempotency
for bound messages survives reconnects with another transport sessionId.

messageId has a nullable unique index, so legacy rows with no ID remain valid.
Retransmission of the same ID and immutable message content is idempotent; an ID
collision with different content/identity is rejected. Delivery states are
`received`, `generated`, `sent`, `sent_unconfirmed`, `played`, `failed`, `interrupted`. Defaults are
received for user and generated for assistant. Sent is not proof of playback.
`played` requires `playbackAck:true`, asserted by the trusted server only after a
real firmware ACK. Repost the same message and ID with the ACK to advance status;
retries never downgrade terminal status.

Trusted buffered reports may arrive after session expiry/revocation. They still
must match the original session's MAC/user/boot/bindingVersion; they retain that session's
snapshots rather than being attributed to a new user. Resolve rejects expired or
revoked credentials. Legacy reports without usageSessionId retain the existing
storage configuration behavior. Bound reportTime accepts seconds or milliseconds;
legacy timestamp handling is unchanged.

## History

- `GET /user/profile/{userGUID}/sessions?page=1&limit=50`
- `GET /user/profile/{userGUID}/sessions/{usageSessionID}/messages?page=1&limit=50`

Both require normal operator login and enforce owner/user/session scope. data is
an array, with no total count. page is 1..1000000; limit is 1..100. Sessions sort
newest first, messages by ascending createdAt then database ID. Chat createdAt is
the original reportTime converted to milliseconds, not database arrival time;
the existing database column is DATETIME(3). History also exposes reportTime as an
epoch-millisecond integer. Late outbox retries retain the original chunk order.
Sessions expose id (usageSessionID),
macAddress,deviceId,ownerId,userGuid,bootId,bindingVersion,agentId,agentName,
profileSnapshot (JSON string),profileVersion,expiresAt,revokedAt,createdAt; never
tokenHash. Messages expose existing chat fields plus all new metadata and snapshots.

Missing profile/current binding: code0/data:null. Missing or foreign device/session
and invalid/expired binding: generic code500. Existing auth codes: 401/403;
request bean validation: 10034. No live-boot mismatch is detected at operator POST.

## Migration And Checks

Migration `202610021200.sql` follows `202607241100` at the end of the master.
It adds three tables and nullable chat fields/indexes without rewriting old rows.
Apply through the project's ordinary Liquibase release process before using new
code. This task does not deploy, modify production data, or run existing DB/Redis
integration tests.

Focused unit tests (no database or Redis required):

```sh
mvn -DskipTests=false -Dtest=UserBindingServiceTest,BoundChatStorageTest test
```

The existing default skipTests=true is preserved; the property now supports an
explicit command-line override. Validate migration execution and concurrency
against a disposable MySQL instance as part of release acceptance.

## Automatic MQTT Binding Bridge

All routes retain the existing `{code,msg,data}` envelope. The manager does not
subscribe to MQTT or fetch broker credentials. The trusted bridge holds tokens
in memory only and publishes the returned command unchanged. Do not put tokens
in browser storage, logs, unencrypted disk queues, or MQTT online reports.

### Owner Pairing And Browser View

- `PUT /device/{macAddress}/mqtt-binding` body: `{"cabGuid":"CAB"}`.
  Requires login, `sys:role:normal`, and current ownership of the registered MAC.
  It explicitly pairs one cabGuid with one MAC and snapshots the deviceId/owner.
  A cabGuid already paired with another MAC is rejected; it is never inferred
  from IMEI, userGUID, telemetry, or caller-supplied ownerId.
- `GET /device/{macAddress}/mqtt-binding` returns exactly these data fields:
  `{macAddress,cabGuid,report,receivedAt,fresh}`. PUT returns the same view.
  MAC is normalized to lower-case colon notation. An owned but unpaired device
  has `cabGuid:null,report:null,receivedAt:null,fresh:false`.
- `report` is a sanitized raw KJ object or null. `receivedAt` is an ISO-8601 UTC
  datetime ending in `Z`, assigned by the manager on receipt, or null.
  `fresh` means the server receipt is between 0 and 180 seconds old, inclusive.
  A stale report remains visible with `fresh:false`.
- Repeating the same valid pairing is idempotent and preserves telemetry.
  Changing cabGuid, deviceId, or owner starts a new mapping generation and clears
  observations/cached commands. Reownership/device recreation invalidates the
  old mapping; the new owner must explicitly PUT again. Their GET reveals no old
  owner's mapping or telemetry. Previously used request IDs remain consumed.

### Trusted Online Observation

`POST /config/user-binding/online` uses the existing `/config/**` server Bearer
secret filter. Its body is the raw KJ JSON object, not a wrapper:

```json
{"cmd":"KJ","cabGuid":"CAB","imei":"123456789012345","bootID":"BOOT",
 "userGUID":"USER","groupID":"GROUP","bindingState":"legacy_pending",
 "bindingVersion":0,"usageSessionID":"","fwVersion":"2.1.7","Battery Level":"75"}
```

`cmd:"KJ"`, cabGuid and bootID are required. macAddress is optional for firmware
2.1.7; when present it must be a valid MAC matching the explicit registration.
The mapping's deviceId and owner must still match the registered device. Unknown
cabinets, reassigned devices, and optional-MAC mismatches are rejected. IMEI is
informational and is never used to discover or change the mapping.

Only cmd, cabGuid, bootID, macAddress, imei, userGUID, groupID, bindingState,
bindingVersion, usageSessionID, fwVersion and these metrics survive sanitization:
Battery Level, Volume, Screen Brightness, Signal Strength, TF Card Capacity,
TF Card Remaining Space, Total Runtime, Single Session Runtime, Playback Duration,
Conversation Count, Role. Text fields have a 128-character bound; metrics retain
only scalar strings/numbers/booleans up to 128 characters. bindingVersion must be
a nonnegative signed-64-bit JSON integer. Nested metrics and unknown fields are
discarded, including bindingToken, tokenHash, profile, ownerId and receivedAt.
The response data is the same token-free view as GET. A changed boot invalidates
cached commands, including when an older boot is subsequently observed again.
Only forward live observations; a retained/replayed MQTT packet is not proof of
live device presence, and server receipt time does not establish publisher time.

### Trusted Legacy Upgrade

`POST /config/user-binding/legacy` uses the same server Bearer filter. Exact body:

```json
{"cabGuid":"CAB","bootID":"BOOT","userGUID":"USER","requestID":"REQUEST"}
```

All four identifiers are nonblank, at most 128 characters, with no surrounding
whitespace or control characters. No caller-supplied owner or profile is used.
The current mapping and a fresh observed same boot (at most 180 seconds old)
are mandatory, even on retry. The service calls the existing
`UserBindingService.create(registeredOwner, registeredMac, Create(bootID,userGUID,null))`.
This preserves the owner-scoped profile, verified boot/token/session flow,
agent ownership validation, and hash-only usage-session token storage.

Data is the existing Binding DTO, including `bindingToken` and `command`:
`{userGUID,bootID,bindingVersion,usageSessionID,bindingToken,profile,profileVersion,
agentId,expiresAt,command}`. Command is the existing
`{cmd:"bindUser",cmdID,userGUID,bootID,bindingVersion,usageSessionID,bindingToken}`.
Protocol versions stay JSON integers. Use the existing expiresAt representation;
only telemetry receivedAt has the new explicit ISO-8601 contract.

requestID is durably unique per `(macAddress,cabGuid)`, with case-sensitive IDs.
Same-payload retries return the exact same command/token/session after restarts.
Different payloads with the same ID, changed mapping/boot, expired cache, revoked
sessions, and superseded sessions are rejected without creating a replacement.
A *new* requestID for the same currently active user+boot reuses its unexpired
cached command and original expiry. Without a reusable cache, a new intentional
requestID may recover an undelivered binding by creating a new session/version;
the old IDs remain rejected. If the latest same-boot report says `ready` and its
userGUID, usageSessionID and bindingVersion exactly match the active session,
recovery issuance is rejected to avoid rotating a working binding. Telemetry is
not used to grant verified identity; token resolve remains authoritative.
New intentional users/boots require new IDs. Preserve the original
requestID across bridge transport retries; do not generate IDs to recover a
rejected revoked/superseded request.

The outbox's maximum lifetime is 180 seconds, capped by token expiry. AES-256-GCM
uses a random 96-bit nonce and a key derived with SHA-256 from
`"xiaozhi:mqtt-binding-outbox:v1\0" || SysParamsService.getValue(Constant.SERVER_SECRET,true)`.
Associated data authenticates mapping, MAC/cab/request, boot/user/session/version
and expiry. Only ciphertext is persisted; the key is not stored with it. The
server secret must be stable across instances/restarts. Secret rotation rejects
old cached responses instead of minting replacements. Ciphertext is scrubbed by
a scheduled 60-second cleanup; expiry is enforced immediately on reads. Keep
token-free request tombstones so delayed retries remain rejected.

Migration `202610041200.sql` adds `ai_mqtt_binding_mapping` (mapping plus sanitized
latest telemetry) and `ai_mqtt_binding_outbox`. It follows `202610021200.sql`.
Bridge transactions use READ_COMMITTED so the initial nonlocking cabinet lookup
does not pin an old session/outbox snapshot while waiting for another device
transaction. They acquire device, mapping, then binding-state locks. Session creation
and encrypted outbox insertion commit together; a failed encryption/write rolls
back both. Existing binding/resolve/revoke implementations and Shiro chains are
unchanged.

Focused bridge and existing binding tests:

```sh
mvn -DskipTests=false -Dtest=MqttBindingServiceTest,MqttBindingCipherTest,MqttBindingContractTest,UserBindingServiceTest,UserBindingSerializationTest,BoundChatStorageTest test
```

Opt-in real MySQL fixture (creates and drops only a random `mqtt_binding_it_*`
database, applies both production migrations, exercises concurrent requests and
transaction rollback):

```sh
mvn -DskipTests=false -Dmqtt.test.mysql=true -Dtest=MqttBindingMysqlTest test
```

By default it connects to local MySQL on port 3306, with credentials read from
application-dev.yml but never that file's database name or host. Override with
environment variables MQTT_TEST_MYSQL_URL (server-only JDBC URL with empty database
path and a query string), MQTT_TEST_MYSQL_USER, MQTT_TEST_MYSQL_PASSWORD. This is
opt-in and requires CREATE/DROP DATABASE privileges. Credentials are not printed.
