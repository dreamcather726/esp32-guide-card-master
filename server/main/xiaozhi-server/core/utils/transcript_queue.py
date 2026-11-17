"""Commit generated text before it can enter a lossy TTS playback queue."""

import queue
import threading
import time
import uuid

from core.handle.reportHandle import capture_report_identity, enqueue_tts_report


class TranscriptTextQueue(queue.Queue):
    def __init__(self, connection_getter):
        super().__init__()
        self._connection_getter = connection_getter
        self._identities = {}
        self._report_lock = threading.Lock()
        self._last_report_time = 0

    def put(self, item, block=True, timeout=None):
        conn = self._connection_getter()
        if conn is not None and getattr(conn, "user_binding_required", False):
            with self._report_lock:
                try:
                    sentence_id = item.sentence_id
                    event = getattr(item, "_transcript_event", None)
                    if event is None:
                        if sentence_id not in self._identities:
                            if len(self._identities) >= 64:
                                raise RuntimeError("too many unfinished transcript turns")
                            self._identities[sentence_id] = capture_report_identity(
                                conn, turn_id=sentence_id)
                        self._last_report_time = max(int(time.time() * 1000), self._last_report_time + 1)
                        # Retrying the same DTO after WAL commit keeps the exact event.
                        event = (self._identities[sentence_id], uuid.uuid4().hex,
                                 self._last_report_time, item.content_detail)
                        item._transcript_event = event
                    identity, message_id, report_time, text = event
                    content_type = getattr(item.content_type, "value", item.content_type)
                    if str(content_type).lower() == "text" and text:
                        enqueue_tts_report(conn, text, None,
                            identity=identity, message_id=message_id,
                            report_time=report_time,
                            delivery_status="generated")
                    sentence_type = getattr(item.sentence_type, "value", item.sentence_type)
                    if str(sentence_type).lower() == "last":
                        self._identities.pop(sentence_id, None)
                except Exception:
                    conn.client_abort = True
                    conn.user_binding_ready = False
                    raise
        return super().put(item, block=block, timeout=timeout)
