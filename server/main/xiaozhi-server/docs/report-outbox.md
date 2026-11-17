# Durable Bound Chat Reports

## Bootstrap Contract

`core.utils.report_outbox` is standard-library-only. Bootstrap integration and the
HTTP adapter belong to the parent task, not to `reportHandle.py`.

```python
from core.utils.report_outbox import (
    get_report_outbox, start_report_outbox, stop_report_outbox,
)

async def send_report(payload: dict) -> bool:
    # Send this exact captured payload. Check BOTH HTTP and application acceptance.
    # Return True on accepted insert OR an accepted duplicate messageId.
    # Return False or raise on all other outcomes. Do not resolve identity here.
    return await report_payload_to_manager(payload)

# Inside the running bootstrap event loop, before accepting bound connections:
outbox = start_report_outbox(send_report)

# Before listening for a bound user; persistence exceptions also stop that turn:
health = outbox.health()
accept_bound_audio = health["healthy"] and health["worker_running"]

# During server shutdown (not connection shutdown):
await stop_report_outbox()
```

`start_report_outbox(sender, db_path=None, **options)` is synchronous and creates
one asyncio worker per process. Repeated starts return the same outbox/task; the
first active sender remains in use. An explicit conflicting path or option raises.
`get_report_outbox()` returns the same singleton without starting a worker. Direct
`ReportOutbox(path)` instances and `await outbox.deliver_once(sender)` are available
for tests/operator tooling; production uses the bootstrap singleton.

The callback receives a new deserialized dict per attempt and has no connection
reference. Only `result is True` acknowledges: `None`, `1`, response objects,
exceptions, timeouts, and cancellation never imply success. Callback retries must
not supplement missing fields from a current user, device binding, or connection.
The Java API must validate the captured session/device/user tuple, enforce a
unique `messageId`, and treat a previously accepted identical message as success.
Duplicate IDs with different identities/content must fail, not overwrite.

## Storage and Operations

The default path is `xiaozhi-server/data/report-outbox.sqlite3`, resolved relative
to the module rather than the shell working directory. Both existing compose
files already bind `./data:/opt/xiaozhi-esp32-server/data`. Keep the whole directory
on persistent local storage, including SQLite `-wal` and `-shm` sidecars. Do not
mount only the database file or use a filesystem without working SQLite locks.
No compose change is required. Identity and chat text are plaintext local data;
restrict directory access and include the outbox in the deployment retention and
backup policy. Use SQLite's backup API for live backups, not a lone DB-file copy.

Each enqueue commits with WAL and `synchronous=FULL` before it returns; it is
intentionally synchronous at the event hook. Concurrent producers use separate
SQLite connections, a bounded busy timeout, and a unique message ID constraint.
No database transaction is held over a network await. Normal crash/restart
recovers persisted rows without a live websocket. A claim interrupted by a crash
becomes due after at most `send_timeout + max_delay` seconds from the claim.
This is at-least-once delivery; a crash after remote acceptance and before local
acknowledgement repeats the same message ID and immutable payload.

Options (keyword arguments on initial `start_report_outbox`/`get_report_outbox`):

| Option | Default | Meaning |
| --- | --- | --- |
| `max_attempts` | 20 | Per-record attempt budget, persisted across restart |
| `base_delay` | 1 s | First retry delay |
| `max_delay` | 300 s | Exponential retry delay cap |
| `send_timeout` | 15 s | Async callback timeout |
| `poll_interval` | 1 s | Idle/storage-failure worker sleep |
| `busy_timeout` | 5 s | Maximum SQLite lock wait |

Exhausted or malformed records remain in `error`. Other records keep delivering;
no records (including delivered ones) are automatically deleted. After diagnosing
and fixing the cause, `outbox.requeue(message_id)` explicitly resets an error
record's attempt budget, preserving its payload/ID. New events with conflicting
duplicate IDs raise `DuplicateMessageError`. Do not generate a new ID to retry an
old message. Retention/archival is an explicit later operational decision.

`health()` performs a real committed write probe and returns `healthy`, `writable`,
`worker_running`, `pending`, `error`, `delivered`, `oldest_pending_at` (epoch seconds),
and `last_error`. `healthy` means writable storage and no retained error records;
the parent also checks `worker_running`. Temporary HTTP failures remain pending
and do not close the listening gate while storage works. Corrupt/read-only/full/
locked storage raises `OutboxError` on enqueue and reports unhealthy in `health()`.
A failed initial open raises, rather than creating an in-memory replacement.
Failed appends are not accepted events: the caller must stop that turn and retain
its event for retry or surface the failure. The outbox has no copy of an event
whose initial commit failed. A successful later write probe permits storage
recovery; it does not retroactively acknowledge a failed append.

## Event Contract

`enqueue_report(conn, type, text, opus_data, timestamp, *, identity=None,
message_id=None, delivery_status=None, text_truncated=False)` returns the bound
event's `messageId` only after commit. `enqueue_asr_report` and
`enqueue_tts_report` accept the same optional event keywords and `report_time`
(epoch milliseconds, generated once at the hook if omitted). Bound hooks run before every legacy reporting switch,
memory/history setting, and old queue. They log and propagate failures. Only
legacy connections use `conn.report_queue` and its pre-existing switches.

When `conn.user_binding_required` is true, an event needs
`conn.user_binding_ready`, `device_id`, `user_guid`, `session_id`,
`usage_session_id`, `boot_id`, and `binding_version`. An unready or incomplete
binding raises `ReportIdentityError`, never falls back to legacy/current user.
`role_id` and `turn_id` are optional connection attributes: absent role stays null;
absent turn produces an event-local UUID, not a fabricated shared conversation
turn. Supply a real turn ID on the snapshot to correlate ASR and assistant events.

For a delayed producer, call `capture_report_identity(conn, turn_id=sentence_id)` at event creation
and pass its frozen `ReportIdentity` to the later hook as `identity=snapshot`.
Supplying this snapshot deliberately retains the original user even after unbind,
rebind, reconnect, role switch, or connection closure. Publish binding fields
atomically in the parent: set readiness false before mutation, change the binding
version, and publish readiness last. The capture helper detects readiness/version
changes, but it is not a replacement for the parent's binding synchronization.

The parent-owned `TranscriptTextQueue.put()` is the generation boundary: persist
every nonempty `ContentType.TEXT` chunk before `super().put()` makes it visible to
the TTS consumer. Do not report from `store_tts_text()`; it caches cumulative
subtitles rather than defining immutable events. Bound audio consumers skip their
old report callback to avoid duplicates. This captures streamed LLM, direct answer,
tool result, error response, and one-sentence fallback text regardless of later
synthesis failure, interruption, or playback queue clearing.

Capture a sentence snapshot on FIRST, or on the first TEXT for existing fallback
paths with no FIRST marker. Reuse that snapshot through LAST, including chunks
produced after connection closure; release it after LAST is published. An absent
snapshot for a new sentence after closure is an error, not permission to consult
another user's current binding. Bound connections keep their original user for
their entire lifetime; a different user requires a new connection. Do not evict
active sentence snapshots silently when limiting the cache. Either bound active
producers explicitly or fail closed before losing their identity.

Assign the event's UUID and timestamp once, and retain them with its DTO/event
metadata before the first WAL attempt. Retrying the same DTO after a storage or
queue-publication failure must reuse that event, including the original snapshot.
Distinct DTO chunks with identical text still get distinct message IDs. A random
UUID retained on the DTO or a deterministic UUID of session/boot/sentence/chunk
index both work; generating a fresh UUID on every `put()` attempt does not. A
committed report whose playback enqueue then fails remains `generated`, not
`played`, and stays in the outbox. Text should never reach `super().put()` on a
snapshot/commit failure; abort the producer and close its listening gate.

```python
import time
import uuid
from core.handle.reportHandle import capture_report_identity, enqueue_tts_report

# At generation, while binding is ready; keep this snapshot with the producer.
snapshot = capture_report_identity(conn, turn_id=sentence_id)
message_id = uuid.uuid5(uuid.NAMESPACE_URL,
    f"tts:{snapshot.usage_session_id}:{snapshot.boot_id}:{sentence_id}:{chunk_index}").hex
report_time = int(time.time() * 1000)

# These event fields remain unchanged for any retry, including after disconnect.
enqueue_tts_report(conn, chunk_text, None, identity=snapshot,
    message_id=message_id, report_time=report_time, delivery_status="generated")
```

The immutable payload contains:

| Field | Meaning |
| --- | --- |
| `macAddress`, `userGuid` | Captured device and authenticated user |
| `sessionId`, `usageSessionId`, `bootId`, `bindingVersion` | Captured binding/session identity |
| `roleId`, `turnId` | Captured role (nullable), turn ID |
| `messageId` | Stable UUID for a new event, or explicit caller event ID |
| `deliveryStatus` | User: `received`; assistant/tool: `generated` or `sent_unconfirmed` |
| `chatType` | 1 user, 2 assistant, 3 tool |
| `content`, `reportTime` | Full hook text, original epoch milliseconds |
| `audioBase64` | Null: bound reports are text-only in this implementation |
| `textTruncated` | False by default; caller sets true for a known partial text |

Retries preserve the entire payload. Regenerating an event's timestamp/turn ID
while reusing its `messageId` is a conflict, not an identical retry; retain the
original payload for explicit enqueue retries. Raw Opus is never decoded or
persisted in the bound flow, regardless of `chat_history_conf=2`.

Text is not shortened locally, and no delivery claims hardware playback. The
generation queue hook does not prove device playback, so the default is
`generated`, never `played`. A producer with send evidence may opt for
`sent_unconfirmed`; the Java validation enum must accept this spelling. No hook
can infer text lost upstream before `put()`, or whether an individual input was
already truncated. `text_truncated=True` describes a known partial event; it is
not needed merely because a complete stream was divided into chunks. An abort
does not rewrite committed chunks or convert `generated` into `played`.

The coordinated Java DTO accepts nonempty whitespace-only content for bound
reports, preserving streamed text without creating rejected blank reports; legacy
content remains nonblank. Empty structural FIRST/LAST markers and FILE/ACTION
DTOs are not generated TEXT reports. The DTO also includes `sent_unconfirmed`,
`bindingVersion`, and `textTruncated`; database/UI roundtrips should be exercised
in the parent's end-to-end tests. This change does not alter TTS provider,
connection, or HTTP-client ownership.

## Tests

From `server/main/xiaozhi-server`:

```text
python -m unittest discover -s tests -p test_report_outbox.py -v
```

The suite needs only the standard library. It covers an abruptly exited child
process with a live WAL, restart retry, duplicate conflict/idempotency, exact bool
acknowledgement, timeout/cancellation/ack-write failures, immutable identities,
concurrent writes/claims, readonly/corrupt/locked storage, worker singleton,
bound hook fail-closed behavior, full/partial text, and legacy queue compatibility.
Queue integration tests import the parent-owned `core.utils.transcript_queue`
and standard-library DTO module; no TTS engine is imported or started. They verify
commit-before-publication without a consumer, FIRST/last-chunk snapshots after
close, failure gating, post-commit publication retry, distinct repeated chunks,
first-TEXT fallback, FILE/empty marker handling, legacy behavior, and concurrent
producers. Additional regressions cover the 64-active-sentence bound (overflow
fails closed without evicting the oldest identity), LAST capacity release, capture
failure gating, and retrying a LAST DTO after its sentence cache was cleaned up.
The parent should additionally test actual streamed/direct/tool/error
producers all reach this queue, and that bound audio-consumer callbacks stay off.
