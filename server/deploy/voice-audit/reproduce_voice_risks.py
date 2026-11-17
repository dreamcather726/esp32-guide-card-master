"""Offline characterization of current source behavior, not a fix or hardware test.

Extracts actual functions with AST and substitutes only external dependencies.
No network calls, audio devices, production module imports, or database writes.
"""
import argparse
import ast
import asyncio
import json
from pathlib import Path
import threading
import types
from typing import Any, Dict, List
from unittest.mock import AsyncMock


class Log:
    def __init__(self):
        self.errors = []

    def bind(self, **kwargs):
        return self

    def debug(self, *args):
        pass

    info = debug
    warning = debug

    def error(self, message):
        self.errors.append(message)


def extract(root, file, names, context):
    tree = ast.parse((root / file).read_text(encoding='utf-8-sig'))
    nodes = [node for node in tree.body if getattr(node, 'name', None) in names]
    assert len(nodes) == len(names), 'Source function names changed; review the reproducer'
    exec(compile(ast.Module(body=nodes, type_ignores=[]), file, 'exec'), context)
    return context


class ConnectionClosed(Exception):
    pass


class FakeWS:
    def __init__(self, conn, events=()):
        self.conn = conn
        self.events = iter(events)
        self.sent = []

    async def recv(self):
        try:
            event, stopped = next(self.events)
        except StopIteration:
            raise ConnectionClosed()
        self.conn.client_voice_stop = stopped
        return json.dumps(event)

    async def send(self, value):
        self.sent.append(value)

    async def close(self):
        pass


def asr_fixture(root, events):
    log = Log()
    namespace = extract(root, 'core/providers/asr/aliyunbl_stream.py', ['ASRProvider'], {
        'ASRProviderBase': object, 'logger': log, 'TAG': 'audit', 'List': List,
        'asyncio': asyncio, 'json': json,
        'websockets': types.SimpleNamespace(ConnectionClosed=ConnectionClosed)})
    provider = namespace['ASRProvider'].__new__(namespace['ASRProvider'])
    conn = types.SimpleNamespace(client_listen_mode='manual', client_voice_stop=False,
        stop_event=threading.Event(), asr_audio=[b'frame'])

    def reset():
        conn.asr_audio.clear()
        conn.client_voice_stop = False

    conn.reset_audio_states = reset
    provider.text = ''
    provider.asr_ws = FakeWS(conn, events)
    provider.is_processing = True
    provider.server_ready = False
    provider.task_id = 'fixture'
    provider.forward_task = None
    provider.decoder = types.SimpleNamespace(decode=lambda frame, size: frame)
    provider.handle_voice_stop = AsyncMock()
    return provider, conn, log


def final(text):
    return {'header': {'event': 'result-generated'}, 'payload': {'output': {
        'sentence': {'text': text, 'sentence_end': True, 'end_time': 900}}}}


FINISHED = {'header': {'event': 'task-finished'}}


async def run(root):
    results = []
    provider, conn, _ = asr_fixture(root, [(final('FIRST'), False), (FINISHED, True)])
    await provider._forward_results(conn)
    assert provider.handle_voice_stop.await_count == 0
    assert provider.text == 'FIRST'
    results.append({'case': 'manual_final_before_stop_then_task_finished',
        'handler_calls': 0, 'recognized_text_left_unsubmitted': True})

    provider.asr_ws = FakeWS(conn, [(final('SECOND'), True)])
    conn.asr_audio = [b'next-frame']
    seen = []

    async def capture(connection, audio):
        seen.append(provider.text)
        await provider.speech_to_text([], 'fixture', audio_format='opus')

    provider.handle_voice_stop = capture
    await provider._forward_results(conn)
    assert seen == ['FIRSTSECOND']
    assert provider.text == ''
    results.append({'case': 'next_question_after_unsubmitted_final', 'previous_text_concatenated': True})

    provider, conn, _ = asr_fixture(root, [({'header': {'event': 'task-started'}}, False), (FINISHED, True)])
    conn.asr_audio = [bytes([n]) for n in range(30)]
    ws = provider.asr_ws
    await provider._forward_results(conn)
    frames = [value for value in ws.sent if isinstance(value, bytes)]
    assert frames == [bytes([n]) for n in range(20, 30)]
    results.append({'case': 'buffered_audio_before_asr_ready', 'buffered_frames': 30,
        'sent_frames': len(frames), 'missing_leading_frames': 20,
        'frame_duration_ms_in_project': 60})

    provider, conn, log = asr_fixture(root, [])

    async def delayed_cleanup():
        await asyncio.sleep(.02)
        await provider._cleanup()

    await asyncio.gather(provider._cleanup(), delayed_cleanup())
    assert any("'NoneType' object has no attribute 'close'" in message for message in log.errors)
    results.append({'case': 'concurrent_asr_cleanup', 'none_close_error_reproduced': True})

    namespace = extract(root, 'core/handle/sendAudioHandle.py',
        ['_wait_for_audio_completion', 'send_tts_message', '_cancel_tts_watchdog'],
        {'asyncio': asyncio, 'json': json, 'TAG': 'audit', 'PRE_BUFFER_COUNT': 5})
    rate = types.SimpleNamespace(queue=[b'audio'] * 1000, queue_empty_event=asyncio.Event(),
                                frame_duration=0, stopped=False)

    def stop():
        rate.stopped = True

    rate.stop_sending = stop
    conn = types.SimpleNamespace(config={'tts_audio_completion_timeout': .01},
        sentence_id='fixture', session_id='fixture', audio_rate_controller=rate,
        client_is_speaking=True, logger=Log())
    conn.websocket = FakeWS(conn)
    conn.clearSpeakStatus = lambda: setattr(conn, 'client_is_speaking', False)
    await namespace['send_tts_message'](conn, 'stop')
    assert rate.stopped and len(rate.queue) == 1000
    assert json.loads(conn.websocket.sent[-1])['state'] == 'stop'
    results.append({'case': 'tts_drain_deadline', 'queued_packets_at_stop': len(rate.queue),
        'sender_cancelled_with_pending_audio': True, 'deadline_scaled_for_test_seconds': .01,
        'production_default_seconds': 15})

    namespace = extract(root, 'core/handle/textHandler/listenMessageHandler.py', ['ListenTextMessageHandler'],
        {'TextMessageHandler': object, 'Dict': Dict, 'Any': Any, 'TextMessageType': object,
         'InterfaceType': types.SimpleNamespace(STREAM='stream'), 'TAG': 'audit'})
    handler = namespace['ListenTextMessageHandler']()
    conn = types.SimpleNamespace(ensure_user_binding=AsyncMock(return_value=True), asr=None)
    try:
        await handler.handle(conn, {'state': 'stop'})
    except AttributeError as exc:
        assert 'interface_type' in str(exc)
    else:
        raise AssertionError('Expected the current uninitialized-ASR defect')
    results.append({'case': 'listen_stop_before_asr_initialized', 'none_interface_type_error_reproduced': True})
    print(json.dumps({'characterization_only': True, 'source_root': str(root),
        'reproduced_current_risks': len(results), 'results': results}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    args = parser.parse_args()
    asyncio.run(run(args.source_root))
