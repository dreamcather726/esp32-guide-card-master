"""Read-only, scoped multilingual diagnosis without credentials or profile text."""
import json
import re
import subprocess
import sys

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql


def rows(statement):
    return [json.loads(bytes.fromhex(row).decode())
            for row in database(statement).splitlines() if row]


def emit(name, value):
    print(json.dumps({name: value}, ensure_ascii=False))


agents = rows("SELECT HEX(JSON_OBJECT('id',id,'name',agent_name,'prompt',system_prompt,"
              "'asr',asr_model_id,'llm',llm_model_id,'tts',tts_model_id)) FROM ai_agent "
              "WHERE agent_name LIKE '%小豹%' OR system_prompt LIKE '%TVET%'")
emit('matching_agents', agents)
emit('matching_devices', rows("SELECT HEX(JSON_OBJECT('mac',mac_address,'alias',alias,"
      "'agent',agent_id,'firmware',app_version,'last_connected',last_connected_at)) "
      "FROM ai_device WHERE alias LIKE '%小豹%' ORDER BY last_connected_at DESC LIMIT 20"))
emit('history_columns', database('SHOW COLUMNS FROM ai_agent_chat_history'))
emit('agent_columns', database('SHOW COLUMNS FROM ai_agent'))
emit('agent_directory', rows("SELECT HEX(JSON_OBJECT('id',id,'name',agent_name,"
     "'prompt_chars',CHAR_LENGTH(system_prompt),'tts_language',tts_language)) FROM ai_agent"))
emit('recent_device_directory', rows("SELECT HEX(JSON_OBJECT('mac',mac_address,'alias',alias,"
     "'agent',agent_id,'last_connected',last_connected_at)) FROM ai_device "
     "ORDER BY last_connected_at DESC LIMIT 15"))
emit('relevant_history', rows("SELECT HEX(JSON_OBJECT('mac',mac_address,'agent',agent_id,"
     "'type',chat_type,'content',content,'time',created_at,'turn',turn_id)) "
     "FROM ai_agent_chat_history WHERE created_at > DATE_SUB(NOW(),INTERVAL 7 DAY) "
     "AND (LOWER(content) LIKE '%tvet%' OR LOWER(content) LIKE '%league%' "
     "OR content LIKE " + sql('%联盟%') + ") ORDER BY created_at DESC LIMIT 40"))
emit('role_tables', database("SHOW TABLES LIKE '%role%'"))
emit('role_directory', rows("SELECT HEX(JSON_OBJECT('id',id,'agent',agent_id,'name',name,"
     "'enabled',enabled,'prompt_chars',CHAR_LENGTH(system_prompt))) "
     "FROM ai_agent_role_voice_binding"))
emit('matching_role_prompts', rows("SELECT HEX(JSON_OBJECT('id',id,'name',name,'prompt',system_prompt)) "
     "FROM ai_agent_role_voice_binding WHERE LOWER(system_prompt) LIKE '%tvet%' "
     "OR system_prompt LIKE " + sql('%联盟%') + " OR name LIKE " + sql('%小豹%')))
runtime = r'''
import json
from pathlib import Path
import yaml
cfg = yaml.safe_load(Path('config.yaml').read_text()) or {}
private = Path('data/.config.yaml')
if private.exists():
    cfg.update(yaml.safe_load(private.read_text()) or {})
path = Path(cfg.get('prompt_template') or 'agent-base-prompt.txt')
print(json.dumps({'live_prompt_template': str(path), 'language_rules':
    [line for line in path.read_text().splitlines() if '{{language}}' in line]}, ensure_ascii=False))
print(json.dumps({'log_files': [p.name for p in Path('tmp').glob('*.log')]}))
'''
run = subprocess.run(['docker', 'exec', 'xiaozhi-esp32-server', 'python', '-c', runtime],
                     capture_output=True, text=True, timeout=20, check=True)
print(run.stdout)
logs = subprocess.run(['docker', 'logs', '--since', '168h', '--tail', '150000',
                       'xiaozhi-esp32-server'], stdout=subprocess.PIPE,
                      stderr=subprocess.STDOUT, text=True, timeout=30, check=True).stdout
hits = [line for line in logs.splitlines() if re.search(r'tvet|league|联盟|小豹', line, re.I)]
emit('live_container_log_keyword_hits', len(hits))
for agent in agents:
    for identifier in {agent['asr'], agent['tts']} - {None}:
        for row in rows("SELECT HEX(JSON_OBJECT('id',id,'config',config_json)) FROM "
                        "ai_model_config WHERE id=" + sql(identifier)):
            cfg = row['config']
            if isinstance(cfg, str):
                cfg = json.loads(cfg)
            keep = ['type', 'model', 'model_name', 'language', 'language_hints', 'voice',
                    'speaker', 'voice_id', 'resource_id', 'sample_rate']
            emit('speech_settings', {'id': row['id'], 'settings':
                 {key: cfg[key] for key in keep if key in cfg}})
