import asyncio
import json
from typing import Dict, Any

from core.handle.textMessageHandler import TextMessageHandler
from core.handle.textMessageType import TextMessageType

TAG = __name__


class RoleSwitchMessageHandler(TextMessageHandler):
    @property
    def message_type(self) -> TextMessageType:
        return TextMessageType.ROLE_SWITCH

    async def handle(self, conn, msg_json: Dict[str, Any]) -> None:
        action = msg_json.get("action", "next")
        init_event = getattr(conn, "config_initialized_event", None)
        if init_event is not None and not init_event.is_set():
            try:
                await asyncio.wait_for(init_event.wait(), timeout=8)
            except asyncio.TimeoutError:
                conn.logger.bind(tag=TAG).warning(
                    "Role switch ignored: connection config is not initialized"
                )
                return

        role_id = str(msg_json.get("roleId") or msg_json.get("role_id") or "").strip()
        if action == "next":
            success = await conn.switch_next_role_voice_binding()
        elif action == "select" and role_id:
            success = await conn.switch_role_voice_binding(role_id)
        else:
            conn.logger.bind(tag=TAG).warning(
                f"Unsupported role switch request: action={action}, roleId={role_id}"
            )
            success = False

        if not success:
            binding_count = len(conn.get_enabled_role_voice_bindings())
            await conn.websocket.send(
                json.dumps(
                    {
                        "type": "role_switch",
                        "state": "error",
                        "session_id": conn.session_id,
                        "roleId": role_id,
                        "bindingCount": binding_count,
                        "message": "Role binding not found or unavailable",
                    },
                    ensure_ascii=False,
                )
            )
