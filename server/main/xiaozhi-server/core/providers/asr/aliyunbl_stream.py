import asyncio
import json
import time
import uuid
from collections import deque
from dataclasses import dataclass, field

import opuslib_next
import websockets

from config.logger import setup_logging
from core.providers.asr.base import ASRProviderBase
from core.providers.asr.dto.dto import InterfaceType

TAG = __name__
logger = setup_logging()


@dataclass
class RecognitionSession:
    conn: object
    manual: bool
    audio: list
    task_id: str = field(default_factory=lambda: uuid.uuid4().hex)
    started: float = field(default_factory=time.monotonic)
    pending: deque = field(default_factory=deque)
    sentences: dict = field(default_factory=dict)
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    ws: object = None
    task: object = None
    decoder: object = None
    ready: bool = False
    stop_at: float = 0
    finish_sent: bool = False
    submitted: bool = False
    sent_frames: int = 0


class ASRProvider(ASRProviderBase):
    def __init__(self, config, delete_audio_file):
        super().__init__()
        self.interface_type = InterfaceType.STREAM
        self.config = config
        self.text = ""
        self._session = None
        self.api_key = config.get("api_key")
        self.model = config.get("model", "paraformer-realtime-v2")
        self.sample_rate = int(config.get("sample_rate", 16000))
        self.format = config.get("format", "pcm")
        self.ws_url = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
        self.output_dir = config.get("output_dir", "./audio_output")
        self.delete_audio_file = delete_audio_file
        self.connect_timeout = float(config.get("connect_timeout", 5))
        self.ready_timeout = float(config.get("ready_timeout", 10))
        self.result_timeout = float(config.get("result_timeout", 12))
        self.max_duration = float(config.get("max_duration", 180))
        self.max_pending_frames = int(config.get("max_pending_frames", 250))

    def _log(self, session, event, **values):
        detail = " ".join(f"{key}={value}" for key, value in values.items())
        logger.bind(tag=TAG).info(
            f"[ASR_DIAG] event={event} device={getattr(session.conn, 'device_id', '-')} "
            f"turn={getattr(session.conn, 'turn_id', '-')} task={session.task_id} {detail}"
        )

    async def start_listening(self, conn):
        await self._cleanup()
        self.text = ""
        conn.reset_audio_states()

    async def receive_audio(self, conn, audio, audio_have_voice):
        session = self._session
        if session is not None and session.stop_at:
            return
        await super().receive_audio(conn, audio, audio_have_voice)
        try:
            if session is None:
                if not audio_have_voice and conn.client_listen_mode != "manual":
                    return
                session = await self._start_recognition(conn)
            else:
                session.audio.append(audio)
                session.pending.append(audio)
            if session is not self._session:
                return
            if len(session.pending) > self.max_pending_frames:
                raise RuntimeError("pending_audio_limit")
            if time.monotonic() - session.started > self.max_duration:
                raise RuntimeError("recognition_duration_limit")
            await self._flush_pending(session)
            if not session.manual and conn.client_voice_stop:
                await self._send_stop_request()
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            session = session or self._session
            if session is not None:
                self._log(session, "audio_error", error=type(exc).__name__)
                await self._fail(session, "audio_transport_error")

    async def _start_recognition(self, conn):
        session = RecognitionSession(conn, conn.client_listen_mode == "manual", conn.asr_audio.copy())
        session.pending.extend(session.audio)
        session.decoder = opuslib_next.Decoder(self.sample_rate, 1)
        self.text = ""
        self._session = session
        self._log(session, "connect")
        session.ws = await asyncio.wait_for(websockets.connect(
            self.ws_url, additional_headers={"Authorization": f"Bearer {self.api_key}"},
            max_size=1024 * 1024, ping_interval=20, ping_timeout=20, close_timeout=2,
        ), timeout=self.connect_timeout)
        if session is not self._session:
            await self._cleanup(session)
            return session
        await asyncio.wait_for(session.ws.send(json.dumps(self._build_run_task_message(session))),
                               timeout=self.connect_timeout)
        session.task = asyncio.create_task(self._forward_results(session))
        return session

    def _build_run_task_message(self, session):
        parameters = {
            "format": self.format,
            "sample_rate": self.sample_rate,
            "max_sentence_silence": 6000 if session.manual else int(self.config.get("max_sentence_silence") or 200),
            "disfluency_removal_enabled": self.config.get("disfluency_removal_enabled", False),
            "semantic_punctuation_enabled": self.config.get("semantic_punctuation_enabled", False),
            "multi_threshold_mode_enabled": self.config.get("multi_threshold_mode_enabled", False),
            "punctuation_prediction_enabled": self.config.get("punctuation_prediction_enabled", True),
            "inverse_text_normalization_enabled": self.config.get("inverse_text_normalization_enabled", True),
        }
        if self.model.lower().endswith("v2") and self.config.get("vocabulary_id"):
            parameters["vocabulary_id"] = self.config["vocabulary_id"]
        if self.config.get("language_hints"):
            parameters["language_hints"] = self.config["language_hints"]
        return {
            "header": {"action": "run-task", "task_id": session.task_id, "streaming": "duplex"},
            "payload": {"task_group": "audio", "task": "asr", "function": "recognition",
                        "model": self.model, "parameters": parameters, "input": {}},
        }

    async def _flush_pending(self, session):
        # Frames and finish-task share one lock, preserving wire order at readiness.
        async with session.send_lock:
            if session is not self._session or not session.ready or session.finish_sent:
                return
            while session.pending:
                audio = session.pending.popleft()
                pcm = audio if session.conn.audio_format == "pcm" else session.decoder.decode(audio, 960)
                await asyncio.wait_for(session.ws.send(pcm), timeout=self.connect_timeout)
                session.sent_frames += 1
            if session.stop_at:
                message = {"header": {"action": "finish-task", "task_id": session.task_id,
                                      "streaming": "duplex"}, "payload": {"input": {}}}
                await asyncio.wait_for(session.ws.send(json.dumps(message)), timeout=self.connect_timeout)
                session.finish_sent = True
                self._log(session, "finish_sent", frames=session.sent_frames)

    async def _send_stop_request(self):
        session = self._session
        if session is None:
            return
        if not session.stop_at:
            session.stop_at = time.monotonic()
        try:
            await self._flush_pending(session)
        except Exception as exc:
            self._log(session, "finish_error", error=type(exc).__name__)
            await self._fail(session, "finish_transport_error")

    async def _forward_results(self, session):
        reason = "connection_closed"
        try:
            while session is self._session and not session.conn.stop_event.is_set():
                now = time.monotonic()
                deadline = session.started + self.max_duration
                if not session.ready:
                    deadline = min(deadline, session.started + self.ready_timeout)
                if session.stop_at:
                    deadline = min(deadline, session.stop_at + self.result_timeout)
                if now >= deadline:
                    reason = "result_timeout" if session.stop_at else "ready_or_duration_timeout"
                    break
                try:
                    response = await asyncio.wait_for(session.ws.recv(), timeout=min(1, deadline - now))
                except asyncio.TimeoutError:
                    continue
                result = json.loads(response)
                header = result.get("header", {})
                if header.get("task_id", session.task_id) != session.task_id:
                    continue
                event = header.get("event")
                if event == "task-started":
                    session.ready = True
                    self._log(session, "ready", elapsed_ms=round((time.monotonic() - session.started) * 1000),
                              buffered=len(session.pending))
                    await self._flush_pending(session)
                elif event == "result-generated":
                    sentence = result.get("payload", {}).get("output", {}).get("sentence", {})
                    if sentence.get("sentence_end") and sentence.get("end_time") is not None:
                        key = (sentence.get("sentence_id"), sentence.get("begin_time"), sentence["end_time"])
                        session.sentences[key] = sentence.get("text", "")
                        if not session.manual:
                            await self._send_stop_request()
                elif event == "task-finished":
                    # All final segments are now available, regardless of stop timing.
                    await self._submit(session)
                    return
                elif event == "task-failed":
                    reason = "upstream_task_failed"
                    self._log(session, reason, code=header.get("error_code", "unknown"))
                    break
            if session is self._session and not session.conn.stop_event.is_set():
                await self._fail(session, reason)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self._log(session, "receive_error", error=type(exc).__name__)
            await self._fail(session, "result_transport_error")
        finally:
            await self._cleanup(session)

    async def _submit(self, session):
        if session is not self._session or session.submitted:
            return
        session.submitted = True
        self.text = "".join(session.sentences.values()).strip()
        self._log(session, "final", chars=len(self.text),
                  elapsed_ms=round((time.monotonic() - session.started) * 1000))
        if self.text:
            await self.handle_voice_stop(session.conn, session.audio.copy())
        else:
            await self._notify_failure(session, "empty_result")

    async def _notify_failure(self, session, reason):
        if session is not self._session or session.conn.stop_event.is_set():
            return
        self._log(session, "failed", reason=reason)
        # End the device's waiting state without passing error text to the LLM.
        await session.conn.websocket.send(json.dumps({"type": "stt", "session_id": session.conn.session_id,
            "text": "语音识别未完成，请再试一次。"}, ensure_ascii=False))
        await session.conn.websocket.send(json.dumps({"type": "tts", "state": "stop",
            "session_id": session.conn.session_id}))
        session.conn.clearSpeakStatus()

    async def _fail(self, session, reason):
        try:
            await self._notify_failure(session, reason)
        finally:
            await self._cleanup(session)

    async def _cleanup(self, session=None):
        session = session or self._session
        if session is None:
            return
        # Detach before any await; an old cleanup must never clear a new session.
        if self._session is session:
            self._session = None
            self.text = ""
            session.conn.reset_audio_states()
        task, ws = session.task, session.ws
        session.task = session.ws = None
        if task and task is not asyncio.current_task() and not task.done():
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        if ws is not None:
            try:
                await asyncio.wait_for(ws.close(), timeout=2)
            except Exception as exc:
                self._log(session, "close_error", error=type(exc).__name__)

    async def speech_to_text(self, opus_data, session_id, audio_format="opus", artifacts=None):
        text, self.text = self.text, ""
        return text, None

    async def close(self):
        await self._cleanup()
