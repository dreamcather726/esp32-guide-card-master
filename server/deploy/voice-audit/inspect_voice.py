"""Read-only voice diagnostics. Outputs aggregate data and redacted event samples."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import re
import subprocess


INSPECT = r'''
import hashlib, json, os, pathlib, yaml
root = pathlib.Path.cwd()
files = ['core/connection.py', 'core/handle/receiveAudioHandle.py',
    'core/handle/textHandler/listenMessageHandler.py', 'core/handle/sendAudioHandle.py',
    'core/handle/abortHandle.py', 'core/providers/asr/base.py',
    'core/providers/llm/openai/openai.py', 'core/providers/tts/base.py',
    'core/providers/tts/huoshan_double_stream.py', 'core/utils/audioRateController.py']
print(json.dumps({'live_sha256': {p: hashlib.sha256((root / p).read_bytes()).hexdigest()
    for p in files if (root / p).is_file()}}))
safe = ['close_connection_no_voice_time', 'tts_output_timeout', 'tts_audio_completion_timeout',
    'tts_audio_send_delay', 'max_output_size', 'tool_call_timeout', 'selected_module',
    'enable_greeting', 'delete_audio', 'max_context_length', 'clear_memory', 'connection_timeout']
module_safe = ['type', 'model_name', 'timeout', 'max_tokens', 'temperature', 'top_p',
    'min_silence_duration_ms', 'min_speech_duration_ms', 'threshold', 'threshold_low',
    'silence_threshold_ms', 'max_duration', 'use_itn', 'enable_punc', 'enable_itn']
for name in ['config.yaml', 'data/.config.yaml']:
    p = root / name
    if not p.is_file(): continue
    cfg = yaml.safe_load(p.read_text()) or {}
    result = {k: cfg[k] for k in safe if k in cfg}
    result['provider_settings'] = {section: {k: {a: v[a] for a in module_safe if a in v}
        for k, v in (cfg.get(section) or {}).items() if isinstance(v, dict)}
        for section in ['VAD', 'ASR', 'LLM', 'TTS', 'Memory', 'Intent']}
    print(json.dumps({'config_file': name, 'safe_settings': result}, ensure_ascii=False))
log_dir = root / 'tmp'
print(json.dumps({'log_files': [{'name': p.name, 'bytes': p.stat().st_size}
    for p in sorted(log_dir.glob('*.log'))][-12:]}))
'''

MARKERS = {
    'tts_output_timeout': 'TTS output timeout',
    'audio_drain_timeout': 'Waiting for audio queue completion timed out',
    'recognition': '\u8bc6\u522b\u6587\u672c:',
    'llm_question': '\u5927\u6a21\u578b\u6536\u5230\u7528\u6237\u6d88\u606f:',
    'first_audio': '\u53d1\u9001\u7b2c\u4e00\u6bb5\u8bed\u97f3:',
    'llm_diag': '[LLM_DIAG]',
    'asr_error': '\u8bed\u97f3\u8bc6\u522b\u5931\u8d25:',
    'voice_stop_error': '\u5904\u7406\u8bed\u97f3\u505c\u6b62\u5931\u8d25:',
    'audio_error': '\u53d1\u9001\u97f3\u9891\u5931\u8d25',
    'connection_timeout': '\u8fde\u63a5\u8d85\u65f6',
    'abort': '\u6536\u5230\u4e2d\u6b62',
    'user_context': 'user_context',
}


def run(*cmd):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True).stdout


def redact(text):
    text = re.sub(r'(?i)(Bearer\s+|api[_-]?key[\s:=\"\x27]+|token[\s:=\"\x27]+)\S+', r'\1[redacted]', text)
    text = re.sub(r'https?://\S+', '[url]', text)
    text = re.sub(r'\b[\w-]{24,}\b', '[identifier]', text)
    text = re.sub(r'\b\d{11,}\b', '[number]', text)
    return text[:600]


def main(args):
    if not args.logs_only:
        print(run('docker', 'exec', 'xiaozhi-esp32-server', 'python', '-c', INSPECT))
    logs = run('docker', 'logs', '--since', args.since, '--tail', str(args.tail),
               '--timestamps', 'xiaozhi-esp32-server')
    lines = logs.splitlines()
    print(json.dumps({'window': args.since, 'line_count': len(lines), 'tail_limit': args.tail,
        'first_timestamp': lines[0].split(' ')[0] if lines else None,
        'last_timestamp': lines[-1].split(' ')[0] if lines else None}))
    counts, errors = Counter(), Counter()
    samples, transcript_events, error_samples = defaultdict(list), [], defaultdict(list)
    model_requests = []
    latency = defaultdict(list)
    for line in lines:
        timestamp = line.split(' ')[0]
        model_start = re.search(r'\[LLM_DIAG\] request_start model=(\S+) messages=(\d+) chars=(\d+)', line)
        if model_start:
            model_requests.append({'time': timestamp, 'model': model_start[1],
                'messages': int(model_start[2]), 'chars': int(model_start[3])})
        for kind, marker in MARKERS.items():
            if marker not in line:
                continue
            counts[kind] += 1
            if kind in ('recognition', 'llm_question', 'first_audio'):
                content = line.split(marker, 1)[1].strip()
                transcript_events.append({'time': timestamp, 'kind': kind, 'chars': len(content),
                    'fingerprint': hashlib.sha256(content.encode()).hexdigest()[:12]})
            elif kind != 'user_context':
                samples[kind].append(redact(line))
        if '-ERROR-' in line or ' - ERROR - ' in line or '-WARNING-' in line:
            tag = re.search(r'\[([^\]]+)\]-(?:ERROR|WARNING)-', line)
            if tag:
                errors[tag[1]] += 1
                if len(line) < 1400 and not re.search(r'(?i)api_key|access_token|authorization|config_json', line):
                    error_samples[tag[1]].append(redact(line))
        for key, pattern in [('llm_first', r'first_content elapsed=([\d.]+)s'),
                             ('llm_total', r'stream_end elapsed=([\d.]+)s'),
                             ('llm_error', r'request_error.*elapsed=([\d.]+)s')]:
            found = re.search(pattern, line)
            if found:
                latency[key].append(float(found[1]))
    print(json.dumps({'event_counts': counts, 'warning_error_tags': errors}, ensure_ascii=False))
    print(json.dumps({'model_requests': model_requests[-50:]}))
    for key, values in latency.items():
        ordered = sorted(values)
        print(json.dumps({'metric': key, 'n': len(values), 'min_s': ordered[0],
            'median_s': ordered[len(ordered)//2], 'p95_s': ordered[min(len(ordered)-1,int(len(ordered)*.95))],
            'max_s': ordered[-1]}))
    for key, values in samples.items():
        print(json.dumps({'event': key, 'recent_samples': values[-args.samples:]}, ensure_ascii=False))
    for key, values in error_samples.items():
        print(json.dumps({'error_tag': key, 'recent_samples': values[-args.samples:]}, ensure_ascii=False))
    print(json.dumps({'recent_transcript_events': transcript_events[-80:]}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--since', default='48h')
    parser.add_argument('--tail', type=int, default=150000)
    parser.add_argument('--samples', type=int, default=6)
    parser.add_argument('--logs-only', action='store_true')
    main(parser.parse_args())
