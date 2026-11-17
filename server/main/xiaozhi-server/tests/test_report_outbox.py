"""Run with: python -m unittest discover -s tests -p test_report_outbox.py -v"""

import asyncio
import copy
import dataclasses
import json
import queue
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SERVER_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVER_ROOT))

from core.handle import reportHandle
from core.providers.tts.dto.dto import ContentType, SentenceType, TTSMessageDTO
from core.utils import report_outbox
from core.utils.transcript_queue import TranscriptTextQueue
from core.utils.report_outbox import (
    DuplicateMessageError,
    OutboxError,
    ReportIdentity,
    ReportIdentityError,
    ReportOutbox,
    get_report_outbox,
    start_report_outbox,
    stop_report_outbox,
)


def identity(**changes):
    fields = dict(mac_address="device-A", user_guid="user-A", session_id="ws-A",
                  usage_session_id="usage-A", boot_id="boot-A", binding_version=7,
                  role_id="role-A", turn_id="turn-A")
    fields.update(changes)
    return ReportIdentity(**fields)


def event(message_id="message-A", **changes):
    payload = identity().payload(1, "user speech", 1700000000123, message_id=message_id)
    payload.update(changes)
    return payload


def connection(**changes):
    values = dict(user_binding_required=True, user_binding_ready=True,
                  device_id="device-A", user_guid="user-A", session_id="ws-A",
                  usage_session_id="usage-A", boot_id="boot-A", binding_version=7,
                  role_id="role-A", turn_id="turn-A", read_config_from_api=False,
                  need_bind=True, report_asr_enable=False, report_tts_enable=False,
                  chat_history_conf=0, memory=None, config={"selected_module": {"Memory": "nomem"}},
                  report_queue=queue.Queue(), logger=mock.Mock())
    values.update(changes)
    return SimpleNamespace(**values)


class ReportOutboxTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "data" / "report-outbox.sqlite3"
        self.singleton_patch = mock.patch.object(report_outbox, "_singleton", None)
        self.singleton_patch.start()
        self.addCleanup(self.singleton_patch.stop)

    async def asyncTearDown(self):
        await stop_report_outbox()

    def box(self, **options):
        return ReportOutbox(self.path, **options)

    def row(self, message_id="message-A"):
        with closing(sqlite3.connect(self.path)) as db:
            db.row_factory = sqlite3.Row
            row = db.execute("SELECT * FROM report_outbox WHERE message_id = ?",
                             (message_id,)).fetchone()
            return dict(row) if row else None

    def sql(self, statement, parameters=()):
        with closing(sqlite3.connect(self.path)) as db:
            result = db.execute(statement, parameters).fetchall()
            db.commit()
            return result

    async def test_restart_recovers_committed_event_from_crashed_process_wal(self):
        script = """
import json, os, sqlite3, sys
from core.utils.report_outbox import ReportOutbox
box = ReportOutbox(sys.argv[1])
reader = sqlite3.connect(sys.argv[1])
reader.execute('SELECT COUNT(*) FROM report_outbox').fetchone()
box.enqueue(json.loads(sys.argv[2]))
os._exit(0)
"""
        subprocess.run([sys.executable, "-c", script, str(self.path), json.dumps(event())],
                       cwd=SERVER_ROOT, check=True, timeout=20, capture_output=True)
        self.assertTrue(Path(str(self.path) + "-wal").exists())
        box = self.box()
        sender = mock.AsyncMock(return_value=True)
        self.assertTrue(await box.deliver_once(sender))
        sender.assert_awaited_once_with(event())
        self.assertEqual(self.row()["status"], "delivered")
        self.assertEqual(self.sql("PRAGMA journal_mode")[0][0], "wal")

    async def test_duplicate_id_is_idempotent_even_after_delivery(self):
        box = self.box()
        payload = event()
        self.assertEqual(box.enqueue(payload), "message-A")
        box.enqueue(dict(reversed(list(payload.items()))))
        sender = mock.AsyncMock(return_value=True)
        await box.deliver_once(sender)
        self.box().enqueue(payload)
        self.assertFalse(await box.deliver_once(sender))
        self.assertEqual(sender.await_count, 1)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM report_outbox")[0][0], 1)

    def test_conflicting_duplicate_never_overwrites_original(self):
        box = self.box()
        box.enqueue(event())
        for changes in ({"userGuid": "user-B"}, {"content": "different"}, {"bootId": "boot-B"}):
            with self.subTest(changes=changes), self.assertRaises(DuplicateMessageError):
                box.enqueue(event(**changes))
        self.assertEqual(json.loads(self.row()["payload"]), event())

    async def test_only_literal_true_acknowledges(self):
        box = self.box(max_attempts=10, base_delay=1, max_delay=4)
        box.enqueue(event())
        now = 100
        for result in (False, None, 1, "true", {"code": 0}, []):
            with self.subTest(result=result):
                await box.deliver_once(mock.AsyncMock(return_value=result), now=now)
                row = self.row()
                self.assertEqual(row["status"], "pending")
                self.assertIsNone(row["delivered_at"])
                self.assertLessEqual(row["next_attempt_at"] - now, 4)
                now = row["next_attempt_at"]
        await box.deliver_once(mock.AsyncMock(return_value=True), now=now)
        self.assertEqual(self.row()["status"], "delivered")
        self.assertEqual(self.row()["attempts"], 7)

    async def test_exception_backoff_and_retry_survive_restart(self):
        box = self.box(base_delay=2, max_delay=3)
        box.enqueue(event())
        sender = mock.AsyncMock(side_effect=OSError("offline"))
        await box.deliver_once(sender, now=100)
        self.assertEqual(self.row()["next_attempt_at"], 102)
        self.assertFalse(await box.deliver_once(sender, now=101.9))
        restarted = self.box(base_delay=2, max_delay=3)
        await restarted.deliver_once(sender, now=102)
        self.assertEqual(self.row()["next_attempt_at"], 105)
        self.assertEqual(self.row()["attempts"], 2)
        self.assertTrue(restarted.health()["healthy"])
        self.assertIn("offline", restarted.health()["last_error"])
        success = mock.AsyncMock(return_value=True)
        await restarted.deliver_once(success, now=105)
        self.assertEqual(success.await_args.args[0], event())
        self.assertEqual(self.row()["status"], "delivered")

    async def test_exhaustion_keeps_error_until_explicit_requeue(self):
        box = self.box(max_attempts=2)
        box.enqueue(event())
        sender = mock.AsyncMock(return_value=False)
        await box.deliver_once(sender, now=10)
        await box.deliver_once(sender, now=11)
        self.assertEqual(self.row()["status"], "error")
        self.assertFalse(await box.deliver_once(sender, now=99999))
        self.assertEqual(sender.await_count, 2)
        health = box.health()
        self.assertFalse(health["healthy"])
        self.assertTrue(health["writable"])
        self.assertEqual(health["error"], 1)
        self.assertTrue(box.requeue("message-A"))
        self.assertFalse(box.requeue("missing"))
        await box.deliver_once(mock.AsyncMock(return_value=True), now=99999)
        self.assertEqual(self.row()["status"], "delivered")
        self.assertEqual(json.loads(self.row()["payload"]), event())
        self.assertFalse(box.requeue("message-A"))

    async def test_hung_sender_times_out_without_ack(self):
        box = self.box(send_timeout=0.01)
        box.enqueue(event())

        async def sender(payload):
            await asyncio.sleep(10)
            return True

        await box.deliver_once(sender, now=100)
        self.assertEqual(self.row()["status"], "pending")
        self.assertIn("TimeoutError", self.row()["last_error"])

    async def test_cancellation_retains_claim_for_restart_retry(self):
        box = self.box(send_timeout=1, max_delay=2)
        box.enqueue(event())
        started = asyncio.Event()

        async def sender(payload):
            started.set()
            await asyncio.Future()

        task = asyncio.create_task(box.deliver_once(sender, now=100))
        await asyncio.wait_for(started.wait(), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(self.row()["status"], "pending")
        self.assertIsNone(self.row()["delivered_at"])
        restarted = self.box(send_timeout=1, max_delay=2)
        success = mock.AsyncMock(return_value=True)
        self.assertFalse(await restarted.deliver_once(success, now=102))
        self.assertTrue(await restarted.deliver_once(success, now=103))
        success.assert_awaited_once_with(event())

    async def test_crash_on_last_attempt_leaves_visible_error(self):
        box = self.box(max_attempts=1, send_timeout=1, max_delay=1)
        box.enqueue(event())
        box._claim(100)
        sender = mock.AsyncMock(return_value=True)
        self.assertFalse(await self.box(max_attempts=1).deliver_once(sender, now=103))
        sender.assert_not_awaited()
        self.assertEqual(self.row()["status"], "error")

    async def test_ack_write_failure_is_retried_with_same_id(self):
        box = self.box(send_timeout=1, max_delay=1)
        box.enqueue(event())
        actual_connect = sqlite3.connect

        async def sender(payload):
            patcher.start()
            return True

        patcher = mock.patch.object(report_outbox.sqlite3, "connect",
                                    side_effect=sqlite3.OperationalError("disk full during ack"))
        try:
            with self.assertRaises(OutboxError):
                await box.deliver_once(sender, now=100)
        finally:
            patcher.stop()
        self.assertIs(sqlite3.connect, actual_connect)
        self.assertEqual(self.row()["status"], "pending")
        retry = mock.AsyncMock(return_value=True)
        await self.box().deliver_once(retry, now=103)
        retry.assert_awaited_once_with(event())

    async def test_callback_and_caller_mutations_do_not_change_stored_identity(self):
        box = self.box()
        payload = event()
        box.enqueue(payload)
        payload["userGuid"] = "user-B"

        async def bad_sender(snapshot):
            self.assertEqual(snapshot["userGuid"], "user-A")
            snapshot["userGuid"] = "user-C"
            snapshot["messageId"] = "other-ID"
            raise RuntimeError("retry")

        await box.deliver_once(bad_sender, now=100)
        retry = mock.AsyncMock(return_value=True)
        await box.deliver_once(retry, now=101)
        retry.assert_awaited_once_with(event())

    def test_concurrent_producers_and_duplicates(self):
        boxes = [self.box(), self.box()]

        def enqueue(i):
            return boxes[i % 2].enqueue(event(f"message-{i % 40}"))

        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(enqueue, range(160)))
        self.assertEqual(len(set(results)), 40)
        self.assertEqual(boxes[0].health()["pending"], 40)

    async def test_slow_sender_does_not_block_producers_or_duplicate_claim(self):
        box = self.box()
        box.enqueue(event())
        started, finish = asyncio.Event(), asyncio.Event()

        async def sender(payload):
            started.set()
            await finish.wait()
            return True

        task = asyncio.create_task(box.deliver_once(sender, now=100))
        await asyncio.wait_for(started.wait(), 1)
        try:
            other = self.box()
            self.assertFalse(await box.deliver_once(sender, now=100))
            self.assertFalse(await other.deliver_once(sender, now=100))
            await asyncio.to_thread(box.enqueue, event("message-B"))
            self.assertEqual(box.health()["pending"], 2)
        finally:
            finish.set()
            await task
        self.assertEqual(box.health()["delivered"], 1)

    def test_corrupted_database_raises_and_is_not_replaced(self):
        self.path.parent.mkdir()
        damaged = b"not a SQLite database\x00" * 64
        self.path.write_bytes(damaged)
        with self.assertRaises(OutboxError):
            self.box()
        self.assertEqual(self.path.read_bytes(), damaged)

    def test_runtime_corruption_fails_health_and_enqueue(self):
        box = self.box()
        self.path.write_bytes(b"corrupted" * 256)
        self.assertFalse(box.health()["healthy"])
        self.assertFalse(box.health()["writable"])
        with self.assertRaises(OutboxError):
            box.enqueue(event())

    def test_real_sqlite_readonly_connection_fails_health_enqueue_and_boot(self):
        box = self.box()
        original_connect = sqlite3.connect

        def readonly_connect(path, **options):
            return original_connect(Path(path).as_uri() + "?mode=ro", uri=True, **options)

        with mock.patch.object(report_outbox.sqlite3, "connect", side_effect=readonly_connect):
            health = box.health()
            self.assertFalse(health["healthy"])
            self.assertFalse(health["writable"])
            self.assertIn("readonly", health["last_error"])
            with self.assertRaises(OutboxError):
                box.enqueue(event())
            with self.assertRaises(OutboxError):
                self.box()
        self.assertIsNone(self.row())
        self.assertTrue(box.health()["healthy"])

    def test_lock_timeout_is_a_persistence_failure(self):
        box = self.box(busy_timeout=0.01)
        with closing(sqlite3.connect(self.path)) as lock:
            lock.execute("BEGIN IMMEDIATE")
            with self.assertRaises(OutboxError):
                box.enqueue(event())
            self.assertFalse(box.health()["healthy"])
        box.enqueue(event())
        self.assertEqual(box.health()["pending"], 1)

    async def test_malformed_record_is_retained_and_does_not_block_following_reports(self):
        box = self.box()
        box.enqueue(event())
        box.enqueue(event("message-B"))
        self.sql("UPDATE report_outbox SET payload = ? WHERE message_id = ?", ("{broken", "message-A"))
        sender = mock.AsyncMock(return_value=True)
        await box.deliver_once(sender, now=100)
        sender.assert_not_awaited()
        self.assertEqual(self.row()["status"], "error")
        self.assertEqual(self.row()["payload"], "{broken")
        await box.deliver_once(sender, now=100)
        self.assertEqual(self.row("message-B")["status"], "delivered")

    async def test_worker_singleton_runs_without_connections_and_stops(self):
        sender = mock.AsyncMock(return_value=True)
        box = start_report_outbox(sender, self.path, poll_interval=0.01)
        task = box._worker_task
        self.assertIs(start_report_outbox(sender), box)
        self.assertIs(box._worker_task, task)
        self.assertIs(get_report_outbox(), box)
        with self.assertRaises(OutboxError):
            get_report_outbox(self.path.with_name("other.sqlite3"))
        with self.assertRaises(OutboxError):
            get_report_outbox(poll_interval=99)
        box.enqueue(event())
        for _ in range(100):
            if box.health()["delivered"]:
                break
            await asyncio.sleep(0.01)
        self.assertEqual(box.health()["delivered"], 1)
        self.assertTrue(box.health()["worker_running"])
        await stop_report_outbox()
        self.assertFalse(box.health()["worker_running"])
        self.assertTrue(task.done())
        self.assertIs(start_report_outbox(sender), box)
        self.assertIsNot(box._worker_task, task)

    def test_default_path_is_config_bind_mount_not_working_directory(self):
        self.assertEqual(report_outbox.DEFAULT_DB_PATH,
                         SERVER_ROOT / "data" / "report-outbox.sqlite3")

    def test_bound_hooks_ignore_legacy_switches_and_disable_audio(self):
        box = get_report_outbox(self.path)
        conn = connection()
        conn.report_queue = mock.Mock()
        conn.report_queue.put.side_effect = AssertionError("bound event reached legacy queue")
        with mock.patch.object(reportHandle, "opus_to_wav", side_effect=AssertionError("decoded audio")):
            for history in (0, 1, 2):
                conn.chat_history_conf = history
                for hook, chat_type in ((reportHandle.enqueue_asr_report, 1),
                                        (reportHandle.enqueue_tts_report, 2)):
                    message_id = hook(conn, "mandatory text", [b"raw audio"])
                    payload = json.loads(self.row(message_id)["payload"])
                    self.assertEqual(payload["chatType"], chat_type)
                    self.assertEqual(payload["content"], "mandatory text")
                    self.assertEqual(payload["deliveryStatus"], "received" if chat_type == 1 else "generated")
                    self.assertIsNone(payload["audioBase64"])
        conn.report_queue.put.assert_not_called()
        self.assertEqual(box.health()["pending"], 6)

    async def test_connection_rebind_never_changes_pending_identity(self):
        box = get_report_outbox(self.path)
        conn = connection()
        old = copy.copy(conn)
        message_id = reportHandle.enqueue_asr_report(conn, "original user", [])
        conn.user_guid = "user-B"
        conn.usage_session_id = "usage-B"
        conn.session_id = "ws-B"
        conn.boot_id = "boot-B"
        conn.role_id = "role-B"
        conn.turn_id = "turn-B"
        conn.binding_version = 8
        conn.user_binding_ready = False
        sender = mock.AsyncMock(return_value=True)
        await box.deliver_once(sender)
        payload = sender.await_args.args[0]
        self.assertEqual(payload["messageId"], message_id)
        for name, field in (("user_guid", "userGuid"), ("usage_session_id", "usageSessionId"),
                            ("session_id", "sessionId"), ("boot_id", "bootId"),
                            ("role_id", "roleId"), ("turn_id", "turnId"),
                            ("binding_version", "bindingVersion")):
            self.assertEqual(payload[field], getattr(old, name))

    def test_delayed_producer_can_pass_original_frozen_snapshot(self):
        get_report_outbox(self.path)
        conn = connection()
        snapshot = reportHandle.capture_report_identity(conn)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            snapshot.user_guid = "user-B"
        conn.user_guid = "user-B"
        conn.user_binding_ready = False
        message_id = reportHandle.enqueue_tts_report(conn, "old response", [], identity=snapshot)
        payload = json.loads(self.row(message_id)["payload"])
        self.assertEqual(payload["userGuid"], "user-A")
        self.assertEqual(payload["bindingVersion"], 7)

    def test_stable_tts_chunk_id_retries_after_close_with_original_snapshot(self):
        box = get_report_outbox(self.path)
        conn = connection()
        snapshot = reportHandle.capture_report_identity(conn, turn_id="sentence-42")
        conn.user_binding_ready = False
        conn.user_guid = None
        conn.usage_session_id = None
        options = dict(identity=snapshot, message_id="sentence-42-chunk-0",
                       report_time=1700000000456, delivery_status="generated")
        first = reportHandle.enqueue_tts_report(conn, "first chunk", None, **options)
        second = reportHandle.enqueue_tts_report(conn, "first chunk", None, **options)
        self.assertEqual(first, second)
        self.assertEqual(box.health()["pending"], 1)
        payload = json.loads(self.row(first)["payload"])
        self.assertEqual(payload["turnId"], "sentence-42")
        self.assertEqual(payload["reportTime"], options["report_time"])
        self.assertEqual(payload["userGuid"], "user-A")
        with self.assertRaises(DuplicateMessageError):
            reportHandle.enqueue_tts_report(conn, "first chunk with cumulative suffix", None, **options)
        options["message_id"] = "sentence-42-chunk-1"
        reportHandle.enqueue_tts_report(conn, " with cumulative suffix", None, **options)
        self.assertEqual(box.health()["pending"], 2)

    def test_missing_or_unready_binding_raises_never_falls_back(self):
        box = get_report_outbox(self.path)
        for changes in ({"user_binding_ready": False}, {"user_guid": None}, {"device_id": None},
                        {"session_id": None}, {"usage_session_id": None}, {"boot_id": None},
                        {"binding_version": None}, {"binding_version": True}):
            conn = connection(**changes)
            for hook in (reportHandle.enqueue_asr_report, reportHandle.enqueue_tts_report):
                with self.subTest(changes=changes, hook=hook.__name__), self.assertRaises(ReportIdentityError):
                    hook(conn, "required", [])
                self.assertTrue(conn.report_queue.empty())
                conn.logger.bind.return_value.error.assert_called()
        self.assertEqual(box.health()["pending"], 0)

    def test_bound_persistence_failure_propagates_from_all_hooks(self):
        box = get_report_outbox(self.path)
        conn = connection()
        with mock.patch.object(report_outbox.sqlite3, "connect",
                               side_effect=sqlite3.OperationalError("disk full")):
            for hook in (reportHandle.enqueue_asr_report, reportHandle.enqueue_tts_report):
                with self.assertRaises(OutboxError):
                    hook(conn, "required", [])
            with self.assertRaises(OutboxError):
                reportHandle.enqueue_tool_report(conn, "lookup", {"key": "value"})
            self.assertFalse(box.health()["healthy"])
        self.assertTrue(conn.report_queue.empty())
        self.assertEqual(conn.logger.bind.return_value.error.call_count, 4)

    def test_tool_call_and_result_are_durable_when_history_is_off(self):
        box = get_report_outbox(self.path)
        conn = connection()
        reportHandle.enqueue_tool_report(conn, "lookup", {"key": "value"}, "result")
        self.assertEqual(box.health()["pending"], 2)
        self.assertTrue(conn.report_queue.empty())

    def test_text_is_not_silently_truncated_and_partial_text_is_explicit(self):
        get_report_outbox(self.path)
        conn = connection()
        text = "long text \u4f60\u597d " * 10000
        message_id = reportHandle.enqueue_tts_report(conn, text, [], text_truncated=True,
                                                    delivery_status="sent_unconfirmed")
        payload = json.loads(self.row(message_id)["payload"])
        self.assertEqual(payload["content"], text)
        self.assertTrue(payload["textTruncated"])
        self.assertEqual(payload["deliveryStatus"], "sent_unconfirmed")
        with self.assertRaises(ValueError):
            reportHandle.enqueue_tts_report(conn, "text", [], delivery_status="played")

    def test_distinct_events_have_distinct_ids_even_with_same_text_and_time(self):
        box = get_report_outbox(self.path)
        conn = connection(turn_id=None, role_id=None)
        first = reportHandle.enqueue_report(conn, 1, "repeated", None, 123)
        second = reportHandle.enqueue_report(conn, 1, "repeated", None, 123)
        self.assertNotEqual(first, second)
        self.assertTrue(json.loads(self.row(first)["payload"])["turnId"])
        self.assertEqual(box.health()["pending"], 2)

    def test_legacy_queue_and_switches_stay_unchanged(self):
        conn = connection(user_binding_required=False, need_bind=False,
                          read_config_from_api=True, report_tts_enable=True,
                          report_asr_enable=True, chat_history_conf=2)
        for hook, chat_type in ((reportHandle.enqueue_asr_report, 1), (reportHandle.enqueue_tts_report, 2)):
            hook(conn, "legacy", [b"audio"])
            item = conn.report_queue.get_nowait()
            self.assertEqual(item[:3], (chat_type, "legacy", [b"audio"]))
            self.assertEqual(item[4:], ("ws-A", "user-A"))
        conn.chat_history_conf = 0
        reportHandle.enqueue_asr_report(conn, "off", [])
        reportHandle.enqueue_tts_report(conn, "off", [])
        self.assertTrue(conn.report_queue.empty())
        self.assertIsNone(report_outbox._singleton)

    def test_text_queue_commits_each_chunk_before_publication_without_tts_consumer(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        real_put = queue.Queue.put
        chunks = []

        def verify_publication(target, item, block=True, timeout=None):
            if item.content_type == ContentType.TEXT and item.content_detail:
                chunks.append(item.content_detail)
                rows = self.sql("SELECT payload FROM report_outbox ORDER BY id")
                self.assertEqual([json.loads(row[0])["content"] for row in rows], chunks)
            return real_put(target, item, block=block, timeout=timeout)

        with mock.patch.object(queue.Queue, "put", new=verify_publication):
            text_queue.put(TTSMessageDTO("sentence-1", SentenceType.FIRST, ContentType.TEXT))
            for text in ("stream chunk", "direct answer", "tool response", "error fallback"):
                text_queue.put(TTSMessageDTO("sentence-1", SentenceType.MIDDLE, ContentType.TEXT, text))
            text_queue.put(TTSMessageDTO("sentence-1", SentenceType.LAST, ContentType.TEXT))
        self.assertEqual(text_queue.qsize(), 6)
        self.assertEqual(box.health()["pending"], 4)
        payloads = [json.loads(row[0]) for row in self.sql("SELECT payload FROM report_outbox")]
        self.assertTrue(all(p["deliveryStatus"] == "generated" for p in payloads))
        self.assertTrue(all(p["turnId"] == "sentence-1" for p in payloads))
        self.assertTrue(all(p["audioBase64"] is None for p in payloads))

    def test_text_queue_keeps_first_snapshot_after_connection_closes(self):
        get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        text_queue.put(TTSMessageDTO("sentence-1", SentenceType.FIRST, ContentType.TEXT))
        conn.user_binding_ready = False
        conn.user_guid = None
        conn.usage_session_id = None
        conn.role_id = None
        text_queue.put(TTSMessageDTO("sentence-1", SentenceType.MIDDLE, ContentType.TEXT, "late chunk"))
        text_queue.put(TTSMessageDTO("sentence-1", SentenceType.LAST, ContentType.TEXT, "last chunk"))
        for row in self.sql("SELECT payload FROM report_outbox"):
            payload = json.loads(row[0])
            self.assertEqual(payload["userGuid"], "user-A")
            self.assertEqual(payload["usageSessionId"], "usage-A")
            self.assertEqual(payload["roleId"], "role-A")
        with self.assertRaises(ReportIdentityError):
            text_queue.put(TTSMessageDTO("sentence-2", SentenceType.FIRST, ContentType.TEXT))

    def test_text_queue_persistence_failure_never_publishes_text(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        text_queue.put(TTSMessageDTO("sentence-1", SentenceType.FIRST, ContentType.TEXT))
        with mock.patch.object(box, "enqueue", side_effect=OutboxError("disk full")):
            with self.assertRaises(OutboxError):
                text_queue.put(TTSMessageDTO("sentence-1", SentenceType.MIDDLE, ContentType.TEXT, "blocked"))
        self.assertEqual(text_queue.qsize(), 1)
        self.assertEqual(box.health()["pending"], 0)
        self.assertTrue(conn.client_abort)
        self.assertFalse(conn.user_binding_ready)

    def test_text_queue_post_commit_publication_retry_uses_same_event(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        text_queue.put(TTSMessageDTO("sentence-1", SentenceType.FIRST, ContentType.TEXT))
        chunk = TTSMessageDTO("sentence-1", SentenceType.MIDDLE, ContentType.TEXT, "durable before publish")
        with mock.patch.object(queue.Queue, "put", side_effect=queue.Full), \
                mock.patch.object(reportHandle.time, "time", return_value=100):
            with self.assertRaises(queue.Full):
                text_queue.put(chunk, block=False)
        first_payload = json.loads(self.sql("SELECT payload FROM report_outbox")[0][0])
        self.assertEqual(box.health()["pending"], 1)
        conn.user_binding_ready = False
        with mock.patch.object(reportHandle.time, "time", return_value=200):
            text_queue.put(chunk, block=False)
        self.assertEqual(text_queue.qsize(), 2)
        self.assertEqual(box.health()["pending"], 1)
        self.assertEqual(json.loads(self.sql("SELECT payload FROM report_outbox")[0][0]), first_payload)

    def test_text_queue_same_text_in_distinct_chunks_has_distinct_ids(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        for _ in range(2):
            text_queue.put(TTSMessageDTO("sentence-1", SentenceType.MIDDLE, ContentType.TEXT, "repeat"))
        self.assertEqual(box.health()["pending"], 2)
        payloads = [json.loads(row[0]) for row in self.sql("SELECT payload FROM report_outbox")]
        self.assertNotEqual(payloads[0]["messageId"], payloads[1]["messageId"])
        self.assertEqual(payloads[0]["turnId"], payloads[1]["turnId"])

    def test_text_queue_first_text_fallback_and_file_markers(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        text_queue.put(TTSMessageDTO("fallback-1", SentenceType.MIDDLE, ContentType.TEXT, "one sentence"))
        text_queue.put(TTSMessageDTO("fallback-1", SentenceType.MIDDLE, ContentType.FILE,
                                    "file label", "audio.wav"))
        text_queue.put(TTSMessageDTO("fallback-1", SentenceType.MIDDLE, ContentType.TEXT, ""))
        self.assertEqual(box.health()["pending"], 1)
        payload = json.loads(self.sql("SELECT payload FROM report_outbox")[0][0])
        self.assertEqual(payload["turnId"], "fallback-1")
        self.assertEqual(payload["content"], "one sentence")

    def test_text_queue_legacy_does_not_create_outbox(self):
        conn = connection(user_binding_required=False)
        text_queue = TranscriptTextQueue(lambda: conn)
        text_queue.put(TTSMessageDTO("legacy", SentenceType.MIDDLE, ContentType.TEXT, "legacy text"))
        self.assertEqual(text_queue.qsize(), 1)
        self.assertIsNone(report_outbox._singleton)

    def test_text_queue_concurrent_chunks_preserve_all_events(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        text_queue.put(TTSMessageDTO("sentence-1", SentenceType.FIRST, ContentType.TEXT))

        def put(index):
            text_queue.put(TTSMessageDTO("sentence-1", SentenceType.MIDDLE, ContentType.TEXT, str(index)))

        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(put, range(20)))
        self.assertEqual(box.health()["pending"], 20)
        self.assertEqual(text_queue.qsize(), 21)

    def test_text_queue_snapshot_capture_failure_closes_gate_before_publication(self):
        conn = connection(user_binding_ready=False)
        text_queue = TranscriptTextQueue(lambda: conn)
        with self.assertRaises(ReportIdentityError):
            text_queue.put(TTSMessageDTO("unbound", SentenceType.FIRST, ContentType.TEXT))
        self.assertTrue(text_queue.empty())
        self.assertTrue(conn.client_abort)
        self.assertFalse(conn.user_binding_ready)
        self.assertIsNone(report_outbox._singleton)

    def test_text_queue_capacity_failure_preserves_oldest_snapshot(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        for index in range(64):
            text_queue.put(TTSMessageDTO(f"sentence-{index}", SentenceType.FIRST, ContentType.TEXT))
        with self.assertRaisesRegex(RuntimeError, "unfinished"):
            text_queue.put(TTSMessageDTO("overflow", SentenceType.FIRST, ContentType.TEXT))
        self.assertEqual(text_queue.qsize(), 64)
        self.assertTrue(conn.client_abort)
        self.assertFalse(conn.user_binding_ready)
        conn.user_guid = None
        text_queue.put(TTSMessageDTO("sentence-0", SentenceType.MIDDLE, ContentType.TEXT, "oldest producer"))
        self.assertEqual(box.health()["pending"], 1)
        payload = json.loads(self.sql("SELECT payload FROM report_outbox")[0][0])
        self.assertEqual(payload["userGuid"], "user-A")
        self.assertEqual(payload["turnId"], "sentence-0")

    def test_text_queue_last_frees_sentence_capacity(self):
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        for index in range(64):
            text_queue.put(TTSMessageDTO(f"sentence-{index}", SentenceType.FIRST, ContentType.TEXT))
        text_queue.put(TTSMessageDTO("sentence-0", SentenceType.LAST, ContentType.TEXT))
        text_queue.put(TTSMessageDTO("replacement", SentenceType.FIRST, ContentType.TEXT))
        self.assertEqual(text_queue.qsize(), 66)
        self.assertTrue(conn.user_binding_ready)
        self.assertIsNone(report_outbox._singleton)

    def test_text_queue_last_dto_retry_retains_identity_after_cache_cleanup(self):
        box = get_report_outbox(self.path)
        conn = connection()
        text_queue = TranscriptTextQueue(lambda: conn)
        text_queue.put(TTSMessageDTO("sentence-1", SentenceType.FIRST, ContentType.TEXT))
        last = TTSMessageDTO("sentence-1", SentenceType.LAST, ContentType.TEXT, "last chunk")
        with mock.patch.object(queue.Queue, "put", side_effect=queue.Full):
            with self.assertRaises(queue.Full):
                text_queue.put(last, block=False)
        original = self.sql("SELECT payload FROM report_outbox")[0][0]
        conn.user_binding_ready = False
        conn.user_guid = None
        text_queue.put(last)
        self.assertEqual(text_queue.qsize(), 2)
        self.assertEqual(box.health()["pending"], 1)
        self.assertEqual(self.sql("SELECT payload FROM report_outbox")[0][0], original)


if __name__ == "__main__":
    unittest.main()
