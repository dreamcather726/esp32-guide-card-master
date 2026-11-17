"""Durable, connection-independent chat reports using only the standard library."""

import asyncio
import json
import logging
import math
import sqlite3
import threading
import time
import uuid
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Awaitable, Callable


LOGGER = logging.getLogger(__name__)
DEFAULT_DB_PATH = Path(__file__).resolve().parents[2] / "data" / "report-outbox.sqlite3"
Sender = Callable[[dict], Awaitable[bool]]


class OutboxError(RuntimeError):
    """Persistence failed; bound producers must stop accepting new work."""


class ReportIdentityError(OutboxError):
    """The event has no complete, authenticated binding snapshot."""


class DuplicateMessageError(OutboxError):
    """A message ID was reused with different immutable event data."""


@dataclass(frozen=True)
class ReportIdentity:
    mac_address: str
    user_guid: str
    session_id: str
    usage_session_id: str
    boot_id: str
    binding_version: object
    role_id: str = None
    turn_id: str = None
    history_generation: int = None

    def __post_init__(self):
        if self.history_generation is not None and (type(self.history_generation) is not int or self.history_generation < 0):
            raise ReportIdentityError("Invalid history generation")
        for name in ("mac_address", "user_guid", "session_id", "usage_session_id", "boot_id"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ReportIdentityError(f"Missing report identity: {name}")
        version = self.binding_version
        if (type(version) not in (str, int) or
                (isinstance(version, str) and not version.strip()) or
                (isinstance(version, int) and version < 0)):
            raise ReportIdentityError("Missing or invalid report identity: binding_version")
        for name in ("role_id", "turn_id"):
            value = getattr(self, name)
            if value is not None and (not isinstance(value, str) or not value.strip()):
                raise ReportIdentityError(f"Invalid report identity: {name}")

    def payload(self, chat_type, content, report_time, *, message_id=None,
                delivery_status=None, text_truncated=False):
        """Capture an event once; neither enqueue nor delivery truncates its text."""
        payload = {
            "macAddress": self.mac_address,
            "userGuid": self.user_guid,
            "sessionId": self.session_id,
            "usageSessionId": self.usage_session_id,
            "bootId": self.boot_id,
            "bindingVersion": self.binding_version,
            "roleId": self.role_id,
            "turnId": self.turn_id or uuid.uuid4().hex,
            "messageId": message_id if message_id is not None else uuid.uuid4().hex,
            "deliveryStatus": delivery_status or ("received" if chat_type == 1 else "generated"),
            "chatType": chat_type,
            "content": content,
            "reportTime": report_time,
            "audioBase64": None,
            "textTruncated": text_truncated,
        }
        if self.history_generation is not None:
            payload["historyGeneration"] = self.history_generation
        _validate_payload(payload)
        return payload


def _validate_payload(payload):
    ReportIdentity(
        payload.get("macAddress"), payload.get("userGuid"), payload.get("sessionId"),
        payload.get("usageSessionId"), payload.get("bootId"), payload.get("bindingVersion"),
        payload.get("roleId"), payload.get("turnId"),
        payload.get("historyGeneration"),
    )
    for name in ("messageId", "turnId"):
        if not isinstance(payload.get(name), str) or not payload[name].strip():
            raise ValueError(f"Missing report field: {name}")
    if type(payload.get("chatType")) is not int or payload["chatType"] not in (1, 2, 3):
        raise ValueError("chatType must be 1, 2 or 3")
    if not isinstance(payload.get("content"), str):
        raise ValueError("content must be text (use an empty string for an empty event)")
    if type(payload.get("reportTime")) is not int or payload["reportTime"] < 0:
        raise ValueError("reportTime must be nonnegative epoch milliseconds")
    allowed_statuses = ("received",) if payload["chatType"] == 1 else ("generated", "sent_unconfirmed")
    if payload.get("deliveryStatus") not in allowed_statuses:
        raise ValueError("Invalid deliveryStatus; this pipeline has no playback acknowledgement")
    if payload.get("audioBase64") is not None:
        raise ValueError("Bound reports are text-only")
    if type(payload.get("textTruncated")) is not bool:
        raise ValueError("textTruncated must be boolean")


class ReportOutbox:
    """Synchronous durable enqueue plus bounded async delivery.

    Connections are short-lived and never cross threads. SQLite serializes writers
    across producer threads/processes; no transaction is held over a network await.
    """

    def __init__(self, db_path=None, *, max_attempts=20, base_delay=1.0,
                 max_delay=300.0, send_timeout=15.0, poll_interval=1.0,
                 busy_timeout=5.0):
        if type(max_attempts) is not int or max_attempts < 1:
            raise ValueError("max_attempts must be a positive integer")
        for name, value in (("base_delay", base_delay), ("max_delay", max_delay),
                            ("send_timeout", send_timeout), ("poll_interval", poll_interval),
                            ("busy_timeout", busy_timeout)):
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"{name} must be positive and finite")
        if max_delay < base_delay:
            raise ValueError("max_delay must be at least base_delay")
        self.db_path = Path(db_path or DEFAULT_DB_PATH).resolve()
        self.max_attempts = max_attempts
        self.base_delay = base_delay
        self.max_delay = max_delay
        self.send_timeout = send_timeout
        self.poll_interval = poll_interval
        self.busy_timeout = busy_timeout
        self._storage_error = None
        self._delivery_lock = threading.Lock()
        self._worker_task = None
        self._initialize()

    @contextmanager
    def _database(self):
        db = None
        try:
            db = sqlite3.connect(str(self.db_path), timeout=self.busy_timeout,
                                 isolation_level=None)
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA synchronous=FULL")
            yield db
        except (sqlite3.Error, OSError) as exc:
            self._storage_error = f"{type(exc).__name__}: {exc}"
            LOGGER.error("Report outbox storage failure: %s", self._storage_error)
            raise OutboxError(f"Report outbox storage failure: {exc}") from exc
        finally:
            if db is not None:
                db.close()

    def _initialize(self):
        try:
            self.db_path.parent.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            LOGGER.error("Report outbox directory failure: %s", exc)
            raise OutboxError(f"Report outbox directory failure: {exc}") from exc
        with self._database() as db:
            mode = db.execute("PRAGMA journal_mode=WAL").fetchone()[0]
            if mode.lower() != "wal":
                raise OutboxError("Report outbox requires SQLite WAL mode")
            if db.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                raise OutboxError("Report outbox integrity check failed")
            db.execute("BEGIN IMMEDIATE")
            db.execute("""CREATE TABLE IF NOT EXISTS report_outbox (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                message_id TEXT NOT NULL UNIQUE,
                payload TEXT NOT NULL,
                status TEXT NOT NULL DEFAULT 'pending'
                    CHECK (status IN ('pending', 'delivered', 'error')),
                attempts INTEGER NOT NULL DEFAULT 0,
                next_attempt_at REAL NOT NULL DEFAULT 0,
                created_at REAL NOT NULL,
                delivered_at REAL,
                last_error TEXT
            )""")
            db.execute("""CREATE INDEX IF NOT EXISTS report_outbox_due
                ON report_outbox(status, next_attempt_at, id)""")
            db.execute("""CREATE TABLE IF NOT EXISTS report_outbox_health (
                id INTEGER PRIMARY KEY CHECK (id = 1), probe INTEGER NOT NULL
            )""")
            db.execute("INSERT OR IGNORE INTO report_outbox_health VALUES (1, 0)")
            db.commit()

    def enqueue(self, payload):
        """Commit before returning. Exact duplicate events are idempotent, not replaced."""
        serialized = json.dumps(payload, ensure_ascii=False, sort_keys=True,
                                separators=(",", ":"), allow_nan=False)
        snapshot = json.loads(serialized)
        _validate_payload(snapshot)
        message_id = snapshot["messageId"]
        with self._database() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT payload FROM report_outbox WHERE message_id = ?",
                             (message_id,)).fetchone()
            if row is not None:
                if row["payload"] != serialized:
                    raise DuplicateMessageError(f"Conflicting report messageId: {message_id}")
            else:
                db.execute("""INSERT INTO report_outbox(message_id, payload, created_at)
                    VALUES (?, ?, ?)""", (message_id, serialized, time.time()))
            db.commit()
        return message_id

    def health(self):
        """Probe actual writes for listening gates; transient network failures are queued."""
        summary = {"healthy": False, "writable": False, "pending": 0, "error": 0,
                   "delivered": 0, "oldest_pending_at": None,
                   "worker_running": self._worker_task is not None and not self._worker_task.done(),
                   "last_error": self._storage_error}
        try:
            with self._database() as db:
                db.execute("BEGIN IMMEDIATE")
                db.execute("UPDATE report_outbox_health SET probe = probe + 1 WHERE id = 1")
                for row in db.execute("SELECT status, COUNT(*) AS n FROM report_outbox GROUP BY status"):
                    summary[row["status"]] = row["n"]
                summary["oldest_pending_at"] = db.execute("""SELECT MIN(created_at)
                    FROM report_outbox WHERE status IN ('pending', 'error')""").fetchone()[0]
                row = db.execute("""SELECT last_error FROM report_outbox
                    WHERE status != 'delivered' AND last_error IS NOT NULL
                    ORDER BY id DESC LIMIT 1""").fetchone()
                summary["last_error"] = row[0] if row else None
                db.commit()
            self._storage_error = None
            summary["writable"] = True
            summary["healthy"] = summary["error"] == 0
        except OutboxError:
            summary["last_error"] = self._storage_error
        return summary

    def requeue(self, message_id):
        """Explicit operator retry of an exhausted/error record; preserve its payload/ID."""
        with self._database() as db:
            result = db.execute("""UPDATE report_outbox SET status = 'pending',
                attempts = 0, next_attempt_at = 0, last_error = NULL
                WHERE message_id = ? AND status = 'error'""", (message_id,))
            return result.rowcount == 1

    def _claim(self, now):
        with self._database() as db:
            db.execute("BEGIN IMMEDIATE")
            # A crash during the final attempt must retain a visible error record.
            db.execute("""UPDATE report_outbox SET status = 'error',
                last_error = COALESCE(last_error, 'Attempt budget exhausted before acknowledgement')
                WHERE status = 'pending' AND attempts >= ? AND next_attempt_at <= ?""",
                       (self.max_attempts, now))
            row = db.execute("""SELECT * FROM report_outbox
                WHERE status = 'pending' AND next_attempt_at <= ? ORDER BY id LIMIT 1""",
                             (now,)).fetchone()
            if row is not None:
                db.execute("""UPDATE report_outbox SET attempts = attempts + 1,
                    next_attempt_at = ? WHERE id = ?""",
                           (now + self.send_timeout + self.max_delay, row["id"]))
            db.commit()
            return dict(row) if row else None

    def _fail(self, row, now, error, *, permanent=False):
        attempts = row["attempts"] + 1
        delay = min(self.max_delay, self.base_delay * (2.0 ** min(attempts - 1, 30)))
        status = "error" if permanent or attempts >= self.max_attempts else "pending"
        with self._database() as db:
            db.execute("""UPDATE report_outbox SET status = ?, next_attempt_at = ?, last_error = ?
                WHERE id = ? AND status = 'pending' AND attempts = ?""",
                       (status, now + delay, error[:1024], row["id"], attempts))
        LOGGER.warning("Report delivery %s: messageId=%s attempt=%s: %s",
                       status, row["message_id"], attempts, error[:1024])

    async def deliver_once(self, sender: Sender, *, now=None):
        """Try at most one due report. Return whether a record was attempted, not its ack."""
        if not self._delivery_lock.acquire(blocking=False):
            return False
        try:
            row = self._claim(time.time() if now is None else now)
            if row is None:
                return False
            try:
                payload = json.loads(row["payload"])
                _validate_payload(payload)
                if payload["messageId"] != row["message_id"]:
                    raise ValueError("Stored message ID mismatch")
            except (TypeError, ValueError, AttributeError, ReportIdentityError) as exc:
                self._fail(row, time.time() if now is None else now,
                           f"Invalid stored payload: {exc}", permanent=True)
                return True
            try:
                accepted = await asyncio.wait_for(sender(payload), timeout=self.send_timeout)
                if accepted is not True:
                    raise ValueError("Sender did not return explicit True")
            except Exception as exc:
                self._fail(row, time.time() if now is None else now,
                           f"{type(exc).__name__}: {exc}")
                return True
            # Cancellation or an acknowledgement-write failure leaves the durable row retryable.
            with self._database() as db:
                db.execute("""UPDATE report_outbox SET status = 'delivered', delivered_at = ?,
                    last_error = NULL WHERE id = ?""",
                           (time.time() if now is None else now, row["id"]))
            return True
        finally:
            self._delivery_lock.release()

    async def _run(self, sender):
        while True:
            try:
                attempted = await self.deliver_once(sender)
            except Exception:
                LOGGER.exception("Report outbox worker failure; retaining durable records")
                attempted = False
            await asyncio.sleep(0 if attempted else self.poll_interval)


_singleton_lock = threading.RLock()
_singleton = None


def get_report_outbox(db_path=None, **options):
    """Initialize once per process; explicit conflicting paths/settings are errors."""
    global _singleton
    with _singleton_lock:
        if _singleton is None:
            _singleton = ReportOutbox(db_path, **options)
        else:
            if db_path is not None and Path(db_path).resolve() != _singleton.db_path:
                raise OutboxError("Report outbox singleton already uses a different database")
            for name, value in options.items():
                if getattr(_singleton, name, None) != value:
                    raise OutboxError(f"Report outbox singleton already uses different {name}")
        return _singleton


def start_report_outbox(sender: Sender, db_path=None, **options):
    """Call in bootstrap's running loop. Repeated starts do not create extra workers."""
    loop = asyncio.get_running_loop()
    if not callable(sender):
        raise TypeError("sender must be an async callable returning explicit bool")
    with _singleton_lock:
        outbox = get_report_outbox(db_path, **options)
        task = outbox._worker_task
        if task is not None and not task.done():
            if task.get_loop() is not loop:
                raise OutboxError("Report outbox worker belongs to a different event loop")
            return outbox
        outbox._worker_task = loop.create_task(outbox._run(sender), name="chat-report-outbox")
        return outbox


async def stop_report_outbox():
    """Stop delivery without deleting any reports or relying on a live connection."""
    with _singleton_lock:
        outbox = _singleton
        task = outbox._worker_task if outbox else None
        if task is None:
            return
        task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    finally:
        with _singleton_lock:
            if outbox._worker_task is task:
                outbox._worker_task = None
