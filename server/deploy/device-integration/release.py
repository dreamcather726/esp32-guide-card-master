"""Guarded API, additive-schema and voice rollout; no firmware or model changes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import time
import yaml

sys.path.insert(0, '/opt/xiaozhi-server/deploy-20261009-profile-api-key')
from host_admin import TARGET, CONTAINER, digest, jar_diff, wait_healthy, database, sql

HERE=Path(__file__).resolve().parent
ROOT=Path('/opt/xiaozhi-server')
VOICE='xiaozhi-esp32-server'
OVERRIDE=ROOT/'docker-compose.user-binding.yml'
NEW='xiaozhi-esp32-server:custom-20261020-integration'
BASE='xiaozhi-esp32-server:rollback-20261020-integration'
STATE=HERE/'state.json'
API_PROXY=Path('/www/server/panel/vhost/nginx/xiaozhi-api-proxy.conf')
PUBLIC=Path('/www/server/panel/vhost/nginx/robot.suji.work.conf')
EXTENSION=Path('/www/server/panel/vhost/nginx/extension/robot.suji.work/integration-api.conf')
NGINX='/www/server/nginx/sbin/nginx'


def run(*args): return subprocess.check_output(args,text=True).strip()
def save(state): STATE.write_text(json.dumps(state,indent=2)+'\n')
def load(): return json.loads(STATE.read_text())
def info():
    entry=json.loads(run('docker','inspect',VOICE))[0]
    return {'image':entry['Image'],'tag':entry['Config']['Image']}


def hashes():
    names=list(json.loads((HERE/'manifest.json').read_text())['runtime'])
    program='import hashlib,json,pathlib; names='+repr(names)+'; print(json.dumps({p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() if pathlib.Path(p).exists() else None for p in names}))'
    return json.loads(run('docker','exec',VOICE,'python','-c',program))


def recreate():
    subprocess.run(['docker','compose','-f',str(ROOT/'docker-compose_all.yml'),'-f',str(OVERRIDE),
                    'up','-d','--no-deps','--force-recreate',VOICE],check=True,cwd=ROOT)


def wait_voice():
    for _ in range(90):
        try:
            assert run('docker','inspect',VOICE,'--format','{{.State.Running}}')=='true'
            for port in (8000,8003):
                with socket.create_connection(('127.0.0.1',port),timeout=1): pass
            return
        except (OSError,AssertionError): time.sleep(1)
    raise RuntimeError('Voice startup deadline')


def prepare():
    assert not STATE.exists(), 'Release already prepared'
    manifest=json.loads((HERE/'manifest.json').read_text())
    assert digest(TARGET)==manifest['base_jar'] and digest(HERE/TARGET.name)==manifest['new_jar']
    diff=jar_diff(HERE/TARGET.name)
    assert not set(diff)-set(manifest['jar_changes']), 'Unexpected JAR changes'
    for name,value in manifest['runtime'].items(): assert digest(HERE/name)==value
    old=info()
    assert old['tag']=='xiaozhi-esp32-server:custom-20261015-voice', 'Voice image changed since review'
    backup=HERE/'backup'; backup.mkdir(mode=0o700)
    shutil.copy2(TARGET,backup/TARGET.name); shutil.copy2(OVERRIDE,backup/OVERRIDE.name)
    with (backup/'database.sql').open('xb') as output:
        result=subprocess.run(['docker','exec','xiaozhi-esp32-server-db','sh','-lc',
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysqldump -uroot --single-transaction --no-tablespaces xiaozhi_esp32_server'],
            stdout=output,stderr=subprocess.PIPE)
        assert result.returncode==0, 'Database backup failed'
    state={'status':'backed_up','old':old,'old_hashes':hashes(),'old_jar':digest(TARGET),
           'new_jar':manifest['new_jar'],'old_compose':digest(OVERRIDE),'new_hashes':manifest['runtime'],
           'jar_changes':diff}
    save(state)
    run('docker','tag',old['image'],BASE)
    subprocess.run(['docker','build','--network=host','--build-arg','BASE_IMAGE='+BASE,'-t',NEW,str(HERE)],check=True)
    state['new_image']=run('docker','image','inspect',NEW,'--format','{{.Id}}')
    state['status']='built_tested'; save(state)
    print(json.dumps({'status':state['status'],'jar_changed_entries':len(diff),'image':state['new_image']}))
    directory=database("SELECT HEX(JSON_OBJECT('owner',u.id,'username',u.username)) FROM sys_user u "
                       "WHERE u.status=1 AND u.id IN (SELECT owner_id FROM ai_profile_api_key WHERE revoked_at IS NULL)")
    print(json.dumps({'eligible_owners':[json.loads(bytes.fromhex(v)) for v in directory.splitlines() if v]}))


def rollback(state):
    if 'proxy_backup' in state:
        API_PROXY.write_bytes((HERE/'backup'/API_PROXY.name).read_bytes())
        EXTENSION.unlink(missing_ok=True)
        subprocess.run([NGINX,'-t'],check=True,capture_output=True)
        subprocess.run([NGINX,'-s','reload'],check=True,capture_output=True)
    subprocess.run(['docker','stop',CONTAINER],check=True)
    shutil.copy2(HERE/'backup'/TARGET.name,TARGET)
    subprocess.run(['docker','start',CONTAINER],check=True); wait_healthy()
    shutil.copy2(HERE/'backup'/OVERRIDE.name,OVERRIDE); recreate(); wait_voice()
    assert info()['image']==state['old']['image'] and digest(TARGET)==state['old_jar']
    state['status']='rolled_back'; save(state)


def deploy():
    state=load(); assert state['status']=='built_tested'
    assert digest(TARGET)==state['old_jar'] and info()==state['old'] and hashes()==state['old_hashes']
    assert digest(OVERRIDE)==state['old_compose']
    try:
        subprocess.run(['docker','stop',CONTAINER],check=True)
        shutil.copy2(HERE/TARGET.name,TARGET)
        subprocess.run(['docker','start',CONTAINER],check=True); wait_healthy()
        assert database("SELECT COUNT(*) FROM DATABASECHANGELOG WHERE ID='202610191200'")=='1'
        cfg=yaml.safe_load(OVERRIDE.read_text()); cfg['services'][VOICE]['image']=NEW
        OVERRIDE.write_text(yaml.safe_dump(cfg,sort_keys=False)); recreate(); wait_voice()
        assert info()['image']==state['new_image'] and hashes()==state['new_hashes']
        state['status']='deployed_pending_smoke'; save(state)
    except Exception:
        rollback(state); raise
    print(json.dumps({'status':state['status']}))


def publish():
    state=load(); assert state['status']=='deployed_pending_smoke'
    assert json.loads((HERE/'smoke-result.json').read_text())['passed'] is True
    before=API_PROXY.read_bytes()
    anchor=b'    include /opt/xiaozhi-server/deploy-20261009-profile-api-key/nginx-profile-location.conf;'
    assert before.count(anchor)==1 and not EXTENSION.exists()
    assert b'include /www/server/panel/vhost/nginx/extension/robot.suji.work/*.conf;' in PUBLIC.read_bytes()
    shutil.copy2(API_PROXY,HERE/'backup'/API_PROXY.name)
    state['proxy_backup']=True; save(state)
    try:
        API_PROXY.write_bytes(before.replace(anchor,anchor+b'\n    include '+str(HERE/'nginx-integration-location.conf').encode()+b';'))
        shutil.copy2(HERE/'nginx-integration-location.conf',EXTENSION)
        subprocess.run([NGINX,'-t'],check=True,capture_output=True)
        subprocess.run([NGINX,'-s','reload'],check=True,capture_output=True)
    except Exception:
        API_PROXY.write_bytes(before); EXTENSION.unlink(missing_ok=True)
        subprocess.run([NGINX,'-s','reload'],check=True,capture_output=True); raise
    state['status']='published_pending_public_smoke'; save(state)
    print(json.dumps({'status':state['status']}))


def refresh():
    state=load()
    assert state['status'] in {'deployed_pending_smoke','published_pending_public_smoke'}
    assert digest(TARGET)==state['new_jar'] and hashes()==state['new_hashes'] and info()['image']==state['new_image']
    manifest=json.loads((HERE/'manifest.json').read_text())
    assert manifest['base_jar']==state['old_jar'] and digest(HERE/TARGET.name)==manifest['new_jar']
    assert not set(jar_diff(HERE/TARGET.name))-set(manifest['jar_changes'])
    for name,value in manifest['runtime'].items(): assert digest(HERE/name)==value
    subprocess.run(['docker','build','--network=host','--build-arg','BASE_IMAGE='+BASE,'-t',NEW,str(HERE)],check=True)
    image=run('docker','image','inspect',NEW,'--format','{{.Id}}')
    try:
        subprocess.run(['docker','stop',CONTAINER],check=True)
        shutil.copy2(HERE/TARGET.name,TARGET)
        subprocess.run(['docker','start',CONTAINER],check=True); wait_healthy()
        recreate(); wait_voice()
        assert info()['image']==image and hashes()==manifest['runtime'] and digest(TARGET)==manifest['new_jar']
        state.update(new_image=image,new_hashes=manifest['runtime'],new_jar=manifest['new_jar'])
        for name in ('smoke-result.json','public-smoke-result.json'): (HERE/name).unlink(missing_ok=True)
        state['status']='published_pending_public_smoke' if state.get('proxy_backup') else 'deployed_pending_smoke'
        save(state)
    except Exception:
        rollback(state); raise
    print(json.dumps({'status':state['status']}))


def issue(owner):
    assert load()['status']=='accepted'
    assert database('SELECT COUNT(*) FROM sys_user WHERE status=1 AND id='+sql(owner))=='1'
    output=HERE/'customer-api-key.json'; assert not output.exists()
    identifier=secrets.token_hex(16); key='vxp_'+identifier+'_'+secrets.token_hex(32)
    scopes='profile,roles:read,roles:write,device:role,history:read,history:delete'
    output.write_text(json.dumps({'ownerId':str(owner),'header':'X-API-Key','apiKey':key,
        'baseUrl':'https://robot.suji.work/xiaozhi/integration','scopes':scopes.split(','),'expiresAt':None},indent=2)+'\n')
    try:
        database('INSERT INTO ai_profile_api_key(key_id,owner_id,secret_hash,label,scopes) VALUES ('+
            ','.join(map(sql,(identifier,owner,hashlib.sha256(key.encode()).hexdigest(),'Device integration 20261020',scopes)))+')')
    except Exception: output.unlink(); raise
    print(json.dumps({'key_id':identifier,'delivery_file':str(output),'owner':owner}))


if __name__=='__main__':
    os.umask(0o077); assert HERE==ROOT/'deploy-20261020-device-integration'
    parser=argparse.ArgumentParser(); parser.add_argument('action',choices=['prepare','deploy','publish','refresh','accept','rollback','issue-key'])
    parser.add_argument('--owner',type=int); args=parser.parse_args()
    if args.action=='prepare': prepare()
    elif args.action=='deploy': deploy()
    elif args.action=='publish': publish()
    elif args.action=='refresh': refresh()
    elif args.action=='rollback': rollback(load())
    elif args.action=='issue-key': issue(args.owner)
    else:
        state=load(); assert state['status']=='published_pending_public_smoke'
        assert json.loads((HERE/'smoke-result.json').read_text())['passed'] is True
        assert json.loads((HERE/'public-smoke-result.json').read_text())['passed'] is True
        assert hashes()==state['new_hashes'] and info()['image']==state['new_image'] and digest(TARGET)==state['new_jar']
        state['status']='accepted'; save(state); print(json.dumps({'status':'accepted'}))
