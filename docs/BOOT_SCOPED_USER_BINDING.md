# Boot-Scoped Verified User Binding (2.1.5)

## Scope

Jiuchuan requires a fresh backend-issued binding on every ESP boot. `Board`
creates a UUID-v4 using the existing `esp_fill_random` helper once at construction.
The boot ID remains stable across MQTT/WebSocket reconnects, but is not stored in
NVS. The old `cab_mqtt/user_guid` value is neither restored nor updated.

`Board::GetUserBinding()` returns a mutex-protected snapshot containing
`boot_id`, `user_guid`, `version`, `token`, `usage_session_id`, and `state`.
Other board types retain their existing optional-binding behavior.

Local SD playback, the safety reminder, network setup, business MQTT, content
downloads, and OTA checking do not require a binding. Unbound devices do not open
personal audio or issue server role requests. This change does not publish OTA
or deploy anything.

## MQTT Commands

Publish a standalone command to `cabinet/cmd/{cabGuid}`:

```json
{
  "cmd": "bindUser",
  "cmdID": "bind-command-1",
  "bootID": "current-boot-uuid",
  "bindingVersion": 1,
  "userGUID": "backend-user-guid",
  "bindingToken": "opaque-backend-issued-token",
  "usageSessionID": "backend-usage-session"
}
```

The backend supplies all five binding fields. `bootID` must match this boot.
`bindingVersion` is a JSON number, an integer in `1..9007199254740991`, strictly
greater than the last accepted version for this boot. Stringified numbers,
fractions, zero, and missing fields on explicit binding commands are rejected.
`cmdID` is required. The exact latest accepted command can be retried with the
same ID/version/fields without interrupting audio, resetting counters, or
repeating verification. A conflicting equal version or an older command fails.
New commands should always use new IDs.

The explicit unbind command is:

```json
{
  "cmd": "unbindUser",
  "cmdID": "unbind-command-2",
  "bootID": "current-boot-uuid",
  "bindingVersion": 2,
  "userGUID": ""
}
```

Omit `bindingToken` and `usageSessionID` on unbind, or send empty strings. Nonempty
credentials on unbind are rejected. Unbind retains the version watermark so an
older bind cannot reactivate the previous identity. A later bind needs a higher
version. A different boot starts with version zero. Retained `bindUser`,
`unbindUser`, and legacy user binding requests are rejected before validation,
even when their boot/version fields match. The MQTT retain flag is captured on
the first fragment and carried through payload assembly.

All string identifiers must be printable ASCII without whitespace/control
characters, up to 256 bytes; token permits up to 4096 bytes. The token stays
opaque to firmware. The existing optional `{ "payload": { ... } }` envelope is
accepted. Non-retained group assignment is processed independently before the
binding branch, including on messages whose binding validation fails. Retained
commands never assign a group. Send role, SD, power, and network controls
separately from binding. Only the exact cabinet command topic changes identity.

ACK uses the same command topic, preserving the existing response format:

```json
{"deviceExecute":"OK","cmdID":"bind-command-1"}
```

```json
{"deviceExecute":"error","cmdID":"bind-command-1","msg":"reason"}
```

`OK` means locally accepted (or exact duplicate), not server-verified. The manager
API generates commands for the customer's existing MQTT publisher and must not
claim `ready` merely because it generated a command or received an ACK.

### Legacy Customer Assignment

The non-retained customer command remains compatible:

```json
{"groupID":"9","userGUID":"DTkxHempvRmp"}
```

Firmware saves/subscribes the group and notifies the existing content handler
before handling the user request. A valid user GUID without `cmd`, `bootID`,
`bindingVersion`, `bindingToken`, or `usageSessionID` is a request for the backend
bridge, not a binding. The historical `userGuid`/`user_guid` aliases are also
recognized. Explicit or partially populated binding commands still go through
the unchanged strict validator, never through this compatibility path.

Firmware logs that it is awaiting a server-issued `bindUser` and replies on the
same command topic:

```json
{"deviceExecute":"pending","cmdID":"","msg":"awaiting server-issued bindUser; user identity unchanged"}
```

An optional string `cmdID` is echoed. This request acknowledgment does not change
the RAM identity, token, boot/version, usage session, binding state, or per-user
counters. A new boot therefore still reports `unbound`; an existing verified
identity remains unchanged. Neither saving the group nor this pending response
means binding succeeded. The backend bridge observes the original customer
command and must issue a full current-boot `bindUser`; existing verification
still gates `ready`. Responses marked with `bindingAuto` (field or command) are
ignored before group/binding handling, as are echoed `deviceExecute` and
`downSDfile` responses, to prevent shared-topic loops.

## Online Reports

Topic remains `device/broadcast/online`; existing `KJ` fields are retained:

```json
{
  "cmd": "KJ",
  "cabGuid": "cabinet-guid",
  "macAddress": "aa:bb:cc:dd:ee:ff",
  "userGUID": "",
  "bootID": "current-boot-uuid",
  "bindingState": "unbound",
  "bindingVersion": 0,
  "usageSessionID": "",
  "fwVersion": "2.1.5"
}
```

| State | Meaning |
| --- | --- |
| `unbound` | Startup or explicit unbind; identity/token/session are empty. |
| `pending` | Credentials accepted, awaiting verification, or WebSocket disconnected. |
| `ready` | Both server hello and a matching ready context validated on this channel. |
| `error` | Context rejected/mismatched, service error, or verification timed out. |

State changes request an online report through the existing MQTT worker; normal
60-second reporting continues. `macAddress` comes from
`SystemInfo::GetMacAddress()` for backend device mapping. Reports never contain
`bindingToken`. Delivery
still uses the existing QoS 0 behavior, so the publisher/UI must tolerate lost
ACKs/reports and use exact command retries. Counters reset once for each accepted
new binding/unbind, not duplicate retries. Unbound counters have no user/session
attribution and are not a backend billing authorization.

## WebSocket Contract

Jiuchuan audio now selects WebSocket explicitly; MQTT/UDP does not implement the
verified context handshake and is not a fallback. A configured WebSocket URL is
therefore required even when OTA also returns MQTT settings.

The HTTP handshake sends `User-Binding: 1`, `User-Guid`, `Boot-Id`,
`Binding-Version`, `Binding-Token`, and `Usage-Session-Id`. The authoritative
hello opt-in is `features.user_binding: true`:

```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "features": {"mcp": true, "user_binding": true},
  "userGUID": "backend-user-guid",
  "bootID": "current-boot-uuid",
  "bindingVersion": 1,
  "bindingToken": "opaque-backend-issued-token",
  "usageSessionID": "backend-usage-session",
  "audio_params": {"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}
}
```

Python first sends the existing `hello`, then separately:

```json
{
  "type": "user_context",
  "state": "ready",
  "status": "ready",
  "userGUID": "backend-user-guid",
  "bootID": "current-boot-uuid",
  "bindingVersion": 1,
  "usageSessionID": "backend-usage-session"
}
```

Firmware uses `state`; `status` is optional and ignored. Every other ready field
above must match the connection snapshot exactly. Token echo is unnecessary.
The ordinary `session_id` from server hello remains separate from the backend's
`usageSessionID`. Python sends ready only after its components and durable outbox
are usable. Either reply order is accepted within one 30-second deadline after
sending client hello; hello alone never opens the audio gate.

```json
{
  "type": "user_context",
  "state": "error",
  "status": "error",
  "code": "binding_invalid",
  "message": "Binding verification failed"
}
```

`binding_required`, `binding_invalid`, and `service_unavailable` all fail closed.
An error does not need identity fields. Python may close with code 4003 after
the error; firmware blocks immediately on the error, then closes locally on the
application task. Malformed/mismatched ready and timeout behave the same way.
Old server implementations without this reply deliberately fail closed.

## Lifecycle And Caveats

- Commands are serialized on the application task: stop capture, invalidate
  queued/in-flight audio, close the old channel, then replace/clear the identity.
  The in-place `SendUserContext(user_guid)` path was removed.
- A new bind requests a verification-only connection, with no listen/role
  request or microphone start. If startup has not initialized the protocol, the
  activation-complete path retries verification. Normal later chat/role requests
  also verify before proceeding. An exact duplicate does not retry an error.
- Each connection uses one immutable header/hello snapshot and clears handshake
  event bits. Disconnected/obsolete connection callbacks cannot grant a new
  binding ready status. Queued chat UI actions are scoped to their audio channel.
- Outgoing mic/text, incoming personal JSON/audio, listening/speaking transitions,
  and role startup require the validated channel. Wake-word detection remains
  local; its pre-validation recording is not uploaded for binding-required
  boards. Raw UDP audio debugging is also disabled for these boards.
- Encoder/decoder generations discard obsolete work; processor stop synchronizes
  with feed/output and clears partial AFE output. SD playback is preserved during
  binding/unbind. Audio already submitted to the hardware output may finish its
  current frame; verify this bounded tail on the physical device.
- Firmware validates the envelope/replay watermark, not the token signature or
  expiry. Backend must verify token authority, cabinet/device, user, boot,
  version, and usage session; enforce MQTT publishing ACLs and production WSS.
  This change does not migrate existing endpoint configuration or add local token
  expiry interpretation. Revocation needs a higher-version unbind or server
  channel termination.
- The existing synchronous WebSocket connect/hello pattern is retained. A
  binding/unbind queued during a connection attempt waits for that attempt's
  existing transport timeout and the hello/context deadline. Network/OTA startup
  is never held waiting for an initial binding.

## Verification And Integration Handoff

The legacy MQTT compatibility update passed all 15 source integration checks and
both host C++ tests (`user_binding_test`, `user_binding_json_test`) with MSVC and
ESP-IDF's cJSON sources. Host artifacts were generated in a separate temporary
directory. No firmware build, packaging, version change, or hardware run was
performed. Parent owns firmware compilation, device validation, deployment, and
any OTA decision. Existing dirty changes were preserved; this compatibility
update touches only the MQTT client, JSON helper, focused tests, and this note.

No-build integration checks:

```powershell
python -m unittest discover -s tests -p test_user_binding_source.py -v
git diff --check -- firmware
```

Two host C++ test targets cover the RAM binding state machine and real cJSON
parsing/redaction, including legacy request classification and rejection of
incomplete explicit commands. Re-run them separately from the firmware build:

```powershell
cmake -S tests -B build-binding-tests -DCJSON_SOURCE_DIR="$env:IDF_PATH/components/json/cJSON"
cmake --build build-binding-tests
ctest --test-dir build-binding-tests --output-on-failure -C Debug
```

Required device checks: reset with old NVS user present; reconnect without boot
change; stale/missing/fractional-version/retained commands; exact duplicate retry;
same-version conflicting token; active-chat user switch; active-chat unbind;
legacy group/user assignment with pending reply and unchanged identity;
retained fragmented legacy/bind/unbind delivery; `bindingAuto` reply echo;
KJ MAC mapping; group-only content notification without binding success;
hello-only timeout; matching/mismatching session ready; error then 4003; abrupt
disconnect; SD safety playback and OTA/network startup while unbound; rapid
switching with encoder/decoder queues saturated; token-free logs at debug level.

For integration, copy source to `C:\vanex_s3_build\firmware` while preserving that
directory's `build` tree and `sdkconfig`. Do not mirror/delete the destination.
Include the new `main/boards/common/user_binding*.h`, documentation, and `tests`.

### Files Changed By This Task

- `CMakeLists.txt` (version 2.1.5).
- `main/application.cc`, `main/application.h`.
- `main/audio/audio_service.cc`, `main/audio/audio_service.h`.
- `main/audio/processors/afe_audio_processor.cc`, `main/audio/processors/afe_audio_processor.h`.
- `main/boards/common/board.cc`, `main/boards/common/board.h`.
- `main/boards/common/user_binding.h`, `main/boards/common/user_binding_json.h` (new).
- `main/boards/jiuchuan-s3/jiuchuan_dev_board.cc`.
- `main/boards/jiuchuan-s3/location_mqtt_client.cc`, `main/boards/jiuchuan-s3/location_mqtt_client.h`.
- `main/protocols/protocol.cc`, `main/protocols/protocol.h`.
- `main/protocols/websocket_protocol.cc`, `main/protocols/websocket_protocol.h`.
- `tests/CMakeLists.txt`, `tests/user_binding_test.cc`, `tests/user_binding_json_test.cc`, `tests/test_user_binding_source.py` (new).
- `docs/BOOT_SCOPED_USER_BINDING.md` (this file).
