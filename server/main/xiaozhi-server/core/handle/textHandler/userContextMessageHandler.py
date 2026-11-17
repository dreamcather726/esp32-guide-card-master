from typing import Dict, Any

from core.handle.textMessageHandler import TextMessageHandler
from core.handle.textMessageType import TextMessageType

TAG = __name__


class UserContextMessageHandler(TextMessageHandler):
    @property
    def message_type(self) -> TextMessageType:
        return TextMessageType.USER_CONTEXT

    async def handle(self, conn, msg_json: Dict[str, Any]) -> None:
        user_guid = (
            msg_json.get("userGUID")
            or msg_json.get("userGuid")
            or msg_json.get("user_guid")
        )
        if not user_guid:
            conn.logger.bind(tag=TAG).warning(
                "user_context ignored: missing userGUID"
            )
            return
        await conn.update_user_guid(user_guid, source="user_context")
