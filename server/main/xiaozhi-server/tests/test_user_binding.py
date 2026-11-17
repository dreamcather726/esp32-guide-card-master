import ast
import asyncio
import json
from pathlib import Path
import queue
import sys
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from core.utils.user_binding import UserBindingSession, UserBindingError, personalize_prompt, redact_headers
from core.utils.transcript_queue import TranscriptTextQueue
from core.providers.tts.dto.dto import TTSMessageDTO, SentenceType, ContentType


def credentials():
    return dict(userGUID="user-a", bootID="boot-a", bindingVersion=1, bindingToken="t" * 64)


def resolved():
    return dict(userGUID="user-a", bootID="boot-a", bindingVersion=1,
                usageSessionID="usage-a", agentId="agent-a", profileVersion=1,
                profile={"nickname": "Alice"})


class BindingTests(unittest.IsolatedAsyncioTestCase):
    async def test_verified_identity(self):
        binding = UserBindingSession("mac-a", credentials())
        resolver = AsyncMock(return_value=resolved())
        result = await binding.verify(resolver)
        self.assertEqual(result["usageSessionID"], "usage-a")
        self.assertEqual(resolver.call_args.args[0]["macAddress"], "mac-a")

    async def test_reject_mismatched_identity(self):
        for field, value in (("userGUID", "other"), ("bootID", "old"),
                             ("bindingVersion", 2), ("usageSessionID", None), ("agentId", None)):
            response = resolved()
            response[field] = value
            with self.subTest(field=field), self.assertRaises(UserBindingError):
                await UserBindingSession("mac-a", credentials()).verify(AsyncMock(return_value=response))

    async def test_session_and_profile_immutable_until_reconnect(self):
        for field in ("usageSessionID", "agentId", "profileVersion"):
            binding = UserBindingSession("mac-a", credentials())
            await binding.verify(AsyncMock(return_value=resolved()))
            changed = resolved()
            changed[field] = "changed"
            with self.assertRaises(UserBindingError):
                await binding.verify(AsyncMock(return_value=changed))

    async def test_revocation_not_accepted_from_cached_success(self):
        binding = UserBindingSession("mac-a", credentials())
        await binding.verify(AsyncMock(return_value=resolved()))
        with self.assertRaises(RuntimeError):
            await binding.verify(AsyncMock(side_effect=RuntimeError("revoked")))

    def test_version_and_missing_token(self):
        for version in (True, 0, -1, 1.5, "1", None, 2**54):
            payload = credentials()
            payload["bindingVersion"] = version
            with self.assertRaises(UserBindingError):
                UserBindingSession("mac-a", payload)
        payload = credentials()
        del payload["bindingToken"]
        with self.assertRaises(UserBindingError):
            UserBindingSession("mac-a", payload)

    def test_profile_is_connection_scoped_data(self):
        base = "Shared role prompt"
        prompt_a = personalize_prompt(base, {"nickname": "A", "admin": "secret"})
        prompt_b = personalize_prompt(base, {"nickname": "B"})
        self.assertNotEqual(prompt_a, prompt_b)
        self.assertNotIn("secret", prompt_a)
        self.assertEqual(base, "Shared role prompt")
        self.assertEqual(personalize_prompt(base, None), base)

    def test_logs_redact_credentials(self):
        result = redact_headers({"Authorization": "secret", "Binding-Token": "secret",
                                 "User-Guid": "private", "device-id": "mac"})
        self.assertNotIn("secret", json.dumps(result))
        self.assertEqual(result["device-id"], "mac")

    def test_profile_relationship_and_usage_are_not_ai_role_instructions(self):
        base = "Shared role prompt"
        metadata = {"relationship": "wife", "usageCount": 3}
        self.assertEqual(personalize_prompt(base, metadata), base)
        self.assertEqual(personalize_prompt(base, dict(metadata, nickname="Alice")),
                         personalize_prompt(base, {"nickname": "Alice"}))


class TranscriptQueueTests(unittest.TestCase):
    def setUp(self):
        self.conn = SimpleNamespace(user_binding_required=True, user_binding_ready=True,
            device_id="mac", user_guid="u", session_id="s", usage_session_id="usage",
            boot_id="boot", binding_version=1, role_id="role", turn_id="turn")
        self.queue = TranscriptTextQueue(lambda: self.conn)

    def test_text_is_committed_before_queue_and_survives_close(self):
        self.queue.put(TTSMessageDTO("sentence", SentenceType.FIRST, ContentType.ACTION))
        self.conn.user_binding_ready = False
        self.conn.user_guid = "other"
        with patch("core.utils.transcript_queue.enqueue_tts_report") as save:
            self.queue.put(TTSMessageDTO("sentence", SentenceType.MIDDLE, ContentType.TEXT, "hello"))
            identity = save.call_args.kwargs["identity"]
            self.assertEqual(identity.user_guid, "u")
            self.assertEqual(identity.turn_id, "sentence")
            self.assertEqual(save.call_args.kwargs["delivery_status"], "generated")

    def test_storage_failure_stops_playback_enqueue(self):
        with patch("core.utils.transcript_queue.enqueue_tts_report", side_effect=OSError("full")):
            with self.assertRaises(OSError):
                self.queue.put(TTSMessageDTO("s", SentenceType.MIDDLE, ContentType.TEXT, "hello"))
        self.assertTrue(self.queue.empty())
        self.assertTrue(self.conn.client_abort)
        self.assertFalse(self.conn.user_binding_ready)

    def test_chunk_order_and_distinct_ids(self):
        with patch("core.utils.transcript_queue.enqueue_tts_report") as save:
            for text in ("one", "two", "three"):
                self.queue.put(TTSMessageDTO("s", SentenceType.MIDDLE, ContentType.TEXT, text))
        times = [c.kwargs["report_time"] for c in save.call_args_list]
        self.assertEqual(sorted(set(times)), times)
        self.assertEqual(len({c.kwargs["message_id"] for c in save.call_args_list}), 3)

    def test_legacy_queue_unchanged(self):
        self.conn.user_binding_required = False
        with patch("core.utils.transcript_queue.enqueue_tts_report") as save:
            item = TTSMessageDTO("s", SentenceType.MIDDLE, ContentType.TEXT, "hello")
            self.queue.put(item)
            self.assertIs(self.queue.get_nowait(), item)
            save.assert_not_called()


def connection_methods(*names):
    # Exercise production gate methods without loading GPU/audio model dependencies.
    tree = ast.parse((ROOT / "core/connection.py").read_text(encoding="utf-8"))
    cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "ConnectionHandler")
    methods = [n for n in cls.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name in names]
    namespace = {"json": json, "asyncio": asyncio, "time": __import__("time"), "TAG": "test"}
    exec(compile(ast.Module(body=methods, type_ignores=[]), "connection-gates", "exec"), namespace)
    return namespace


class ConnectionGateTests(unittest.IsolatedAsyncioTestCase):
    async def test_hello_bypasses_initialization_wait_but_audio_does_not(self):
        code = connection_methods("_route_message")
        code["handleTextMessage"] = AsyncMock()
        conn = SimpleNamespace(hello_received=asyncio.Event(), user_binding_required=True,
            user_binding_ready=False, bind_completed_event=asyncio.Event(), asr_audio_queue=queue.Queue())
        await code["_route_message"](conn, '{"type":"hello"}')
        code["handleTextMessage"].assert_awaited_once()
        await code["_route_message"](conn, b"opus")
        self.assertTrue(conn.asr_audio_queue.empty())

    async def test_every_turn_checks_live_binding(self):
        code = connection_methods("ensure_user_binding")
        conn = SimpleNamespace(user_binding_required=True, user_binding_ready=True,
            stop_event=threading.Event(), _transcript_storage_ready=lambda: True,
            user_binding=SimpleNamespace(verify=AsyncMock(side_effect=ValueError("revoked"))),
            reject_user_binding=AsyncMock())
        code["resolve_user_binding"] = AsyncMock()
        self.assertFalse(await code["ensure_user_binding"](conn, revalidate=True))
        conn.reject_user_binding.assert_awaited_once()


if __name__ == "__main__":
    unittest.main()
