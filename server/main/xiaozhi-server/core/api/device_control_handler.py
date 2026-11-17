"""Service-only controls; customer credentials never reach this endpoint."""
import asyncio
import hmac
import re

from aiohttp import web
from config.config_loader import get_private_config_from_api
from core.device_connection_registry import get_connection


class DeviceControlHandler:
    def __init__(self, config):
        self.config = config
        self._locks = {}

    async def handle_post(self, request):
        secret = self.config.get("manager-api", {}).get("secret")
        credential = request.headers.get("Authorization", "")
        if not secret or not hmac.compare_digest(credential, "Bearer " + secret):
            return web.json_response({"success": False}, status=401)
        try:
            data = await request.json()
            mac = data.get("deviceId", "")
            action = data.get("action")
            if not re.fullmatch(r"[0-9a-f]{2}(:[0-9a-f]{2}){5}", mac) or action not in {"apply_role", "clear_history"}:
                raise ValueError()
        except (ValueError, TypeError, AttributeError):
            return web.json_response({"success": False}, status=400)
        # Keep locks only while requests exist, including concurrent waiters.
        state = self._locks.setdefault(mac, [asyncio.Lock(), 0])
        state[1] += 1
        try:
            async with state[0]:
                return await self._apply(mac, action, data)
        finally:
            state[1] -= 1
            if not state[1]:
                self._locks.pop(mac, None)

    async def _apply(self, mac, action, data):
        conn = await get_connection(mac)
        if conn is None or conn.websocket is None:
            return web.json_response({"success": False}, status=404)
        if action == "clear_history":
            # End this live context without changing other devices or user profiles.
            conn.client_abort = True
            conn.clear_queues()
            await conn.websocket.close(code=1012, reason="device_history_cleared")
            return web.json_response({"success": True})
        if not getattr(conn, "components_ready", False) or getattr(conn, "client_is_speaking", False):
            return web.json_response({"success": False, "reason": "device_busy"}, status=409)
        try:
            private = await asyncio.wait_for(get_private_config_from_api(conn.config, mac,
                conn.headers.get("client-id", mac), conn.user_guid), timeout=7)
            if (private.get("agentId") != data.get("agentId")
                    or private.get("agentId") != conn.agent_id
                    or private.get("deviceRoleId") != data.get("roleId")
                    or str(private.get("deviceRoleBindingVersion")) != str(data.get("bindingVersion"))):
                return web.json_response({"success": False, "reason": "binding_superseded"}, status=409)
            if getattr(conn, "user_binding_required", False) and private.get("userGuid") != conn.user_guid:
                return web.json_response({"success": False}, status=409)
            if getattr(conn, "client_is_speaking", False):
                return web.json_response({"success": False, "reason": "device_busy"}, status=409)
            conn.config["roleVoiceBindings"] = private.get("roleVoiceBindings", [])
            applied = await conn.switch_role_voice_binding(data["roleId"])
            return web.json_response({"success": bool(applied)}, status=200 if applied else 409)
        except Exception:
            return web.json_response({"success": False, "reason": "runtime_unavailable"}, status=503)
