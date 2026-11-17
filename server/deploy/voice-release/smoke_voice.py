"""Real bound-session voice loopback, using disposable database fixtures only."""
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
import uuid

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql, clone, api

CLIENT = r'''
import asyncio, json, sys, time, uuid, yaml, websockets, opuslib_next
from core.auth import AuthManager
data = json.load(sys.stdin)
config = yaml.safe_load(open('data/.config.yaml'))
secret = config.get('server', {}).get('auth_key') or config['manager-api']['secret']
results = []

async def connect():
    client = str(uuid.uuid4())
    headers = {'Device-Id': data['mac'], 'Client-Id': client, 'User-Binding': '1',
        'Authorization': 'Bearer ' + AuthManager(secret).generate_token(client, data['mac'])}
    if data.get('legacy'):
        headers.pop('User-Binding')
    ws = await websockets.connect('ws://127.0.0.1:8000/xiaozhi/v1/', additional_headers=headers)
    hello = dict(data['binding']['command'], type='hello', transport='websocket', version=1,
        features={'user_binding': True},
        audio_params={'format':'opus', 'sample_rate':16000, 'channels':1, 'frame_duration':60})
    if data.get('legacy'):
        hello = {key:hello[key] for key in ('type','transport','version','audio_params')}
    await ws.send(json.dumps(hello))
    return ws

async def reply(ws, name, min_seconds=0, tail=None):
    start = time.monotonic()
    frames, stt, text = [], '', ''
    first = None
    stopped = False
    decoder = opuslib_next.Decoder(16000, 1)
    samples = 0
    while time.monotonic() - start < 180:
        raw = await asyncio.wait_for(ws.recv(), 35)
        if isinstance(raw, bytes):
            samples += len(decoder.decode(raw, 960)) // 2
            frames.append(raw)
            if first is None:
                first = time.monotonic() - start
            continue
        event = json.loads(raw)
        if event.get('type') == 'user_context':
            assert event.get('state') == 'ready', 'binding_rejected'
        if event.get('type') == 'stt':
            stt = event.get('text', '')
        if event.get('type') == 'tts':
            if event.get('state') == 'sentence_start':
                text += event.get('text', '')
            if event.get('state') == 'stop':
                stopped = True
                break
    assert stopped and frames and stt, 'incomplete_voice_turn_' + name
    assert samples / 16000 >= min_seconds, 'truncated_long_audio'
    if tail:
        assert tail in text, 'missing_answer_tail'
    if name.startswith('asr'):
        assert '北京' in stt, 'recognition_mismatch'
    results.append(dict(test=name, frames=len(frames), audio_seconds=round(samples/16000,2),
        first_audio_seconds=round(first,2), total_seconds=round(time.monotonic()-start,2),
        recognized_chars=len(stt), answer_chars=len(text), stopped=True))
    print(json.dumps(results[-1]), flush=True)
    return frames

async def replay(ws, frames, name):
    await ws.send(json.dumps({'type':'listen','state':'start','mode':'manual'}))
    for frame in frames:
        await ws.send(frame)
        await asyncio.sleep(.006)
    await ws.send(json.dumps({'type':'listen','state':'stop'}))
    await reply(ws, name)

async def run():
    ws = await connect()
    try:
        # Deliberately send before the server's asynchronous initialization completes.
        await ws.send(json.dumps({'type':'listen','state':'detect',
            'text':'请只朗读这句话，不要补充：北京是中国的首都。'}))
        seed = await reply(ws, 'text_before_ready', tail='北京')
        await replay(ws, seed, 'asr_first')
        await replay(ws, seed, 'asr_repeat_same_connection')
        if data.get('legacy'):
            print(json.dumps({'passed':True, 'legacy':True, 'results':results}), flush=True)
            return
        phrase = '现在正在检查较长语音是否能够连续播放，每一段都应完整到达设备。'
        await ws.send(json.dumps({'type':'listen','state':'detect',
            'text':'请逐字朗读下面全部内容，不要总结也不要省略：' + phrase * 8 + '本轮长语音测试结束。'}))
        await reply(ws, 'long_answer', min_seconds=25, tail='本轮长语音测试结束')
        print(json.dumps({'test':'idle_reconnect', 'waiting_seconds':185}), flush=True)
        await asyncio.sleep(185)
    finally:
        await ws.close()
    ws = await connect()
    try:
        await replay(ws, seed, 'asr_after_idle_reconnect_before_ready')
    finally:
        await ws.close()
    print(json.dumps({'passed':True, 'results':results}), flush=True)

asyncio.run(run())
'''


def main():
    os.umask(0o077)
    marker = uuid.uuid4().hex
    owner = int(time.time() * 1000000)
    token = secrets.token_hex(32)
    agent_id, device_id = uuid.uuid4().hex, uuid.uuid4().hex
    mac = '02:fe:' + ':'.join(marker[n:n+2] for n in range(0, 8, 2))
    guid = 'voice-release-' + marker
    legacy = '--legacy' in sys.argv
    try:
        source = database("SELECT a.id FROM ai_agent a JOIN ai_device d ON d.agent_id=a.id "
            "WHERE d.mac_address NOT LIKE '02:fe:%' ORDER BY d.last_connected_at DESC LIMIT 1")
        assert source
        database(f"INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES "
            f"({owner},{sql('voice-test-'+marker)},NULL,0,1,NOW(),NOW()); "
            f"INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES "
            f"({owner},{owner},{sql(token)},DATE_ADD(NOW(),INTERVAL 30 MINUTE),NOW(),NOW())")
        clone('ai_agent', source, agent_id, {'user_id':owner, 'agent_code':marker,
            'agent_name':'Voice release fixture', 'summary_memory':None,
            'system_prompt':'你是语音链路验证助手。用户要求朗读时逐字朗读，不要改写、重复或添加开场白。其他问题请用简短中文回答。'})
        database('INSERT INTO ai_device(id,user_id,mac_address,agent_id,alias,auto_update,board,app_version) VALUES (' +
            ','.join(map(sql, (device_id, owner, mac, agent_id, 'Voice release fixture', 0, 'test-fixture', 'test'))) + ')')
        api('PUT', '/user/profile/'+guid, token, {'nickname':'测试', 'language':'zh-CN',
            'interests':'语音验证', 'explanationStyle':'按要求完整朗读测试文本。'})
        binding = api('POST', '/device/'+mac+'/user-binding', token, {'bootID':'boot-'+marker,'userGUID':guid})
        process = subprocess.Popen(['docker','exec','-i','xiaozhi-esp32-server','python','-c',CLIENT],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        # communicate bounds both pipes and process lifetime; child writes no credentials.
        output, error = process.communicate(json.dumps({'mac':mac,'binding':binding,'legacy':legacy}), timeout=600)
        print(output, flush=True)
        if process.returncode:
            print(error[-2500:], flush=True)
            raise RuntimeError('voice_loopback_failed')
        result = json.loads(output.strip().splitlines()[-1])
        assert result['passed']
        if legacy:
            Path(__file__).with_name('smoke-legacy-result.json').write_text(json.dumps(result, indent=2))
            return
        path = '/user/profile/'+guid+'/sessions/'+binding['usageSessionID']+'/messages'
        records = []
        for _ in range(30):
            records = api('GET', path, token)
            if sum(row['chatType'] == 1 for row in records) >= 5 and sum(row['chatType'] == 2 for row in records) >= 5:
                break
            time.sleep(1)
        assert sum(row['chatType'] == 1 for row in records) >= 5, 'missing_user_history'
        assert sum(row['chatType'] == 2 for row in records) >= 5, 'missing_assistant_history'
        assert all(row['userGuid'] == guid for row in records)
        result['history_records'] = len(records)
        result['production_devices_modified'] = False
        Path(__file__).with_name('smoke-result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'history_records':len(records), 'passed':True}))
    finally:
        # Restrict cleanup to this run's randomly generated fixture identity.
        cleanup = "import sqlite3,sys; db=sqlite3.connect('data/report-outbox.sqlite3',timeout=5); " + \
            "db.execute(\"DELETE FROM report_outbox WHERE json_extract(payload,'$.macAddress')=?\",(sys.stdin.read().strip(),)); db.commit()"
        subprocess.run(['docker','exec','-i','xiaozhi-esp32-server','python','-c',cleanup],
                       input=mac, text=True, capture_output=True, timeout=15)
        database(f"DELETE FROM ai_agent_chat_history WHERE owner_id={owner} OR agent_id={sql(agent_id)}; "
            f"DELETE FROM ai_usage_session WHERE owner_id={owner}; "
            f"DELETE FROM ai_device_binding_state WHERE mac_address={sql(mac)}; "
            f"DELETE FROM ai_user_profile WHERE owner_id={owner}; "
            f"DELETE FROM ai_device WHERE id={sql(device_id)}; DELETE FROM ai_agent WHERE id={sql(agent_id)}; "
            f"DELETE FROM sys_user_token WHERE user_id={owner}; DELETE FROM sys_user WHERE id={owner}")
        print('VOICE_SMOKE_FIXTURES_REMOVED', flush=True)


if __name__ == '__main__':
    main()
