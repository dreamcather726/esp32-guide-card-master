"""Read-only model routing and recent device metadata; no profile text or credentials."""
import json
import sys
from urllib.parse import urlparse
import urllib.request
import urllib.error

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database


def rows(statement):
    # HEX avoids MySQL batch-mode escaping of nested JSON values.
    return [json.loads(bytes.fromhex(row).decode()) for row in database(statement).splitlines() if row]


if '--probe-models' in sys.argv:
    row = rows("SELECT HEX(JSON_OBJECT('config',config_json)) FROM ai_model_config WHERE id='LLM_AliLLM'")[0]
    cfg = row['config']
    if isinstance(cfg, str):
        cfg = json.loads(cfg)
    base = cfg.get('base_url') or cfg.get('url') or ''
    parsed = urlparse(base)
    assert parsed.scheme == 'https' and parsed.hostname == 'dashscope.aliyuncs.com'

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    request = urllib.request.Request(base.rstrip('/') + '/models',
        headers={'Authorization': 'Bearer ' + cfg['api_key']})
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=20) as response:
            data = json.load(response)
        available = {r['id'] for r in data.get('data', []) if isinstance(r, dict) and 'id' in r}
        candidates = ['deepseek-v3', 'qwen-flash', 'qwen-turbo', 'qwen-plus', 'deepseek-v3.2']
        print(json.dumps({'read_only_model_catalog': {m: m in available for m in candidates},
            'configured_model': cfg.get('model_name'), 'model_count': len(available),
            'generation_request_sent': False}))
    except urllib.error.HTTPError as exc:
        print(json.dumps({'catalog_http_status': exc.code, 'generation_request_sent': False}))
    except Exception as exc:
        print(json.dumps({'catalog_error_type': type(exc).__name__, 'generation_request_sent': False}))
    raise SystemExit(0)


models = rows("""SELECT HEX(JSON_OBJECT('id',id,'type',model_type,'code',model_code,
    'default',is_default,'enabled',is_enabled,'updated',update_date,'config',config_json))
    FROM ai_model_config WHERE is_enabled=1""")
model_map = {}
for model in models:
    cfg = model.pop('config') or {}
    if isinstance(cfg, str):
        cfg = json.loads(cfg)
    keep = ['type','model_name','model','timeout','max_tokens','temperature','top_p',
        'min_silence_duration_ms','max_sentence_silence','threshold','threshold_low',
        'sample_rate','max_duration','llm','enable_thinking']
    model['settings'] = {k: cfg[k] for k in keep if k in cfg}
    model['endpoint_host'] = urlparse(cfg.get('base_url') or cfg.get('url') or '').hostname
    key = str(cfg.get('api_key') or '')
    model['key_is_placeholder'] = bool(key) and (key.lower() in ('your_api_key','api_key','your-api-key') or '\u4f60\u7684' in key)
    model_map[model['id']] = model
agents = rows("""SELECT HEX(JSON_OBJECT('agentId',a.id,'ownerId',a.user_id,
    'asr',a.asr_model_id,'llm',a.llm_model_id,'slm',a.slm_model_id,'tts',a.tts_model_id,
    'intent',a.intent_model_id,'memory',a.mem_model_id,'promptChars',CHAR_LENGTH(a.system_prompt),
    'devices',COUNT(d.id),'latestConnection',MAX(d.last_connected_at)))
    FROM ai_agent a LEFT JOIN ai_device d ON d.agent_id=a.id
    GROUP BY a.id ORDER BY MAX(d.last_connected_at) DESC LIMIT 12""")
referenced = set()
for agent in agents:
    for kind in ('asr','llm','slm','tts','intent','memory'):
        if agent[kind]:
            referenced.add(agent[kind])
    print(json.dumps({'agent_routing': agent}, ensure_ascii=False))
for identifier in list(referenced):
    dependency = model_map.get(identifier, {}).get('settings', {}).get('llm')
    if dependency:
        referenced.add(dependency)
for model in models:
    if model['id'] in referenced or model['default']:
        print(json.dumps({'model': model}, ensure_ascii=False))
print(json.dumps({'recent_devices': rows("""SELECT HEX(JSON_OBJECT('mac',mac_address,
    'agentId',agent_id,'firmware',app_version,'lastConnected',last_connected_at))
    FROM ai_device WHERE mac_address NOT LIKE '02:fe:%'
    ORDER BY last_connected_at DESC LIMIT 12""")}, ensure_ascii=False))
print(json.dumps({'system_timeout_params': rows("""SELECT HEX(JSON_OBJECT('name',param_code,'value',param_value))
    FROM sys_params WHERE param_code IN ('close_connection_no_voice_time','tts_output_timeout',
    'tts_audio_completion_timeout','tts_audio_send_delay','tool_call_timeout')""")}, ensure_ascii=False))
