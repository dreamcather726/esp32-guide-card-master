"""Real HTTP/MySQL/WebSocket checks using disposable, owner-isolated fixtures."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0,'/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql, clone, api as login_api

HERE=Path(__file__).resolve().parent
BASE='https://robot.suji.work/xiaozhi' if '--public' in sys.argv else 'http://127.0.0.1:8004/xiaozhi'
CHECKS=[]


def check(name, condition):
    assert condition, name
    CHECKS.append(name)
    print(json.dumps({'check':name,'passed':True}),flush=True)


def request(method,path,key=None,body=None,ok=True,secret=None,bearer=None):
    headers={'Content-Type':'application/json'}
    if key: headers['X-API-Key']=key
    if secret: headers['Authorization']='Bearer '+secret
    if bearer: headers['Authorization']='Bearer '+bearer
    base='http://127.0.0.1:8004/xiaozhi' if secret else BASE
    req=urllib.request.Request(base+path,method=method,headers=headers,
        data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req,timeout=30) as response: result=json.load(response)
    except urllib.error.HTTPError as e:
        if not ok: return None
        raise RuntimeError('HTTP integration failure: '+str(e.code)) from None
    if not ok:
        assert result.get('code')!=0, 'Unexpected permission success'
        return None
    assert result.get('code')==0, 'Application integration failure: '+str(result.get('code'))
    return result.get('data')


CLIENT=r'''
import asyncio,json,sys,time,uuid,urllib.request,yaml,websockets
from core.auth import AuthManager
data=json.load(sys.stdin)
cfg=yaml.safe_load(open('data/.config.yaml'))
base=cfg['manager-api']['url'].rstrip('/')
def api(method,path,body=None):
    req=urllib.request.Request(base+path,method=method,
        headers={'X-API-Key':data['key'],'Content-Type':'application/json'},
        data=json.dumps(body).encode() if body is not None else None)
    with urllib.request.urlopen(req,timeout=20) as response: result=json.load(response)
    assert result['code']==0,'client_api_failure'
    return result['data']
async def run():
    client=str(uuid.uuid4()); secret=cfg.get('server',{}).get('auth_key') or cfg['manager-api']['secret']
    headers={'Device-Id':data['mac'],'Client-Id':client,'User-Binding':'1',
        'Authorization':'Bearer '+AuthManager(secret).generate_token(client,data['mac'])}
    async with websockets.connect('ws://127.0.0.1:8000/xiaozhi/v1/',additional_headers=headers) as ws:
        await ws.send(json.dumps(dict(data['binding']['command'],type='hello',transport='websocket',version=1,
            features={'user_binding':True},audio_params={'format':'opus','sample_rate':16000,'channels':1,'frame_duration':60})))
        ready=False; initial=None
        for _ in range(30):
            raw=await asyncio.wait_for(ws.recv(),35)
            if isinstance(raw,bytes): continue
            event=json.loads(raw)
            if event.get('type')=='role_switch' and event.get('state')=='success': initial=event['roleId']
            if event.get('type')=='user_context':
                assert event.get('state')=='ready','user_context_not_ready'
                ready=True; break
        assert ready and initial==data['role1'],'persisted_role_not_restored'
        saved=await asyncio.to_thread(api,'PUT','/integration/devices/'+data['mac']+'/role-binding',
            {'agentId':data['agent'],'roleId':data['role2']})
        assert saved['runtimeStatus']=='applied','online_role_not_applied'
        switched=False
        for _ in range(20):
            raw=await asyncio.wait_for(ws.recv(),20)
            if not isinstance(raw,bytes):
                event=json.loads(raw)
                if event.get('type')=='role_switch' and event.get('state')=='success' and event.get('roleId')==data['role2']:
                    switched=True; break
        assert switched,'role_notification_missing'
        await ws.send(json.dumps({'type':'listen','state':'detect','text':'请只说一句：接口验证成功。'}))
        frames=0; stopped=False
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            raw=await asyncio.wait_for(ws.recv(),35)
            if isinstance(raw,bytes): frames+=1; continue
            event=json.loads(raw)
            if event.get('type')=='tts' and event.get('state')=='stop': stopped=True; break
        assert stopped and frames>0,'voice_regression'
        for _ in range(20):
            history=await asyncio.to_thread(api,'GET','/integration/devices/'+data['mac']+'/chat-history?limit=100')
            if int(history['total'])>0: break
            await asyncio.sleep(0.5)
        assert int(history['total'])>0,'voice_history_not_persisted'
        cleared=await asyncio.to_thread(api,'DELETE','/integration/devices/'+data['mac']+'/chat-history?confirm=true')
        assert cleared['runtimeStatus']=='applied','live_context_not_closed'
        try:
            while True: await asyncio.wait_for(ws.recv(),10)
        except websockets.exceptions.ConnectionClosed as exc:
            assert exc.code==1012,'unexpected_close_code'
    print(json.dumps({'client_passed':True,'frames':frames,'initial_role_restored':True,
        'online_role_applied':True,'live_context_closed':True}),flush=True)
asyncio.run(run())
'''


def main():
    os.umask(0o077)
    marker=uuid.uuid4().hex; owner=int(time.time()*1000000); foreign=owner+1
    agent, device, second, other, role1, role2, disabled=[uuid.uuid4().hex for _ in range(7)]
    mac='02:fd:'+':'.join(marker[i:i+2] for i in range(0,8,2))
    mac2='02:fc:'+':'.join(marker[i:i+2] for i in range(0,8,2))
    mac3='02:fb:'+':'.join(marker[i:i+2] for i in range(0,8,2))
    cab='99'+str(owner)[-13:]+'s3'; guid='integration-'+marker
    token=secrets.token_hex(32); keys=[]; message=uuid.uuid4().hex; message2=uuid.uuid4().hex; audio=uuid.uuid4().hex
    client_result={}
    def issue(scopes,key_owner=owner):
        kid=secrets.token_hex(16); key='vxp_'+kid+'_'+secrets.token_hex(32); keys.append(kid)
        database('INSERT INTO ai_profile_api_key(key_id,owner_id,secret_hash,label,scopes) VALUES ('+
            ','.join(map(sql,(kid,key_owner,hashlib.sha256(key.encode()).hexdigest(),'Disposable integration fixture',scopes)))+')')
        return key
    try:
        source=database("SELECT id FROM ai_agent WHERE user_id=2063542642570055681 AND id='ce9307e119034a219459334df1fff0e8'")
        assert source
        database('INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES '+
            ','.join('('+','.join(map(sql,(o,'integration-'+str(o),None,0,1)))+',NOW(),NOW())' for o in (owner,foreign)))
        database('INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES ('+
            ','.join(map(sql,(owner,owner,token)))+',DATE_ADD(NOW(),INTERVAL 30 MINUTE),NOW(),NOW())')
        clone('ai_agent',source,agent,{'user_id':owner,'agent_code':marker,'agent_name':'Integration fixture',
            'summary_memory':None,'chat_history_conf':1,'system_prompt':'You are an integration test assistant. Answer briefly.'})
        role_source=database('SELECT id FROM ai_agent_role_voice_binding WHERE agent_id='+sql(source)+' LIMIT 1')
        for rid,name,enabled in ((role1,'First fixture role',1),(role2,'Second fixture role',1),(disabled,'Disabled fixture role',0)):
            clone('ai_agent_role_voice_binding',role_source,rid,{'agent_id':agent,'name':name,'enabled':enabled,
                'system_prompt':'你是接口验证助手。用户要求复述时原样复述，回答简短。','image_url':None,'video_url':None,'audio_url':None})
        for did,dm,own in ((device,mac,owner),(second,mac2,owner),(other,mac3,foreign)):
            database('INSERT INTO ai_device(id,user_id,mac_address,agent_id,alias,auto_update,board,app_version) VALUES ('+
                ','.join(map(sql,(did,own,dm,agent,'Disposable integration fixture',0,'test-fixture','test')))+')')
        database('INSERT INTO ai_mqtt_binding_mapping(mac_address,cab_guid,mapping_id,device_id,owner_id) VALUES ('+
            ','.join(map(sql,(mac,cab,str(uuid.uuid4()),device,owner)))+')')
        key=issue('profile,roles:read,roles:write,device:role,history:read,history:delete')
        read=issue('roles:read,history:read'); profile=issue('profile'); foreign_key=issue('roles:read,history:read',foreign)
        secret=run_secret()
        rows=request('GET','/integration/agents/'+agent+'/roles',key)
        check('all_roles_voice_fields_and_media',len(rows)==3 and all('voiceName' in row and 'imageUrl' in row for row in rows))
        check('console_bearer_login_supported',len(request('GET','/integration/agents/'+agent+'/roles',bearer=token))==3)
        request('GET','/integration/agents/'+agent+'/roles',None,ok=False)
        request('GET','/integration/agents/'+agent+'/roles',foreign_key,ok=False)
        request('GET','/integration/agents/'+agent+'/roles',profile,ok=False)
        check('anonymous_foreign_and_profile_only_keys_rejected',True)
        media={'imageUrl':'https://example.com/avatar.png','videoUrl':'https://example.com/avatar.mp4','audioUrl':'https://example.com/voice.mp3'}
        result=request('PUT',f'/integration/agents/{agent}/roles/{role1}/media',key,media)
        check('media_saved_and_returned',all(result[k]==v for k,v in media.items()))
        request('PUT',f'/integration/agents/{agent}/roles/{role1}/media',key,{'imageUrl':'file:///etc/passwd'},ok=False)
        result=request('PUT',f'/integration/agents/{agent}/roles/{role1}/media',key,{})
        check('media_clear_and_invalid_url_rejected',all(result[k] is None for k in media))
        body={'agentId':agent,'roleId':role1}
        result=request('PUT',f'/integration/devices/{device}/role-binding',key,body)
        check('offline_binding_saved_without_false_delivery',result['runtimeStatus']=='saved_device_offline')
        version=result['binding']['bindingVersion']
        result=request('PUT',f'/integration/devices/{device}/role-binding',key,body)
        check('binding_request_idempotent',result['binding']['bindingVersion']==version)
        for identifier in (mac,mac.upper().replace(':','-'),cab,cab[:-2]):
            check('device_identifier_'+identifier.split(':')[0],request('GET',f'/integration/devices/{identifier}/role-binding',key)['roleId']==role1)
        check('other_device_binding_untouched',request('GET',f'/integration/devices/{second}/role-binding',key) is None)
        request('PUT',f'/integration/devices/{device}/role-binding',read,body,ok=False)
        request('PUT',f'/integration/devices/{device}/role-binding',key,{'agentId':agent,'roleId':disabled},ok=False)
        request('GET',f'/integration/devices/{other}/chat-history',key,ok=False)
        check('readonly_disabled_and_cross_owner_device_rejected',True)
        cfg=request('POST','/config/agent-models',body={'macAddress':mac,'clientId':'fixture','selectedModule':{}},secret=secret)
        check('persisted_role_in_next_connection_config',cfg['deviceRoleId']==role1)
        login_api('PUT','/user/profile/'+guid,token,{'nickname':'Fixture','language':'zh-CN'})
        check('existing_profile_api_still_works',request('GET','/user/profile/'+guid,profile) is not None)
        binding=login_api('POST','/device/'+mac+'/user-binding',token,{'bootID':'boot-'+marker,'userGUID':guid})
        process=subprocess.run(['docker','exec','-i','xiaozhi-esp32-server','python','-c',CLIENT],
            input=json.dumps({'mac':mac,'key':key,'agent':agent,'role1':role1,'role2':role2,'binding':binding}),
            text=True,capture_output=True,timeout=180)
        (HERE/'private-client-output.log').write_text(process.stdout+'\n'+process.stderr)
        assert process.returncode==0,'Bound WebSocket fixture failed (see private service logs)'
        client_result=json.loads(process.stdout.strip().splitlines()[-1]); check('real_websocket_role_switch_voice_and_clear',client_result['client_passed'])
        check('cleared_history_empty',int(request('GET',f'/integration/devices/{device}/chat-history',key)['total'])==0)
        report={'macAddress':mac,'messageId':message,'userGuid':guid,'bootId':'boot-'+marker,
            'usageSessionId':binding['usageSessionID'],'bindingVersion':binding['bindingVersion'],
            'sessionId':'fixture-'+marker,'chatType':1,'content':'Disposable new history',
            'reportTime':int(time.time()*1000)+1,'historyGeneration':0}
        check('stale_live_generation_acknowledged_and_discarded',request('POST','/agent/chat-history/report',body=report,secret=secret) is True
            and int(request('GET',f'/integration/devices/{device}/chat-history',key)['total'])==0)
        cfg=request('POST','/config/agent-models',body={'macAddress':mac,'clientId':'fixture','selectedModule':{}},secret=secret)
        report['historyGeneration']=int(cfg['deviceHistoryGeneration']); report['reportTime']=int(time.time()*1000)+2
        request('POST','/agent/chat-history/report',body=report,secret=secret)
        rows=request('GET',f'/integration/devices/{device}/chat-history?page=1&limit=1',key)
        check('new_generation_report_visible_and_paginated',int(rows['total'])==1 and len(rows['list'])==1)
        check('history_response_omits_profile_snapshot', 'profileSnapshot' not in rows['list'][0])
        report['messageId']=message2
        request('POST','/agent/chat-history/report',body=report,secret=secret)
        report['messageId']=message
        database('INSERT INTO ai_agent_chat_audio(id,audio) VALUES ('+sql(audio)+",X'74657374'); UPDATE ai_agent_chat_history SET audio_id="+sql(audio)+' WHERE message_id IN ('+sql(message)+','+sql(message2)+')')
        request('DELETE',f'/integration/devices/{device}/chat-history',key,ok=False)
        request('DELETE',f'/integration/devices/{device}/chat-history?confirm=true',read,ok=False)
        result=request('DELETE',f'/integration/devices/{device}/chat-history?confirm=true',key)
        check('clear_confirmation_scope_and_shared_same_device_audio_deletion',int(result['deletedCount'])==2
            and database('SELECT COUNT(*) FROM ai_agent_chat_audio WHERE id='+sql(audio))=='0')
        report['historyGeneration']=int(time.time()*1000)+1000; report['reportTime']=int(time.time()*1000)+1000
        request('POST','/agent/chat-history/report',body=report,secret=secret)
        check('deleted_message_id_never_resurrects',int(request('GET',f'/integration/devices/{device}/chat-history',key)['total'])==0)
        request('GET',f'/integration/devices/{device}/chat-history?limit=101',key,ok=False)
        check('pagination_limit_enforced',True)
        output='public-smoke-result.json' if '--public' in sys.argv else 'smoke-result.json'
        (HERE/output).write_text(json.dumps({'passed':True,'checks':CHECKS,'public':'--public' in sys.argv,
            'production_devices_modified':False,'voice_frames':client_result.get('frames')},indent=2)+'\n')
    finally:
        cleanup="import sqlite3,sys; db=sqlite3.connect('data/report-outbox.sqlite3',timeout=5); db.execute(\"DELETE FROM report_outbox WHERE json_extract(payload,'$.macAddress')=?\",(sys.stdin.read().strip(),)); db.commit()"
        subprocess.run(['docker','exec','-i','xiaozhi-esp32-server','python','-c',cleanup],input=mac,text=True,capture_output=True,timeout=15)
        database('DELETE FROM ai_agent_chat_history WHERE agent_id='+sql(agent)+'; DELETE FROM ai_chat_history_tombstone WHERE message_id IN ('+sql(message)+','+sql(message2)+')'+
            '; DELETE FROM ai_device_role_binding WHERE device_id='+sql(device)+'; DELETE FROM ai_device_history_clear WHERE owner_id='+sql(owner)+
            '; DELETE FROM ai_agent_chat_audio WHERE id='+sql(audio)+'; DELETE FROM ai_usage_session WHERE owner_id='+sql(owner)+
            '; DELETE FROM ai_device_binding_state WHERE mac_address='+sql(mac)+'; DELETE FROM ai_mqtt_binding_mapping WHERE device_id='+sql(device)+
            '; DELETE FROM ai_user_profile WHERE owner_id='+sql(owner)+'; DELETE FROM ai_agent_user_memory WHERE agent_id='+sql(agent)+
            '; DELETE FROM ai_agent_role_voice_binding WHERE agent_id='+sql(agent)+'; DELETE FROM ai_device WHERE agent_id='+sql(agent)+
            '; DELETE FROM ai_agent WHERE id='+sql(agent)+'; DELETE FROM ai_profile_api_key WHERE owner_id IN ('+sql(owner)+','+sql(foreign)+')'+
            '; DELETE FROM sys_user_token WHERE user_id='+sql(owner)+'; DELETE FROM sys_user WHERE id IN ('+sql(owner)+','+sql(foreign)+')')
        print('INTEGRATION_FIXTURES_REMOVED',flush=True)


def run_secret():
    program="import yaml; print(yaml.safe_load(open('data/.config.yaml'))['manager-api']['secret'])"
    return subprocess.check_output(['docker','exec','xiaozhi-esp32-server','python','-c',program],text=True).strip()


if __name__=='__main__': main()
