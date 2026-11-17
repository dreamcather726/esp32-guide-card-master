"""Host-only model preflight and guarded configuration change. Never print secrets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.request
from urllib.parse import urlparse

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql

MODEL_ID = 'LLM_AliLLM'


def current():
    row = database("SELECT HEX(config_json),SHA2(config_json,256) FROM ai_model_config WHERE id=" + sql(MODEL_ID))
    raw, digest = row.split('\t')
    return json.loads(bytes.fromhex(raw).decode()), digest


def probe(cfg, model):
    base = cfg.get('base_url') or cfg.get('url')
    assert urlparse(base).scheme == 'https' and urlparse(base).hostname == 'dashscope.aliyuncs.com'

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None

    data = dict(model=model, stream=True, max_tokens=96, temperature=0,
        enable_thinking=False, messages=[{'role': 'user', 'content':
            '请用一句中文回答：中国的首都是哪个城市？不要输出思考过程。'}])
    request = urllib.request.Request(base.rstrip('/') + '/chat/completions',
        data=json.dumps(data).encode(), headers={'Authorization': 'Bearer ' + cfg['api_key'],
        'Content-Type': 'application/json'}, method='POST')
    start = time.monotonic()
    first = None
    answer = ''
    finish = None
    with urllib.request.build_opener(NoRedirect).open(request, timeout=15) as response:
        for line in response:
            if time.monotonic() - start > 30:
                raise RuntimeError('probe_deadline')
            if not line.startswith(b'data:') or line.strip() == b'data: [DONE]':
                continue
            chunk = json.loads(line[5:])
            for choice in chunk.get('choices', []):
                content = choice.get('delta', {}).get('content') or ''
                if content and first is None:
                    first = time.monotonic() - start
                answer += content
                finish = choice.get('finish_reason') or finish
    assert '北京' in answer and finish == 'stop', 'probe_content_or_finish_failed'
    result = dict(model=model, first_content_seconds=round(first, 3),
        total_seconds=round(time.monotonic() - start, 3), finish_reason=finish, correct=True)
    print(json.dumps(result), flush=True)
    return result


def clear_cache():
    # Only model and server-configuration keys; never flush the Redis database.
    result = subprocess.run(['docker', 'exec', 'xiaozhi-esp32-server-redis', 'redis-cli',
        'DEL', 'model:data:' + MODEL_ID, 'model:name:' + MODEL_ID, 'server:config'],
        capture_output=True, text=True, check=True)
    if not result.stdout.strip().isdigit():
        raise RuntimeError('model_cache_invalidation_failed')


def main(args):
    os.umask(0o077)
    cfg, digest = current()
    if args.action == 'probe':
        results = [probe(cfg, name) for name in ('qwen-flash', 'qwen-plus')]
        Path(__file__).with_name('model-preflight.json').write_text(json.dumps(results, indent=2))
        return
    backup = Path(__file__).with_name('backup')
    backup.mkdir(mode=0o700, exist_ok=True)
    saved = backup / 'model.json'
    if args.action == 'apply':
        assert cfg.get('model_name') == 'deepseek-v3', 'model_changed_since_audit'
        probe(cfg, args.model)
        with saved.open('x') as handle:
            json.dump({'config': cfg, 'sha256': digest}, handle)
        new = dict(cfg, model_name=args.model, max_tokens='1024')
    else:
        old = json.loads(saved.read_text())
        if cfg == old['config']:
            clear_cache()
            print(json.dumps({'status': 'already_rolled_back'}))
            return
        expected = dict(old['config'], model_name=cfg.get('model_name'), max_tokens='1024')
        assert cfg.get('model_name') in ('qwen-flash', 'qwen-plus') and cfg == expected, 'rollback_model_guard'
        new = old['config']
    changed = database("UPDATE ai_model_config SET config_json=" + sql(json.dumps(new, ensure_ascii=False)) +
        ",update_date=NOW() WHERE id=" + sql(MODEL_ID) + " AND SHA2(config_json,256)=" + sql(digest) +
        "; SELECT ROW_COUNT();")
    assert changed == '1', 'concurrent_model_change'
    clear_cache()
    live, _ = current()
    assert live == new
    print(json.dumps({'status': args.action, 'model': live['model_name'], 'max_tokens': live['max_tokens']}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['probe', 'apply', 'rollback'])
    parser.add_argument('--model', choices=['qwen-flash', 'qwen-plus'], default='qwen-plus')
    try:
        main(parser.parse_args())
    except Exception as exc:
        print(json.dumps({'failed': type(exc).__name__}))
        raise SystemExit(1)
