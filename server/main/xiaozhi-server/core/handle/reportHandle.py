"""Bound reports persist immediately; legacy reports use the connection queue."""

import time
import json
from typing import TYPE_CHECKING

from core.utils.report_outbox import ReportIdentity, ReportIdentityError, get_report_outbox

if TYPE_CHECKING:
    from core.connection import ConnectionHandler

TAG = __name__


async def report(
    conn: "ConnectionHandler",
    type,
    text,
    opus_data,
    report_time,
    session_id=None,
    user_guid=None,
):
    """执行聊天记录上报操作

    Args:
        conn: 连接对象
        type: 上报类型，1为用户，2为智能体，3为工具调用
        text: 合成文本
        opus_data: opus音频数据
        report_time: 上报时间
    """
    from config.manage_api_client import report as manage_report

    try:
        if opus_data:
            audio_data = opus_to_wav(conn, opus_data)
        else:
            audio_data = None
        # 执行异步上报
        await manage_report(
            mac_address=conn.device_id,
            session_id=session_id or conn.session_id,
            chat_type=type,
            content=text,
            audio=audio_data,
            report_time=report_time,
            user_guid=user_guid if user_guid is not None else conn.user_guid,
            history_generation=int(getattr(conn, "config", {}).get("deviceHistoryGeneration", 0)),
        )
    except Exception as e:
        conn.logger.bind(tag=TAG).error(f"聊天记录上报失败: {e}")


def opus_to_wav(conn: "ConnectionHandler", opus_data):
    """将Opus数据转换为WAV格式的字节流

    Args:
        output_dir: 输出目录（保留参数以保持接口兼容）
        opus_data: opus音频数据

    Returns:
        bytes: WAV格式的音频数据
    """
    import opuslib_next

    decoder = None
    try:
        decoder = opuslib_next.Decoder(16000, 1)  # 16kHz, 单声道
        pcm_data = []

        for opus_packet in opus_data:
            try:
                pcm_frame = decoder.decode(opus_packet, 960)  # 960 samples = 60ms
                pcm_data.append(pcm_frame)
            except opuslib_next.OpusError as e:
                conn.logger.bind(tag=TAG).error(f"Opus解码错误: {e}", exc_info=True)

        if not pcm_data:
            raise ValueError("没有有效的PCM数据")

        # 创建WAV文件头
        pcm_data_bytes = b"".join(pcm_data)
        num_samples = len(pcm_data_bytes) // 2  # 16-bit samples

        # WAV文件头
        wav_header = bytearray()
        wav_header.extend(b"RIFF")  # ChunkID
        wav_header.extend((36 + len(pcm_data_bytes)).to_bytes(4, "little"))  # ChunkSize
        wav_header.extend(b"WAVE")  # Format
        wav_header.extend(b"fmt ")  # Subchunk1ID
        wav_header.extend((16).to_bytes(4, "little"))  # Subchunk1Size
        wav_header.extend((1).to_bytes(2, "little"))  # AudioFormat (PCM)
        wav_header.extend((1).to_bytes(2, "little"))  # NumChannels
        wav_header.extend((16000).to_bytes(4, "little"))  # SampleRate
        wav_header.extend((32000).to_bytes(4, "little"))  # ByteRate
        wav_header.extend((2).to_bytes(2, "little"))  # BlockAlign
        wav_header.extend((16).to_bytes(2, "little"))  # BitsPerSample
        wav_header.extend(b"data")  # Subchunk2ID
        wav_header.extend(len(pcm_data_bytes).to_bytes(4, "little"))  # Subchunk2Size

        # 返回完整的WAV数据
        return bytes(wav_header) + pcm_data_bytes
    finally:
        if decoder is not None:
            try:
                del decoder
            except Exception as e:
                conn.logger.bind(tag=TAG).debug(f"释放decoder资源时出错: {e}")


def capture_report_identity(conn: "ConnectionHandler", *, turn_id=None):
    """Capture at the event hook, or earlier when a producer buffers an event."""
    version = getattr(conn, "binding_version", None)
    if not getattr(conn, "user_binding_required", False) or not getattr(conn, "user_binding_ready", False):
        raise ReportIdentityError("Bound report requires a ready user binding")
    identity = ReportIdentity(
        mac_address=getattr(conn, "device_id", None),
        user_guid=getattr(conn, "user_guid", None),
        session_id=getattr(conn, "session_id", None),
        usage_session_id=getattr(conn, "usage_session_id", None),
        boot_id=getattr(conn, "boot_id", None),
        binding_version=version,
        role_id=getattr(conn, "role_id", None),
        turn_id=turn_id if turn_id is not None else getattr(conn, "turn_id", None),
        history_generation=getattr(conn, "config", {}).get("deviceHistoryGeneration"),
    )
    if version != getattr(conn, "binding_version", None) or not getattr(conn, "user_binding_ready", False):
        raise ReportIdentityError("User binding changed while capturing a report")
    return identity


def enqueue_report(conn: "ConnectionHandler", report_type, text, opus_data, timestamp,
                   *, identity=None, message_id=None, delivery_status=None, text_truncated=False):
    if identity is not None or getattr(conn, "user_binding_required", False):
        try:
            snapshot = identity if identity is not None else capture_report_identity(conn)
            if not isinstance(snapshot, ReportIdentity):
                raise ReportIdentityError("Expected an immutable ReportIdentity")
            payload = snapshot.payload(
                report_type, text if text is not None else "", timestamp,
                message_id=message_id, delivery_status=delivery_status,
                text_truncated=text_truncated,
            )
            return get_report_outbox().enqueue(payload)
        except Exception as exc:
            conn.logger.bind(tag=TAG).error(f"Bound report persistence failed: {exc}")
            raise
    conn.report_queue.put(
        (
            report_type,
            text,
            opus_data,
            timestamp,
            conn.session_id,
            conn.user_guid,
        )
    )


def enqueue_tts_report(conn: "ConnectionHandler", text, opus_data, *, report_time=None, **event):
    if event.get("identity") is not None or getattr(conn, "user_binding_required", False):
        timestamp = int(time.time() * 1000) if report_time is None else report_time
        return enqueue_report(conn, 2, text, None, timestamp, **event)
    if not conn.read_config_from_api or conn.need_bind or not conn.report_tts_enable:
        return
    if conn.chat_history_conf == 0:
        return
    """将TTS数据加入上报队列

    Args:
        conn: 连接对象
        text: 合成文本
        opus_data: opus音频数据
    """
    try:
        # 使用连接对象的队列，传入文本和二进制数据而非文件路径
        if conn.chat_history_conf == 2:
            enqueue_report(
                conn, 2, text, opus_data, int(time.time() * 1000)
            )
            conn.logger.bind(tag=TAG).debug(
                f"TTS数据已加入上报队列: {conn.device_id}, 音频大小: {len(opus_data)} "
            )
        else:
            enqueue_report(conn, 2, text, None, int(time.time() * 1000))
            conn.logger.bind(tag=TAG).debug(
                f"TTS数据已加入上报队列: {conn.device_id}, 不上报音频"
            )
    except Exception as e:
        conn.logger.bind(tag=TAG).error(f"加入TTS上报队列失败: {text}, {e}")


def enqueue_tool_report(conn: "ConnectionHandler", tool_name: str, tool_input: dict, tool_result: str = None, report_tool_call: bool = True):
    """将工具调用数据加入上报队列

    Args:
        conn: 连接对象
        tool_name: 工具名称
        tool_input: 工具输入参数
        tool_result: 工具执行结果（可选）
        report_tool_call: 是否上报工具调用本身，默认True；仅上报结果时设为False
    """
    bound = getattr(conn, "user_binding_required", False)
    if not bound and (not conn.read_config_from_api or conn.need_bind):
        return
    if not bound and conn.chat_history_conf == 0:
        return

    try:
        timestamp = int(time.time() * 1000)

        # 构建工具调用内容
        if report_tool_call:
            tool_text = json.dumps(
                [
                    {
                        "type": "tool",
                        "text": f"{tool_name}({json.dumps(tool_input, ensure_ascii=False)})",
                    }
                ]
            )
            enqueue_report(conn, 3, tool_text, None, timestamp)

        # 构建工具结果内容
        if tool_result:
            result_display = f'{{"result":"{str(tool_result)}"}}'
            result_content = json.dumps([{"type": "tool_result", "text": result_display}], ensure_ascii=False)
            enqueue_report(conn, 3, result_content, None, timestamp + 1)
    except Exception as e:
        conn.logger.bind(tag=TAG).error(f"加入工具上报队列失败: {e}")
        if bound:
            raise


def enqueue_asr_report(conn: "ConnectionHandler", text, opus_data, *, report_time=None, **event):
    if event.get("identity") is not None or getattr(conn, "user_binding_required", False):
        timestamp = int(time.time() * 1000) if report_time is None else report_time
        return enqueue_report(conn, 1, text, None, timestamp, **event)
    if not conn.read_config_from_api or conn.need_bind or not conn.report_asr_enable:
        return
    if conn.chat_history_conf == 0:
        return
    """将ASR数据加入上报队列

    Args:
        conn: 连接对象
        text: 合成文本
        opus_data: opus音频数据
    """
    try:
        # 使用连接对象的队列，传入文本和二进制数据而非文件路径
        if conn.chat_history_conf == 2:
            enqueue_report(
                conn, 1, text, opus_data, int(time.time() * 1000)
            )
            conn.logger.bind(tag=TAG).debug(
                f"ASR数据已加入上报队列: {conn.device_id}, 音频大小: {len(opus_data)} "
            )
        else:
            enqueue_report(conn, 1, text, None, int(time.time() * 1000))
            conn.logger.bind(tag=TAG).debug(
                f"ASR数据已加入上报队列: {conn.device_id}, 不上报音频"
            )
    except Exception as e:
        conn.logger.bind(tag=TAG).debug(f"加入ASR上报队列失败: {text}, {e}")
