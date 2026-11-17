import json
import sqlite3
import subprocess
import sys
import tempfile
import ssl
import unittest
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse
from unittest.mock import Mock, patch

from bridge import Backend, BridgeError, COMMAND, Engine, ONLINE, Runtime, Store, credentials, parse_payload, mqtt_tls_context


class FakeBackend:
    def __init__(self):
        self.calls = []
        self.fail = False
        self.command = {"cmd": "bindUser", "cmdID": "issued", "bootID": "boot-1", "userGUID": "user-1",
                        "bindingVersion": 1, "usageSessionID": "session-1", "bindingToken": "x" * 43}

    def post(self, route, payload):
        self.calls.append((route, payload))
        if self.fail:
            raise BridgeError("backend unavailable")
        return {"command": dict(self.command)}


class EngineTest(unittest.TestCase):
    def setUp(self):
        self.now = 1000
        self.api = FakeBackend()
        self.store = Store(":memory:")
        self.sent = []
        self.engine = Engine(self.api, self.store, lambda topic, msg: self.sent.append((topic, msg)), lambda: self.now)
        self.report = {"cmd": "KJ", "cabGuid": "864479086586824s3", "bootID": "boot-1", "groupID": "",
                       "userGUID": "", "bindingState": "unbound", "bindingVersion": 0, "usageSessionID": ""}
        self.cab = self.report["cabGuid"]
        self.topic = COMMAND + self.cab

    def tearDown(self):
        self.store.db.close()

    def receive(self, msg, topic=None, **kwargs):
        self.engine.receive(topic or self.topic, json.dumps(msg), **kwargs)

    def online(self, **kwargs):
        self.receive(dict(self.report, **kwargs), ONLINE)

    def assign(self, **kwargs):
        self.receive(dict(userGUID="user-1", groupID="9", **kwargs))

    def test_legacy_requests_then_dispatches_then_ready(self):
        self.online()
        self.assign()
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "requested")
        self.engine.tick()
        self.assertEqual(self.sent[1][1], {"groupID": "9"})
        self.assertEqual(self.sent[2][1], self.api.command)
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "dispatched")
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="ready")
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "ready")
        self.assertIsNone(self.store.get(self.cab))

    def test_retained_assignments_and_reports_ignored(self):
        self.receive(self.report, ONLINE, retained=True)
        self.receive({"userGUID": "user-1"}, retained=True)
        self.assertFalse(self.api.calls)
        self.assertFalse(self.sent)

    def test_no_fresh_report_rejects_not_queues(self):
        self.assign()
        self.assertIsNone(self.store.get(self.cab))
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "error")
        self.online()
        self.now += 181
        self.assign()
        self.assertIsNone(self.store.get(self.cab))

    def test_unmapped_online_never_allows_assignment(self):
        self.api.fail = True
        with self.assertRaises(BridgeError):
            self.online()
        self.api.fail = False
        self.assign()
        self.assertIsNone(self.store.get(self.cab))

    def test_duplicate_legacy_and_retry_reuse_request_id(self):
        self.online()
        self.assign()
        original = self.store.get(self.cab)["request_id"]
        self.assign()
        self.assertEqual(self.store.get(self.cab)["request_id"], original)
        self.engine.tick()
        self.now += 11
        self.engine.tick()
        calls = [c for route, c in self.api.calls if route == "legacy"]
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], calls[1])

    def test_new_user_supersedes_pending(self):
        self.online()
        self.assign()
        original = self.store.get(self.cab)["request_id"]
        self.receive({"userGUID": "user-2"})
        self.assertNotEqual(self.store.get(self.cab)["request_id"], original)
        self.assertEqual(self.store.get(self.cab)["user"], "user-2")

    def test_same_device_switches_ready_users_and_returns_without_reusing_session(self):
        self.online()
        for version, user in enumerate(("user-1", "user-2", "user-1"), start=1):
            command = dict(self.api.command, userGUID=user, bindingVersion=version,
                           usageSessionID=f"session-{version}", cmdID=f"issued-{version}")
            self.api.command = command
            self.receive({"groupID": "9", "userGUID": user, "cmdID": f"assign-{version}"})
            self.assertEqual(self.sent[-1][1]["bindingAuto"], "requested")
            self.engine.tick()
            self.assertEqual(self.sent[-1][1]["bindingAuto"], "dispatched")
            self.assertIn((self.topic, command), self.sent)
            if version > 1:
                self.online(userGUID="user-1" if version == 2 else "user-2",
                            bindingVersion=version - 1, usageSessionID=f"session-{version - 1}",
                            bindingState="ready")
                self.assertIsNotNone(self.store.get(self.cab))
            self.online(userGUID=user, bindingVersion=version,
                        usageSessionID=f"session-{version}", bindingState="ready")
            self.assertIsNone(self.store.get(self.cab))
            self.assertEqual(self.sent[-1][1]["bindingAuto"], "ready")
        requests = [payload for route, payload in self.api.calls if route == "legacy"]
        self.assertEqual([item["userGUID"] for item in requests], ["user-1", "user-2", "user-1"])
        self.assertEqual(len({item["requestID"] for item in requests}), 3)

    def test_reboot_cancels_intent_and_old_boot_is_ignored(self):
        self.online()
        self.assign()
        self.online(bootID="boot-2")
        self.assertIsNone(self.store.get(self.cab))
        self.online()
        self.assertEqual(self.engine.online[self.cab][0]["bootID"], "boot-2")
        self.engine.tick()
        self.assertFalse(any(route == "legacy" for route, _ in self.api.calls))

    def test_wrong_session_does_not_acknowledge(self):
        self.online()
        self.assign()
        self.engine.tick()
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="old-session", bindingState="ready")
        self.assertIsNotNone(self.store.get(self.cab))

    def test_pending_is_not_ready(self):
        self.online()
        self.assign()
        self.engine.tick()
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="pending")
        self.assertIsNotNone(self.store.get(self.cab))
        self.assertNotEqual(self.sent[-1][1]["bindingAuto"], "ready")

    def test_echo_and_complete_commands_are_ignored(self):
        self.online()
        for key in ("bindingAuto", "deviceExecute", "downSDfile", "bindingToken", "bindingVersion", "usageSessionID"):
            self.receive({key: "value", "userGUID": "user-1"})
        self.receive(self.api.command)
        self.receive({"cmd": "unbindUser", "userGUID": "user-1"})
        self.receive({"groupID": "9"})
        self.assertFalse(self.sent)

    def test_empty_conflicting_and_nonstring_user_rejected(self):
        self.online()
        for msg in ({"userGUID": ""}, {"userGUID": 123}, {"userGUID": "a", "userGuid": "b"}):
            self.receive(msg)
            self.assertEqual(self.sent[-1][1]["bindingAuto"], "error")
        self.assertIsNone(self.store.get(self.cab))

    def test_unsafe_group_and_topic_rejected(self):
        self.online()
        self.receive({"userGUID": "user-1", "groupID": "../9"})
        self.receive({"userGUID": "user-1"}, COMMAND + "+")
        self.assertIsNone(self.store.get(self.cab))

    def test_retry_backoff_and_expiry(self):
        self.online()
        self.assign()
        self.api.fail = True
        self.engine.tick()
        attempts = len(self.api.calls)
        self.engine.tick()
        self.assertEqual(len(self.api.calls), attempts)
        self.now += 181
        self.engine.tick()
        self.assertIsNone(self.store.get(self.cab))
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "error")

    def test_stale_queued_report_ignored(self):
        self.receive(self.report, ONLINE, received=self.now-181)
        self.assertFalse(self.api.calls)

    def test_backend_mismatched_command_not_published(self):
        self.online()
        self.assign()
        self.api.command["bootID"] = "old-boot"
        self.engine.tick()
        self.assertFalse(any(value.get("cmd") == "bindUser" for _, value in self.sent))

    def test_restart_requires_new_live_telemetry(self):
        self.online()
        self.assign()
        restarted = Engine(self.api, self.store, lambda topic, msg: None, lambda: self.now)
        restarted.tick()
        self.assertFalse(any(route == "legacy" for route, _ in self.api.calls))

    def test_database_contains_no_binding_token(self):
        self.online()
        self.assign()
        self.engine.tick()
        dump = "\n".join(self.store.db.iterdump())
        self.assertNotIn(self.api.command["bindingToken"], dump)
        self.assertNotIn("bindingToken", dump)

    def test_ready_same_user_no_session_rotation(self):
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="ready")
        self.assign()
        self.engine.tick()
        self.assertFalse(any(route == "legacy" for route, _ in self.api.calls))

    def test_same_user_group_change_updates_pending_without_new_session(self):
        self.online()
        self.assign()
        original = self.store.get(self.cab)["request_id"]
        self.receive({"userGUID": "user-1", "groupID": "10"})
        self.engine.tick()
        self.assertEqual(self.store.get(self.cab)["request_id"], original)
        self.assertIn((self.topic, {"groupID": "10"}), self.sent)
        self.assertNotIn((self.topic, {"groupID": "9"}), self.sent)

    def test_ready_user_group_update_retries_transport_failure(self):
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="ready")
        publish = self.engine.publish
        self.engine.publish = lambda *_: (_ for _ in ()).throw(BridgeError("offline"))
        self.assign()
        self.assertIsNotNone(self.store.get(self.cab))
        self.engine.publish = publish
        self.now += 11
        self.engine.tick()
        self.assertIn((self.topic, {"groupID": "9"}), self.sent)
        self.assertFalse(any(route == "legacy" for route, _ in self.api.calls))
        self.assertIsNone(self.store.get(self.cab))

    def test_terminal_reply_retried_after_publish_failure(self):
        self.online()
        self.assign()
        self.engine.tick()
        request_id = self.store.get(self.cab)["request_id"]
        publish = self.engine.publish
        self.engine.publish = lambda *_: (_ for _ in ()).throw(BridgeError("offline"))
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="ready")
        self.assertIsNone(self.store.get(self.cab))
        self.assertEqual(self.store.notification(request_id)["state"], "ready")
        self.engine.publish = publish
        self.engine.tick()
        self.assertIsNotNone(self.store.notification(request_id))
        self.now += 10
        self.engine.tick()
        self.assertIsNone(self.store.notification(request_id))
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "ready")

    def test_terminal_survives_new_same_user_assignment_without_rotation(self):
        self.online()
        self.assign(cmdID="producer-1")
        self.engine.tick()
        request_id = self.store.get(self.cab)["request_id"]
        publish = self.engine.publish
        self.engine.publish = lambda *_: (_ for _ in ()).throw(BridgeError("offline"))
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="ready")
        self.engine.publish = publish
        self.assign(cmdID="producer-2")
        self.assertIsNotNone(self.store.notification(request_id))
        self.assertEqual(sum(route == "legacy" for route, _ in self.api.calls), 1)
        self.now += 10
        self.engine.tick()
        ready = [msg for _, msg in self.sent if msg.get("bindingAuto") == "ready"]
        self.assertEqual(len(ready), 2)
        self.assertIn(request_id, [msg["cmdID"] for msg in ready])
        self.assertEqual(self.store.pending_count(), 0)

    def test_terminal_survives_new_user_and_reboot(self):
        self.online()
        self.assign()
        self.engine.tick()
        request_id = self.store.get(self.cab)["request_id"]
        publish = self.engine.publish
        self.engine.publish = lambda *_: (_ for _ in ()).throw(BridgeError("offline"))
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="ready")
        self.engine.publish = publish
        self.receive({"userGUID": "user-2"})
        self.assertEqual(self.store.get(self.cab)["user"], "user-2")
        self.online(bootID="boot-2")
        self.assertEqual(self.store.notification(request_id)["state"], "ready")
        self.now += 10
        self.engine.tick()
        self.assertTrue(any(msg.get("cmdID") == request_id and msg.get("bindingAuto") == "ready"
                            for _, msg in self.sent))

    def test_ready_report_does_not_discard_failed_updated_group(self):
        self.online()
        self.assign(cmdID="producer-1")
        self.engine.tick()
        request_id = self.store.get(self.cab)["request_id"]
        self.receive({"userGUID": "user-1", "groupID": "10", "cmdID": "producer-2"})
        publish = self.engine.publish
        self.engine.publish = lambda *_: (_ for _ in ()).throw(BridgeError("offline"))
        self.engine.tick()
        self.engine.publish = publish
        self.online(userGUID="user-1", groupID="9", bindingVersion=1,
                    usageSessionID="session-1", bindingState="ready")
        self.assertTrue(self.store.get(self.cab)["group_pending"])
        self.assertTrue(self.store.get(self.cab)["binding_ready"])
        self.assertFalse(any(msg.get("bindingAuto") == "ready" for _, msg in self.sent))
        self.now += 10
        self.engine.tick()
        self.assertEqual(self.sent[-2], (self.topic, {"groupID": "10"}))
        self.assertEqual(self.sent[-1][1]["cmdID"], request_id)
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "ready")
        self.assertEqual(sum(msg.get("cmd") == "bindUser" for _, msg in self.sent), 1)
        self.assertIsNone(self.store.get(self.cab))

    def test_delivered_group_is_not_repeated_after_bind_publish_failure(self):
        self.online()
        self.assign()
        publish = self.engine.publish
        def fail_binding(topic, message):
            if message.get("cmd") == "bindUser":
                raise BridgeError("offline")
            publish(topic, message)
        self.engine.publish = fail_binding
        self.engine.tick()
        self.assertFalse(self.store.get(self.cab)["group_pending"])
        self.engine.publish = publish
        self.now += 10
        self.engine.tick()
        self.assertEqual(sum("groupID" in msg for _, msg in self.sent), 1)
        self.assertEqual(sum(msg.get("cmd") == "bindUser" for _, msg in self.sent), 1)

    def test_group_only_retry_does_not_apply_to_another_user(self):
        self.online(userGUID="user-1", bindingVersion=1, usageSessionID="session-1", bindingState="ready")
        publish = self.engine.publish
        self.engine.publish = lambda *_: (_ for _ in ()).throw(BridgeError("offline"))
        self.assign()
        self.engine.publish = publish
        self.online(userGUID="user-2", bindingVersion=2, usageSessionID="session-2", bindingState="ready")
        self.now += 10
        self.engine.tick()
        self.assertFalse(any("groupID" in msg for _, msg in self.sent))
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "error")

    def test_group_only_retry_expires_and_reboot_cancels(self):
        for reboot in (False, True):
            with self.subTest(reboot=reboot):
                self.online(userGUID="user-1", bindingState="ready")
                publish = self.engine.publish
                self.engine.publish = lambda *_: (_ for _ in ()).throw(BridgeError("offline"))
                self.assign()
                self.engine.publish = publish
                if reboot:
                    self.online(bootID="boot-2")
                else:
                    self.now += 181
                    self.engine.tick()
                self.assertIsNone(self.store.get(self.cab))
                self.assertEqual(self.sent[-1][1]["bindingAuto"], "error")
                self.assertFalse(any("groupID" in msg for _, msg in self.sent))

    def test_legacy_without_cmdid_remains_accepted_and_deduplicated_while_pending(self):
        self.online()
        self.assign()
        first = self.store.get(self.cab)["request_id"]
        self.assign()
        self.assertEqual(self.store.get(self.cab)["request_id"], first)
        self.online(bootID="boot-2")
        self.assign()
        self.assertEqual(self.store.get(self.cab)["boot"], "boot-2")
        self.assertNotEqual(self.store.get(self.cab)["request_id"], first)
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM producer_receipt").fetchone()[0], 0)

    def test_atomic_receipt_failures_roll_back_all_assignment_paths(self):
        self.online()
        for mode in ("new", "ready", "update", "replace"):
            with self.subTest(mode=mode):
                self.store.delete(self.cab)
                self.online(userGUID="user-1" if mode == "ready" else "",
                            bindingState="ready" if mode == "ready" else "unbound")
                original = None
                if mode in ("update", "replace"):
                    original = dict(self.store.create(self.cab, "boot-1", "user-1", "9", self.now))
                original_remember = self.store._remember
                def fail_after_receipt(*args):
                    original_remember(*args)
                    raise sqlite3.OperationalError("injected failure")
                with patch.object(self.store, "_remember", side_effect=fail_after_receipt):
                    with self.assertRaises(sqlite3.OperationalError):
                        self.receive({"userGUID": "user-2" if mode == "replace" else "user-1",
                                      "groupID": "10", "cmdID": "fault-" + mode})
                current = self.store.get(self.cab)
                self.assertEqual(dict(current) if current else None, original)
                self.assertIsNone(self.store.receipt(self.cab, "fault-" + mode))
                self.assertFalse(self.store.db.in_transaction)

    def test_producer_cmdid_cannot_be_reused_across_boots(self):
        self.online()
        self.assign(cmdID="customer-1")
        self.online(bootID="boot-2")
        self.assign(cmdID="customer-1")
        self.assertIsNone(self.store.get(self.cab))
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "error")

    def test_producer_cmdid_cannot_change_user_or_group(self):
        self.online()
        self.assign(cmdID="customer-1")
        self.receive({"userGUID": "user-2", "groupID": "9", "cmdID": "customer-1"})
        self.assertEqual(self.store.get(self.cab)["user"], "user-1")
        self.assertEqual(self.sent[-1][1]["bindingAuto"], "error")

    def test_boot_scoped_legacy_is_not_silently_rewritten(self):
        self.online()
        self.assign(bootID="old-boot")
        self.assertIsNone(self.store.get(self.cab))


class StoreRecoveryTest(unittest.TestCase):
    def test_process_exit_rolls_back_intent_and_receipt_together(self):
        script = """
import os, sys
from bridge import Store
store = Store(sys.argv[1])
original = store._remember
def crash(*args):
    if sys.argv[3] == 'after':
        original(*args)
    os._exit(23)
store._remember = crash
if sys.argv[2] == 'update':
    store.update_group('cab', '10', 1001, 'producer-new')
else:
    store.create('cab', 'boot-1', 'user-2', '10', 1001, 'producer-new',
                 binding_ready=sys.argv[2] == 'ready')
"""
        for mode in ("create", "update", "ready"):
            for position in ("before", "after"):
                with self.subTest(mode=mode, position=position), tempfile.TemporaryDirectory() as folder:
                    path = str(Path(folder) / "state.sqlite3")
                    store = Store(path)
                    original = dict(store.create("cab", "boot-1", "user-1", "9", 1000, "producer-old"))
                    store.db.close()
                    result = subprocess.run([sys.executable, "-B", "-c", script, path, mode, position],
                                            cwd=Path(__file__).parent, capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 23, result.stderr.decode(errors="replace"))
                    store = Store(path)
                    try:
                        self.assertEqual(dict(store.get("cab")), original)
                        self.assertIsNone(store.receipt("cab", "producer-new"))
                        self.assertIsNotNone(store.receipt("cab", "producer-old"))
                    finally:
                        store.db.close()

    def test_completion_enqueue_and_removal_are_atomic(self):
        store = Store(":memory:")
        self.addCleanup(store.db.close)
        intent = store.create("cab", "boot", "user", "9", 1000)
        store.db.execute("""CREATE TRIGGER fail_delete BEFORE DELETE ON intent
            BEGIN SELECT RAISE(ABORT, 'injected delete failure'); END""")
        with self.assertRaises(sqlite3.IntegrityError):
            store.complete(intent, "ready", "verified", 1001)
        self.assertIsNotNone(store.get("cab"))
        self.assertIsNone(store.notification(intent["request_id"]))
        store.db.execute("DROP TRIGGER fail_delete")
        store.complete(intent, "ready", "verified", 1001)
        self.assertIsNone(store.get("cab"))
        self.assertEqual(store.notification(intent["request_id"])["state"], "ready")

    def test_old_completion_does_not_delete_new_intent_or_change_terminal_state(self):
        store = Store(":memory:")
        self.addCleanup(store.db.close)
        old = store.create("cab", "boot", "user-1", "9", 1000)
        store.complete(old, "ready", "verified", 1001)
        current = store.create("cab", "boot", "user-2", "10", 1002)
        store.complete(old, "error", "late error", 1003)
        self.assertEqual(store.get("cab")["request_id"], current["request_id"])
        self.assertEqual(store.notification(old["request_id"])["state"], "ready")

    def test_group_delivery_and_verified_binding_survive_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / "state.sqlite3")
            store = Store(path)
            intent = store.create("cab", "boot-1", "user-1", "10", 1000, "producer-1")
            store.dispatched("cab", 1, "session-1")
            store.binding_verified("cab")
            store.db.close()
            store = Store(path)
            try:
                api, sent = FakeBackend(), []
                engine = Engine(api, store, lambda topic, msg: sent.append(msg), lambda: 1001)
                engine.tick()
                self.assertFalse(sent)
                engine.receive(ONLINE, json.dumps({"cmd": "KJ", "cabGuid": "cab", "bootID": "boot-1",
                    "userGUID": "user-1", "bindingVersion": 1, "usageSessionID": "session-1",
                    "bindingState": "ready", "groupID": "9"}))
                self.assertFalse(sent)
                engine.tick()
                self.assertEqual(sent[0], {"groupID": "10"})
                self.assertEqual(sent[1]["bindingAuto"], "ready")
                self.assertEqual(sent[1]["cmdID"], intent["request_id"])
                self.assertFalse(any(route == "legacy" for route, _ in api.calls))
                self.assertEqual(store.pending_count(), 0)
            finally:
                store.db.close()

    def test_terminal_survives_restart_without_telemetry_and_new_active_intent(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / "state.sqlite3")
            store = Store(path)
            intent = store.create("cab", "boot-1", "user-1", "9", 1000, "producer-1")
            store.complete(intent, "ready", "verified", 1001)
            active = store.create("cab", "boot-1", "user-2", None, 1002, "producer-2")
            store.db.close()
            store = Store(path)
            try:
                api, sent = FakeBackend(), []
                engine = Engine(api, store, lambda topic, msg: sent.append(msg), lambda: 1003)
                engine.tick()
                self.assertEqual(sent[0]["cmdID"], intent["request_id"])
                self.assertEqual(sent[0]["bindingAuto"], "ready")
                self.assertEqual(store.get("cab")["request_id"], active["request_id"])
                self.assertEqual(store.pending_count(), 1)
                self.assertFalse(api.calls)
            finally:
                store.db.close()

    def test_notification_batch_is_bounded_and_does_not_rewrite_active_intents(self):
        store = Store(":memory:")
        self.addCleanup(store.db.close)
        for index in range(70):
            intent = store.create("cab", "boot", "user", None, 1000)
            store.complete(intent, "error", "expired", 1001)
        self.assertEqual(len(store.notifications(1001)), 64)
        self.assertEqual(store.pending_count(), 70)
        sent = []
        engine = Engine(FakeBackend(), store, lambda topic, msg: sent.append(msg), lambda: 1001)
        engine.tick()
        self.assertEqual(len(sent), 64)
        self.assertEqual(store.pending_count(), 6)
        engine.tick()
        self.assertEqual(store.pending_count(), 0)

    def test_migrates_old_terminal_and_group_rows_idempotently(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / "state.sqlite3")
            db = sqlite3.connect(path)
            db.execute("""CREATE TABLE intent (
                cab TEXT PRIMARY KEY, boot TEXT NOT NULL, user TEXT NOT NULL,
                group_id TEXT, request_id TEXT NOT NULL, created REAL NOT NULL,
                next_try REAL NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
                version INTEGER, session_id TEXT, terminal_state TEXT, terminal_message TEXT)""")
            for cab, version, state in (("terminal", 1, "ready"), ("group", None, "ready"), ("active", 1, None)):
                db.execute("INSERT INTO intent VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                           (cab, "boot-1", "user-1", "9", cab + "-request", 1000, 1001, 2,
                            version, "session-1", state, "verified" if state else None))
            db.commit()
            db.close()
            for _ in range(2):
                store = Store(path)
                try:
                    self.assertIsNone(store.get("terminal"))
                    self.assertEqual(store.notification("terminal-request")["state"], "ready")
                    self.assertTrue(store.get("group")["binding_ready"])
                    self.assertTrue(store.get("group")["group_pending"])
                    self.assertTrue(store.get("active")["group_pending"])
                    self.assertEqual(store.pending_count(), 3)
                finally:
                    store.db.close()

    def test_migrates_original_intent_schema_without_terminal_columns(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / "state.sqlite3")
            db = sqlite3.connect(path)
            db.execute("""CREATE TABLE intent (
                cab TEXT PRIMARY KEY, boot TEXT NOT NULL, user TEXT NOT NULL,
                group_id TEXT, request_id TEXT NOT NULL, created REAL NOT NULL,
                next_try REAL NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
                version INTEGER, session_id TEXT)""")
            db.execute("INSERT INTO intent VALUES ('cab','boot','user','9','request',1000,1000,0,NULL,NULL)")
            db.commit()
            db.close()
            store = Store(path)
            try:
                self.assertTrue(store.get("cab")["group_pending"])
                self.assertFalse(store.get("cab")["binding_ready"])
            finally:
                store.db.close()


class RuntimeTest(unittest.TestCase):
    def setUp(self):
        self.now = 1000
        self.store = Store(":memory:")
        self.addCleanup(self.store.db.close)
        self.runtime = Runtime({"credentials_url": "https://example.test", "account": "bridge"},
                               FakeBackend(), self.store, clock=lambda: self.now)
        self.addCleanup(self.runtime.disconnect)
        self.clients = []
        data = {"clientId": "bridge", "username": "u", "password": "p"}
        self.credentials = self.enterContext(patch("bridge.credentials", return_value=(data, urlparse("wss://example.test/mqtt"), 2880)))
        self.enterContext(patch("bridge.mqtt_tls_context", return_value=None))
        self.factory = self.enterContext(patch("paho.mqtt.client.Client", side_effect=self.make_client))

    def make_client(self, *args, **kwargs):
        client = Mock()
        client.subscribe.return_value = (0, 1)
        client.publish.return_value.is_published.return_value = True
        client.disconnect.side_effect = lambda: client.on_disconnect(client, None, None, None, None)
        self.clients.append(client)
        return client

    def subscribe(self):
        client = self.runtime.client
        success = SimpleNamespace(is_failure=False)
        client.on_connect(client, None, None, success, None)
        client.on_subscribe(client, None, 1, [success, success], None)

    def test_reconnect_deadline_grows_and_caps_even_with_queued_events(self):
        for delay in (2, 4, 8, 16, 30, 30):
            self.runtime.maintain_connection()
            client = self.runtime.client
            client.on_disconnect(client, None, None, None, None)
            self.runtime.maintain_connection()
            self.assertEqual(self.runtime.next_connect, self.now + delay)
            calls = self.credentials.call_count
            for _ in range(50):
                self.runtime.events.put((COMMAND + "cab", b'{"bindingAuto":"ready"}', False, 1000))
                self.runtime.maintain_connection()
                self.runtime.engine.receive(*self.runtime.events.get_nowait())
            self.assertEqual(self.credentials.call_count, calls)
            self.now += delay
        self.assertEqual(self.credentials.call_count, 6)

    def test_run_loop_backlog_does_not_bypass_reconnect_deadlines(self):
        attempts = []
        original = self.make_client
        def disconnect_immediately(*args, **kwargs):
            attempts.append(self.now)
            client = original(*args, **kwargs)
            client.loop_start.side_effect = lambda: client.on_disconnect(client, None, None, None, None)
            return client
        self.factory.side_effect = disconnect_immediately
        self.runtime.last_health = 1000
        def next_event(timeout):
            self.now += 1
            if self.now >= 1010:
                self.runtime.stopped.set()
            return (COMMAND + "cab", b'{"bindingAuto":"ready"}')
        with patch.object(self.runtime.events, "get", side_effect=next_event), patch("bridge.time.time", return_value=1000):
            self.runtime.run()
        self.assertEqual(attempts, [1000, 1003, 1008])
        self.assertIsNone(self.runtime.client)

    def test_connect_and_credentials_errors_obey_deadline(self):
        for error in (BridgeError("credentials failed"), OSError("connect failed"), ValueError("invalid URL")):
            with self.subTest(error=type(error).__name__):
                self.credentials.side_effect = error
                self.now = max(self.now, self.runtime.next_connect)
                self.runtime.maintain_connection()
                self.assertIsNone(self.runtime.client)
                calls = self.credentials.call_count
                self.runtime.maintain_connection()
                self.assertEqual(self.credentials.call_count, calls)
                self.assertGreater(self.runtime.next_connect, self.now)

    def test_actual_connect_failure_discards_client_and_waits(self):
        original = self.make_client
        def fail_connect(*args, **kwargs):
            client = original(*args, **kwargs)
            client.connect.side_effect = OSError("transport failed")
            return client
        self.factory.side_effect = fail_connect
        self.runtime.maintain_connection()
        self.assertIsNone(self.runtime.client)
        self.clients[-1].loop_stop.assert_called_once()
        self.assertEqual(self.runtime.next_connect, self.now + 2)

    def test_connection_failure_log_does_not_include_exception_secrets(self):
        self.credentials.side_effect = BridgeError("SECRET_PASSWORD_AND_TOKEN")
        with self.assertLogs("mqtt-binding", level="WARNING") as logs:
            self.runtime.maintain_connection()
        self.assertNotIn("SECRET_PASSWORD_AND_TOKEN", "\n".join(logs.output))

    def test_only_stable_subscription_resets_backoff(self):
        self.runtime.reconnect_delay = 16
        self.runtime.maintain_connection()
        self.subscribe()
        self.now += 1
        self.runtime.maintain_connection()
        self.assertEqual(self.runtime.reconnect_delay, 16)
        self.now += 29
        self.runtime.maintain_connection()
        self.assertEqual(self.runtime.reconnect_delay, 2)

    def test_missing_connack_or_suback_times_out(self):
        for connack in (False, True):
            with self.subTest(connack=connack):
                self.now = max(self.now, self.runtime.next_connect)
                self.runtime.maintain_connection()
                if connack:
                    client = self.runtime.client
                    client.on_connect(client, None, None, SimpleNamespace(is_failure=False), None)
                self.now += 30
                self.runtime.maintain_connection()
                self.assertIsNone(self.runtime.client)
                self.assertGreater(self.runtime.next_connect, self.now)

    def test_rejected_connack_suback_and_subscribe_send_request_retry(self):
        for failure in ("connack", "suback", "subscribe"):
            with self.subTest(failure=failure):
                self.now = max(self.now, self.runtime.next_connect)
                self.runtime.maintain_connection()
                client = self.runtime.client
                bad = SimpleNamespace(is_failure=True)
                if failure == "connack":
                    client.on_connect(client, None, None, bad, None)
                elif failure == "suback":
                    client.on_subscribe(client, None, 1, [bad, bad], None)
                else:
                    client.subscribe.return_value = (4, None)
                    client.on_connect(client, None, None, SimpleNamespace(is_failure=False), None)
                self.assertTrue(self.runtime.reconnect_requested.is_set())
                self.runtime.maintain_connection()
                self.assertIsNone(self.runtime.client)

    def test_publish_timeout_discards_queue_and_recreates_client_after_backoff(self):
        self.runtime.maintain_connection()
        self.subscribe()
        old = self.runtime.client
        old.publish.return_value.is_published.return_value = False
        with self.assertRaises(BridgeError):
            self.runtime.publish(COMMAND + "cab", {"groupID": "9"})
        self.assertIsNone(self.runtime.client)
        old.loop_stop.assert_called_once()
        self.assertFalse(self.runtime.connected.is_set())
        with self.assertRaises(BridgeError):
            self.runtime.publish(COMMAND + "cab", {"groupID": "10"})
        self.assertEqual(old.publish.call_count, 1)
        self.runtime.maintain_connection()
        self.runtime.maintain_connection()
        self.assertEqual(len(self.clients), 1)
        self.now += 2
        self.runtime.maintain_connection()
        self.assertIsNot(self.runtime.client, old)
        self.runtime.client.max_queued_messages_set.assert_called_once_with(16)
        self.runtime.client.max_inflight_messages_set.assert_called_once_with(1)
        self.assertFalse(self.factory.call_args.kwargs["reconnect_on_failure"])

    def test_publish_exceptions_and_success_keep_nonretained_qos1_contract(self):
        self.runtime.maintain_connection()
        self.subscribe()
        self.runtime.publish(COMMAND + "cab", {"groupID": "9"})
        self.runtime.client.publish.assert_called_once_with(COMMAND + "cab", '{"groupID":"9"}', qos=1, retain=False)
        for source, error in (("publish", OSError("offline")), ("wait", RuntimeError("no connection")),
                              ("wait", ValueError("queue full"))):
            with self.subTest(source=source, error=type(error).__name__):
                if self.runtime.client is None:
                    self.runtime.maintain_connection()
                    self.now = self.runtime.next_connect
                    self.runtime.maintain_connection()
                    self.subscribe()
                client = self.runtime.client
                target = client.publish if source == "publish" else client.publish.return_value.wait_for_publish
                target.side_effect = error
                with self.assertRaises(BridgeError):
                    self.runtime.publish(COMMAND + "cab", {"groupID": "9"})
                self.assertIsNone(self.runtime.client)
                self.assertTrue(self.runtime.reconnect_requested.is_set())

    def test_refresh_and_old_callbacks_do_not_cause_reconnect_churn(self):
        self.runtime.maintain_connection()
        self.subscribe()
        old = self.runtime.client
        self.now = self.runtime.refresh_at
        self.runtime.maintain_connection()
        current = self.runtime.client
        self.assertIsNot(current, old)
        self.subscribe()
        old.on_disconnect(old, None, None, None, None)
        old.on_subscribe(old, None, 1, [SimpleNamespace(is_failure=True)], None)
        self.assertFalse(self.runtime.reconnect_requested.is_set())
        self.assertTrue(self.runtime.subscribed.is_set())
        self.runtime.maintain_connection()
        self.assertIs(self.runtime.client, current)


class HelpersTest(unittest.TestCase):
    def test_tls_compatibility_preserves_verification(self):
        context = mqtt_tls_context()
        self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(context.check_hostname)
        self.assertEqual(context.minimum_version, ssl.TLSVersion.TLSv1_2)
        names = {cipher['name'] for cipher in context.get_ciphers()}
        self.assertIn('AES256-GCM-SHA384', names)
        self.assertNotIn('ADH-AES256-GCM-SHA384', names)

    def test_json_rejects_ambiguity_and_invalid_payloads(self):
        for payload in ('{"userGUID":"a","userGUID":"b"}', '[]', '{"n":NaN}', 'x'*32769):
            with self.assertRaises(ValueError):
                parse_payload(payload)

    @patch("bridge.post_json")
    def test_credentials_refresh_before_expiry_and_exact_service_id(self, post):
        post.return_value = {"success": True, "clientId": "AB12cd34", "username": "u", "password": "p",
                             "wssUrl": "wss://broker.example:443/mqtt", "expiresIn": 3600}
        _, _, refresh = credentials("https://example.test/credentials", "AB12cd34")
        self.assertEqual(refresh, 2880)
        post.assert_called_once_with("https://example.test/credentials", {"imei": "AB12cd34"})
        post.return_value["clientId"] = "device-imeis3"
        with self.assertRaises(BridgeError):
            credentials("https://example.test", "AB12cd34")

    @patch("bridge.post_json")
    def test_api_rejects_http200_business_errors(self, post):
        post.return_value = {"code": 500, "msg": "secret should not be logged"}
        with self.assertRaisesRegex(BridgeError, "^backend rejected legacy$"):
            Backend("http://localhost/xiaozhi", "test-secret").post("legacy", {})

    def test_durable_intent_survives_restart_without_telemetry(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / "intent.sqlite3")
            store = Store(path)
            row = store.create("cab", "boot", "user", "9", 100)
            store.db.close()
            store = Store(path)
            self.assertEqual(store.get("cab")["request_id"], row["request_id"])
            store.db.close()


if __name__ == "__main__":
    unittest.main()
