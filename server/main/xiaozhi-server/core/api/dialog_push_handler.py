import json
import hmac
import asyncio
import uuid
import re
from aiohttp import web

from core.api.base_handler import BaseHandler
from core.device_connection_registry import get_connection
from core.handle.receiveAudioHandle import startToChat
from core.handle.sendAudioHandle import send_tts_message
from core.providers.tts.dto.dto import ContentType, TTSMessageDTO, SentenceType
from core.utils.auth import AuthToken

TAG = __name__

MAX_PUSH_TEXT_LENGTH = 2000
DEVICE_READY_TIMEOUT_SECONDS = 10
SAFE_AUDIO_FILE_RE = re.compile(r"^[A-Za-z0-9._-]{1,128}$")
NULL_FILE_MARKERS = {"", "null", "none", "nil", "false", "0", "!null", "lnull"}


class DialogPushHandler(BaseHandler):
    def __init__(self, config: dict):
        super().__init__(config)
        self.auth = AuthToken(config["server"]["auth_key"])

    def _json_response(self, data: dict, status: int = 200) -> web.Response:
        response = web.Response(
            text=json.dumps(data, ensure_ascii=False, separators=(",", ":")),
            content_type="application/json",
            status=status,
        )
        self._add_cors_headers(response)
        return response

    def _extract_token(self, request) -> str:
        auth_header = request.headers.get("Authorization", "")
        if auth_header.startswith("Bearer "):
            return auth_header[7:].strip()
        return (
            request.headers.get("X-API-Key")
            or request.headers.get("X-Api-Key")
            or request.headers.get("X-Auth-Token")
            or ""
        ).strip()

    def _verify_request(self, request, device_id: str) -> bool:
        token = self._extract_token(request)
        if not token:
            return False

        is_valid, token_device_id = self.auth.verify_token(token)
        if is_valid and token_device_id == device_id:
            return True

        service_secrets = [
            self.config.get("manager-api", {}).get("secret", ""),
            self.config.get("server", {}).get("auth_key", ""),
        ]
        return any(
            secret and hmac.compare_digest(token, secret)
            for secret in service_secrets
        )

    async def _wait_until_dialogue_ready(self, conn) -> bool:
        deadline = asyncio.get_running_loop().time() + DEVICE_READY_TIMEOUT_SECONDS
        while asyncio.get_running_loop().time() < deadline:
            if (
                getattr(conn, "loop", None) is not None
                and getattr(conn, "llm", None) is not None
                and getattr(conn, "tts", None) is not None
                and getattr(conn, "executor", None) is not None
            ):
                return True
            await asyncio.sleep(0.2)
        return False

    async def _broadcast_text_now(self, conn, text: str) -> None:
        conn.client_abort = True
        conn.clear_queues()
        try:
            await conn.websocket.send(
                json.dumps({"type": "tts", "state": "stop", "session_id": conn.session_id})
            )
        except Exception:
            pass

        conn.client_abort = False
        conn.close_after_chat = False
        conn.sentence_id = uuid.uuid4().hex
        await send_tts_message(conn, "start")
        conn.client_is_speaking = True

        conn.tts.store_tts_text(conn.sentence_id, text)
        conn.tts.tts_text_queue.put(
            TTSMessageDTO(
                sentence_id=conn.sentence_id,
                sentence_type=SentenceType.FIRST,
                content_type=ContentType.ACTION,
            )
        )
        conn.tts.tts_one_sentence(conn, ContentType.TEXT, content_detail=text)
        conn.tts.tts_text_queue.put(
            TTSMessageDTO(
                sentence_id=conn.sentence_id,
                sentence_type=SentenceType.LAST,
                content_type=ContentType.ACTION,
            )
        )

    def _normalize_file_name(self, value) -> str:
        if value is None:
            return ""

        file_value = str(value).strip()
        if not file_value:
            return ""

        compact = file_value.strip().lower()
        if compact in NULL_FILE_MARKERS:
            return ""

        file_value = file_value.replace("\\", "/")
        parts = [part.strip() for part in file_value.split("/") if part.strip()]
        if not parts:
            return ""

        filename = parts[-1]
        if filename.lower() in NULL_FILE_MARKERS:
            return ""
        if filename in (".", "..") or not SAFE_AUDIO_FILE_RE.fullmatch(filename):
            raise ValueError("invalid file name")

        ext = filename.rsplit(".", 1)[-1].lower() if "." in filename else ""
        if ext not in {"wav", "ogg"}:
            raise ValueError("unsupported audio file type")

        return filename

    def _normalize_mode(self, data: dict) -> str:
        mode = str(data.get("mode") or "").strip().lower()
        if not mode:
            direction = str(data.get("direction") or "").strip().lower()
            if direction in {"direct", "introduce_location"}:
                mode = direction
        return mode or "direct"

    async def _push_sd_audio_command(self, conn, filename: str) -> None:
        payload = {
            "type": "custom",
            "payload": {
                "command": "play_sd_audio",
                "file": filename,
            },
            "session_id": conn.session_id,
        }
        await conn.websocket.send(json.dumps(payload, ensure_ascii=False))

    async def _introduce_location(self, conn, text: str) -> None:
        conn.client_abort = True
        conn.clear_queues()
        try:
            await conn.websocket.send(
                json.dumps({"type": "tts", "state": "stop", "session_id": conn.session_id})
            )
        except Exception:
            pass

        conn.client_abort = False
        conn.close_after_chat = False
        prompt = (
            "请根据下面的定位结果，生成一段适合设备立刻播报的中文介绍。"
            "要求自然、简洁、有导游讲解感，不要提到你是大模型，不要输出标题。"
            f"\n定位结果：{text}"
        )
        await startToChat(conn, prompt)

    async def handle_post(self, request):
        try:
            try:
                data = await request.json()
            except Exception:
                return self._json_response(
                    {"success": False, "message": "Invalid JSON body"},
                    status=400,
                )

            device_id = str(
                data.get("deviceId")
                or data.get("device_id")
                or data.get("imei")
                or data.get("IMEI")
                or data.get("clientId")
                or data.get("client_id")
                or ""
            ).strip()
            text = str(
                data.get("text")
                or data.get("content")
                or data.get("message")
                or data.get("msg")
                or ""
            ).strip()
            try:
                filename = self._normalize_file_name(data.get("file"))
            except ValueError as e:
                return self._json_response(
                    {"success": False, "message": str(e)},
                    status=400,
                )
            mode = self._normalize_mode(data)
            source = str(data.get("source") or "external").strip()

            if not device_id:
                return self._json_response(
                    {"success": False, "message": "deviceId is required"},
                    status=400,
                )
            if not text and not filename:
                return self._json_response(
                    {"success": False, "message": "text or file is required"},
                    status=400,
                )
            if len(text) > MAX_PUSH_TEXT_LENGTH:
                return self._json_response(
                    {
                        "success": False,
                        "message": f"text exceeds {MAX_PUSH_TEXT_LENGTH} characters",
                    },
                    status=400,
                )
            if not self._verify_request(request, device_id):
                return self._json_response(
                    {"success": False, "message": "Unauthorized"},
                    status=401,
                )

            conn = await get_connection(device_id)
            if conn is None or conn.websocket is None:
                return self._json_response(
                    {"success": False, "message": "Device is offline"},
                    status=404,
                )

            if not await self._wait_until_dialogue_ready(conn):
                return self._json_response(
                    {"success": False, "message": "Device dialogue is not ready"},
                    status=409,
                )

            if filename:
                self.logger.bind(tag=TAG).info(
                    f"Push SD audio to device {device_id}, source={source}, file={filename}"
                )
                await self._push_sd_audio_command(conn, filename)
                return self._json_response(
                    {
                        "success": True,
                        "message": "SD audio command pushed",
                        "deviceId": device_id,
                        "source": source,
                        "file": filename,
                    }
                )

            self.logger.bind(tag=TAG).info(
                f"Push text to device {device_id}, source={source}, mode={mode}, text_length={len(text)}"
            )
            if mode == "introduce_location":
                await self._introduce_location(conn, text)
                message = "Location introduction pushed"
            else:
                await self._broadcast_text_now(conn, text)
                message = "Broadcast pushed"

            return self._json_response(
                {
                    "success": True,
                    "message": message,
                    "deviceId": device_id,
                    "source": source,
                    "mode": mode,
                }
            )
        except Exception as e:
            self.logger.bind(tag=TAG).error(f"Push dialog failed: {e}")
            return self._json_response(
                {"success": False, "message": "Internal server error"},
                status=500,
            )
