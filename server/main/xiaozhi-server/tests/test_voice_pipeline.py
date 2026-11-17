import ast
import asyncio
from collections import deque
from dataclasses import dataclass, field
import json
from pathlib import Path
import queue
import sys
import threading
import time
import types
import unittest
import uuid
from unittest.mock import AsyncMock, Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from core.utils.asr_queue import AudioQueueBarrier, drain_audio_input


def extract(path, names, context, class_name=None):
    tree = ast.parse((ROOT / path).read_text(encoding="utf-8-sig"))
    nodes = tree.body
    if class_name:
        nodes = next(n for n in nodes if getattr(n, "name", None) == class_name).body
    nodes = [n for n in nodes if getattr(n, "name", None) in names]
    assert len(nodes) == len(names)
    context = dict(context, __name__=__name__)
    exec(compile(ast.Module(body=nodes, type_ignores=[]), path, "exec"), context)
    return context


class Log:
    def bind(self, **kwargs):
        return self

    def info(self, *args):
        pass

    error = debug = warning = info


class FakeWS:
    def __init__(self):
        self.incoming = asyncio.Queue()
        self.sent = []
        self.closed = 0

    async def recv(self):
        return await self.incoming.get()

    async def send(self, value):
        self.sent.append(value)
        await asyncio.sleep(0)

    async def close(self, **kwargs):
        self.closed += 1

    def event(self, event, sentence=None):
        self.incoming.put_nowait(json.dumps({"header": {"event": event},
            "payload": {"output": {"sentence": sentence or {}}}}))


class ASRBase:
    async def receive_audio(self, conn, audio, voiced):
        conn.asr_audio.append(audio)

    async def handle_voice_stop(self, conn, audio):
        text, _ = await self.speech_to_text(audio, conn.session_id, conn.audio_format)
        conn.submitted.append(text)


def make_conn():
    conn = types.SimpleNamespace(client_listen_mode="manual", client_voice_stop=False,
        stop_event=threading.Event(), asr_audio=[], audio_format="opus", device_id="fixture",
        turn_id="turn", session_id="session", websocket=FakeWS(), submitted=[], resets=0)

    def reset():
        conn.asr_audio.clear()
        conn.client_voice_stop = False
        conn.resets += 1

    conn.reset_audio_states = reset
    conn.clearSpeakStatus = Mock()
    return conn


def asr_types():
    return extract("core/providers/asr/aliyunbl_stream.py", {"RecognitionSession", "ASRProvider"},
        dict(asyncio=asyncio, json=json, time=time, uuid=uuid, deque=deque, dataclass=dataclass,
             field=field, logger=Log(), TAG="test", ASRProviderBase=ASRBase,
             InterfaceType=types.SimpleNamespace(STREAM="stream"),
             opuslib_next=types.SimpleNamespace(Decoder=lambda *a: types.SimpleNamespace(decode=lambda b, n: b)),
             websockets=types.SimpleNamespace(connect=AsyncMock(return_value=FakeWS()))))


def final(text, end=900):
    return dict(text=text, sentence_end=True, end_time=end)


class ASRTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.ns = asr_types()
        self.provider = self.ns["ASRProvider"]({}, True)
        self.conn = make_conn()

    def session(self, ready=True):
        s = self.ns["RecognitionSession"](self.conn, True, [b"a"])
        s.ws = FakeWS()
        s.decoder = types.SimpleNamespace(decode=lambda b, n: b)
        s.ready = ready
        self.provider._session = s
        return s

    async def start_forwarder(self, session):
        session.task = asyncio.create_task(self.provider._forward_results(session))
        await asyncio.sleep(0)
        return session.task

    async def test_final_before_stop_is_submitted_once_at_finish(self):
        s = self.session()
        ws = s.ws
        task = await self.start_forwarder(s)
        ws.event("result-generated", final("FIRST"))
        for _ in range(10):
            await asyncio.sleep(0)
        self.assertEqual(self.conn.submitted, [])
        await self.provider._send_stop_request()
        ws.event("task-finished")
        await asyncio.wait_for(task, 1)
        self.assertEqual(self.conn.submitted, ["FIRST"])
        self.assertEqual(self.provider.text, "")
        self.assertIsNone(self.provider._session)

    async def test_multiple_final_segments_after_stop_and_duplicates(self):
        s = self.session()
        ws = s.ws
        await self.provider._send_stop_request()
        task = await self.start_forwarder(s)
        ws.event("result-generated", final("FIRST"))
        ws.event("result-generated", final("FIRST"))
        ws.event("result-generated", final("SECOND", 1800))
        ws.event("task-finished")
        await asyncio.wait_for(task, 1)
        self.assertEqual(self.conn.submitted, ["FIRSTSECOND"])

    async def test_two_turns_never_share_text(self):
        for text in ("FIRST", "SECOND"):
            s = self.session()
            ws = s.ws
            task = await self.start_forwarder(s)
            ws.event("result-generated", final(text))
            ws.event("task-finished")
            await asyncio.wait_for(task, 1)
        self.assertEqual(self.conn.submitted, ["FIRST", "SECOND"])

    async def test_all_buffered_frames_then_exactly_one_finish(self):
        s = self.session(False)
        ws = s.ws
        frames = [bytes([n]) for n in range(30)]
        s.pending.extend(frames)
        await self.provider._send_stop_request()
        self.assertEqual(ws.sent, [])
        s.ready = True
        await asyncio.gather(self.provider._flush_pending(s), self.provider._send_stop_request())
        self.assertEqual(ws.sent[:-1], frames)
        self.assertEqual(json.loads(ws.sent[-1])["header"]["action"], "finish-task")
        self.assertEqual(len(ws.sent), 31)
        await self.provider._cleanup()

    async def test_cleanup_is_idempotent_and_old_cleanup_preserves_new_session(self):
        old = self.session()
        ws = old.ws
        await asyncio.gather(self.provider._cleanup(old), self.provider._cleanup(old))
        self.assertEqual(ws.closed, 1)
        new = self.session()
        self.provider.text = "NEW"
        await self.provider._cleanup(old)
        self.assertIs(self.provider._session, new)
        self.assertEqual(self.provider.text, "NEW")
        await self.provider._cleanup()

    async def test_ready_timeout_exits_and_resets(self):
        s = self.session(False)
        s.started -= 20
        await self.provider._forward_results(s)
        self.assertIsNone(self.provider._session)
        self.assertEqual(self.conn.submitted, [])
        self.assertEqual(json.loads(self.conn.websocket.sent[-1])["state"], "stop")

    async def test_result_timeout_does_not_submit_partial_question(self):
        s = self.session()
        s.stop_at = time.monotonic() - 20
        s.sentences[1] = "PARTIAL"
        await self.provider._forward_results(s)
        self.assertEqual(self.conn.submitted, [])
        self.assertEqual(self.provider.text, "")

    async def test_manual_mode_keeps_leading_unvoiced_audio(self):
        await self.provider.receive_audio(self.conn, b"leading", False)
        s = self.provider._session
        self.assertIsNotNone(s)
        self.assertEqual(list(s.pending), [b"leading"])
        await self.provider.close()

    async def test_new_start_cancels_previous_unfinished_turn(self):
        s = self.session()
        task = await self.start_forwarder(s)
        s.sentences[1] = "OLD"
        await self.provider.start_listening(self.conn)
        self.assertTrue(task.done())
        self.assertIsNone(self.provider._session)
        self.assertEqual(self.provider.text, "")
        self.assertEqual(self.conn.submitted, [])

    async def test_buffer_overflow_fails_instead_of_dropping_leading_audio(self):
        s = self.session(False)
        self.provider.max_pending_frames = 1
        s.pending.append(b"first")
        await self.provider.receive_audio(self.conn, b"second", True)
        self.assertIsNone(self.provider._session)
        self.assertEqual(self.conn.submitted, [])


def audio_types():
    ns = extract("core/utils/audioRateController.py", {"AudioRateController"},
        dict(asyncio=asyncio, time=time, deque=deque, TAG="test", logger=Log()))
    ns = extract("core/handle/sendAudioHandle.py",
        {"_wait_for_audio_completion", "send_tts_message", "_cancel_tts_watchdog"},
        dict(ns, json=json, PRE_BUFFER_COUNT=5))
    return ns


class AudioDrainTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.ns = audio_types()
        self.rate = self.ns["AudioRateController"](2)
        self.conn = types.SimpleNamespace(config={"tts_audio_completion_timeout": .03},
            audio_rate_controller=self.rate, sentence_id="current", session_id="session",
            client_abort=False, stop_event=threading.Event(), logger=Log(),
            websocket=FakeWS(), clearSpeakStatus=Mock(), clear_queues=Mock())

    async def asyncTearDown(self):
        self.rate.stop_sending()
        if self.rate.pending_send_task:
            await self.rate.pending_send_task

    async def test_progressing_audio_outlives_old_fixed_deadline(self):
        sent = []

        async def send(packet):
            sent.append(packet)

        self.rate.start_sending(send)
        for n in range(80):
            self.rate.add_audio(bytes([n]))
        await self.ns["send_tts_message"](self.conn, "stop")
        self.assertEqual(len(sent), 80)
        self.assertEqual(len(self.rate.queue), 0)
        self.assertEqual(json.loads(self.conn.websocket.sent[-1])["state"], "stop")
        self.assertFalse(self.conn.client_abort)

    async def test_stalled_sender_is_not_success(self):
        block = asyncio.Event()
        self.rate.start_sending(lambda packet: block.wait())
        self.rate.add_audio(b"one")
        result = await self.ns["_wait_for_audio_completion"](self.conn)
        self.assertEqual(result, "stalled")

    async def test_sender_failure_is_not_success(self):
        async def fail(packet):
            raise OSError("disconnected")

        self.rate.start_sending(fail)
        self.rate.add_audio(b"one")
        await asyncio.sleep(.001)
        self.assertEqual(await self.ns["_wait_for_audio_completion"](self.conn), "sender_failed")

    async def test_old_stop_does_not_stop_new_turn(self):
        self.rate.start_sending(lambda packet: asyncio.sleep(.2))
        self.rate.add_audio(b"one")
        old = asyncio.create_task(self.ns["send_tts_message"](self.conn, "stop"))
        await asyncio.sleep(.005)
        self.conn.sentence_id = "new"
        self.rate.reset()
        await old
        self.assertEqual(self.conn.websocket.sent, [])
        self.conn.clearSpeakStatus.assert_not_called()

    async def test_stop_with_stale_explicit_sentence_id_is_ignored(self):
        await self.ns["send_tts_message"](self.conn, "stop", sentence_id="old")
        self.assertEqual(self.conn.websocket.sent, [])


class ReadinessTests(unittest.IsolatedAsyncioTestCase):
    async def test_waits_for_actual_initialization(self):
        ns = extract("core/connection.py", {"wait_for_audio_ready"},
                     dict(asyncio=asyncio, TAG="test"), class_name="ConnectionHandler")
        conn = types.SimpleNamespace(components_ready=False, need_bind=False,
            tts=None, stop_event=threading.Event(), websocket=FakeWS(), logger=Log())

        async def init():
            await asyncio.sleep(.01)
            conn.components_ready = True

        conn.initialization_task = asyncio.create_task(init())
        self.assertTrue(await ns["wait_for_audio_ready"](conn))
        self.assertEqual(conn.websocket.closed, 0)

    async def test_failed_initialization_closes_instead_of_dereferencing_none(self):
        ns = extract("core/connection.py", {"wait_for_audio_ready"},
                     dict(asyncio=asyncio, TAG="test"), class_name="ConnectionHandler")
        conn = types.SimpleNamespace(components_ready=False, need_bind=False, tts=None,
            initialization_task=None, stop_event=threading.Event(), websocket=FakeWS(), logger=Log())
        self.assertFalse(await ns["wait_for_audio_ready"](conn))
        self.assertEqual(conn.websocket.closed, 1)

    async def test_input_barrier_waits_for_inflight_frame(self):
        received = []

        async def handle(conn, frame):
            await asyncio.sleep(.01)
            received.append(frame)

        ns = extract("core/providers/asr/base.py", {"asr_text_priority_thread"},
            dict(asyncio=asyncio, queue=queue, AudioQueueBarrier=AudioQueueBarrier,
                 handleAudioMessage=handle, logger=Log(), TAG="test"), class_name="ASRProviderBase")
        conn = types.SimpleNamespace(stop_event=threading.Event(), asr_audio_queue=queue.Queue(),
                                     loop=asyncio.get_running_loop())
        worker = threading.Thread(target=ns["asr_text_priority_thread"], args=(None, conn))
        worker.start()
        try:
            conn.asr_audio_queue.put(b"first")
            conn.asr_audio_queue.put(b"last")
            await drain_audio_input(conn)
            self.assertEqual(received, [b"first", b"last"])
            self.assertEqual(conn.asr_audio_queue.unfinished_tasks, 0)
        finally:
            conn.stop_event.set()
            await asyncio.to_thread(worker.join, 2)
        self.assertFalse(worker.is_alive())


if __name__ == "__main__":
    unittest.main()
