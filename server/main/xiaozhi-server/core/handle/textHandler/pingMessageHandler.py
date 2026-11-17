import json
import time
from typing import Dict, Any

from core.handle.textMessageHandler import TextMessageHandler
from core.handle.textMessageType import TextMessageType

TAG = __name__


class PingMessageHandler(TextMessageHandler):
    """Ping娑堟伅澶勭悊鍣紝鐢ㄤ簬淇濇寔WebSocket杩炴帴"""

    @property
    def message_type(self) -> TextMessageType:
        return TextMessageType.PING

    async def handle(self, conn, msg_json: Dict[str, Any]) -> None:
        """
        澶勭悊PING娑堟伅锛屽彂閫丳ONG鍝嶅簲
        娑堟伅鏍煎紡锛歿"type": "ping"}
        Args:
            conn: WebSocket杩炴帴瀵硅薄
            msg_json: PING娑堟伅鐨凧SON鏁版嵁
        """
        try:
            conn.logger.debug(f"鏀跺埌PING娑堟伅锛屽彂閫丳ONG鍝嶅簲")
            conn.last_activity_time = time.time() * 1000
            # 鏋勯€燩ONG鍝嶅簲娑堟伅
            pong_message = {
                "type": "pong",
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
            }

            # 鍙戦€丳ONG鍝嶅簲
            await conn.websocket.send(json.dumps(pong_message))

        except Exception as e:
            conn.logger.error(f"澶勭悊PING娑堟伅鏃跺彂鐢熼敊璇? {e}")

