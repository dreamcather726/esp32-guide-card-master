"""Real HTTP/MySQL profile-field checks. Removes only the unique fixtures it creates."""
import hashlib
import json
import os
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql, clone

BASE = os.environ.get('PROFILE_API_BASE', 'http://127.0.0.1:8004/xiaozhi')
checks = 0


def api(method, path, key=None, token=None, body=None, reject=False):
    global checks
    headers = {'Content-Type': 'application/json'}
    if key is not None:
        headers['X-API-Key'] = key
    if token is not None:
        headers['Authorization'] = 'Bearer ' + token
    req = urllib.request.Request(BASE + path, method=method, headers=headers,
        data=json.dumps(body).encode() if body is not None else None)
    try:
        response = urllib.request.urlopen(req, timeout=15)
    except urllib.error.HTTPError as exc:
        response = exc
    with response:
        result = json.load(response)
        if reject:
            assert response.status >= 400 or result.get('code') != 0, 'Expected rejection'
        else:
            assert response.status == 200 and result.get('code') == 0, 'Unexpected API result code'
            if path.startswith('/user/profile/'):
                assert response.headers.get('Cache-Control') == 'no-store'
    checks += 1
    return result.get('data')


def main():
    owner = int(time.time() * 1000000)
    other = owner + 1
    marker = uuid.uuid4().hex
    guid = 'fields-smoke-' + marker
    path = '/user/profile/' + guid
    ids = [secrets.token_hex(16) for _ in range(2)]
    keys = ['vxp_' + i + '_' + secrets.token_hex(32) for i in ids]
    token = secrets.token_hex(32)
    agent, device = uuid.uuid4().hex, uuid.uuid4().hex
    mac = '02:fe:' + ':'.join(marker[i:i+2] for i in range(0, 8, 2))
    binding_path = '/device/' + mac + '/user-binding'
    secret = subprocess.check_output(['docker', 'exec', 'xiaozhi-esp32-server', 'python', '-c',
        "import yaml; print(yaml.safe_load(open('data/.config.yaml'))['manager-api']['secret'])"], text=True).strip()
    baseline = {'nickname': 'Fixture', 'language': 'zh-CN', 'interests': 'history', 'explanationStyle': 'brief'}
    try:
        for uid, kid, key in zip((owner, other), ids, keys):
            database(f'INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES '
                f'({uid},{sql("profile-fields-smoke-"+str(uid))},NULL,0,1,NOW(),NOW()); '
                'INSERT INTO ai_profile_api_key(key_id,owner_id,secret_hash,label) VALUES (' +
                ','.join(map(sql, (kid, uid, hashlib.sha256(key.encode()).hexdigest(), 'fields-smoke-' + marker))) + ')')
        database(f'INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES '
            f'({owner},{owner},{sql(token)},DATE_ADD(NOW(),INTERVAL 15 MINUTE),NOW(),NOW())')
        source = database('SELECT a.id FROM ai_agent a JOIN ai_device d ON d.agent_id=a.id '
            "WHERE d.mac_address NOT LIKE '02:fe:%' ORDER BY d.last_connected_at DESC LIMIT 1")
        assert source
        clone('ai_agent', source, agent, {'user_id': owner, 'agent_code': marker,
            'agent_name': 'Profile Fields Smoke Fixture', 'summary_memory': None})
        database('INSERT INTO ai_device(id,user_id,mac_address,agent_id,alias,auto_update,board,app_version) VALUES (' +
            ','.join(map(sql, (device, owner, mac, agent, 'Profile fields smoke', 0, 'test-fixture', 'test'))) + ')')

        legacy = api('PUT', path, keys[0], body=baseline)
        assert legacy['profile']['relationship'] is None
        assert legacy['profile']['usageCount'] == 0
        profile = dict(baseline, relationship='\u8001\u5a46', usageCount=7, ownerId=other)
        saved = api('PUT', path, keys[0], body=profile)
        assert saved['profile']['relationship'] == '\u8001\u5a46'
        assert type(saved['profile']['usageCount']) is int and saved['profile']['usageCount'] == 7
        assert api('GET', path, keys[0])['profile'] == saved['profile']
        assert api('GET', path, keys[1]) is None
        api('PUT', path, keys[1], body=dict(baseline, relationship='other-account', usageCount=99))
        assert api('GET', path, token=token)['profile']['usageCount'] == 7
        assert api('GET', path, keys[1])['profile']['usageCount'] == 99
        for body in (baseline, dict(baseline, relationship=None, usageCount=None)):
            updated = api('PUT', path, keys[0], body=body)['profile']
            assert updated['relationship'] == '\u8001\u5a46' and updated['usageCount'] == 7
        cleared = api('PUT', path, token=token, body=dict(baseline, relationship='', usageCount=0))['profile']
        assert cleared['relationship'] == '' and cleared['usageCount'] == 0
        maximum = api('PUT', path, keys[0], body=dict(baseline, relationship='r'*64, usageCount=2147483647))
        assert maximum['profile']['usageCount'] == 2147483647
        for bad in (-1, 1.5, 1.0, True, '3', '', 2147483648, {}, []):
            api('PUT', path, keys[0], body=dict(baseline, usageCount=bad), reject=True)
        api('PUT', path, keys[0], body=dict(baseline, relationship='r'*65), reject=True)
        assert api('GET', path, keys[0])['profileVersion'] == maximum['profileVersion']

        for relationship in ('\u6211\u81ea\u5df1', '\u8001\u5a46', '\u7238\u7238', 'custom relationship'):
            assert api('PUT', path, keys[0], body=dict(baseline, relationship=relationship, usageCount=7))['profile']['relationship'] == relationship
        first = api('POST', binding_path, token=token, body={'bootID': marker, 'userGUID': guid})
        assert first['profile']['usageCount'] == 7
        second = api('POST', binding_path, token=token, body={'bootID': marker, 'userGUID': guid, 'profile': baseline})
        assert second['profile']['usageCount'] == 7 and second['profile']['relationship'] == 'custom relationship'
        assert api('GET', path, keys[0])['profile']['usageCount'] == 7
        api('PUT', path, keys[0], body=dict(baseline, relationship='father', usageCount=12))
        current = api('GET', binding_path, token=token)
        assert current['profile']['usageCount'] == 7 and current['profile']['relationship'] == 'custom relationship'
        command = second['command']
        resolve = {k: command[k] for k in ('bootID', 'userGUID', 'bindingVersion', 'bindingToken')}
        resolve['macAddress'] = mac
        assert api('POST', '/config/user-binding/resolve', token=secret, body=resolve)['profile']['usageCount'] == 7
        third = api('POST', binding_path, token=token, body={'bootID': marker, 'userGUID': guid})
        assert third['profile']['usageCount'] == 12 and third['profile']['relationship'] == 'father'
        history = api('GET', '/user/profile/' + guid + '/sessions', token=token)
        original = next(row for row in history if row['id'] == first['usageSessionID'])
        assert json.loads(original['profileSnapshot'])['usageCount'] == 7
        legacy_snapshot = json.dumps(baseline)
        database('UPDATE ai_usage_session SET profile_snapshot=' + sql(legacy_snapshot) + ' WHERE id=' + sql(third['usageSessionID'])
            + f' AND owner_id={owner}')
        old = api('GET', binding_path, token=token)['profile']
        assert old.get('relationship') is None and old.get('usageCount') is None
        api('POST', binding_path, keys[0], body={'bootID': marker, 'userGUID': guid}, reject=True)
        api('GET', path + '/sessions', keys[0], reject=True)
        print(json.dumps({'passed': checks, 'base': BASE, 'real_http_mysql': True,
            'automatic_count_increment': False, 'customer_records_modified': False}))
    finally:
        database(f'DELETE FROM ai_usage_session WHERE owner_id={owner}; '
            f'DELETE FROM ai_device_binding_state WHERE mac_address={sql(mac)}; '
            f'DELETE FROM ai_device WHERE id={sql(device)}; DELETE FROM ai_agent WHERE id={sql(agent)}; '
            f'DELETE FROM ai_user_profile WHERE owner_id IN ({owner},{other}); '
            f'DELETE FROM ai_profile_api_key WHERE owner_id IN ({owner},{other}); '
            f'DELETE FROM sys_user_token WHERE user_id IN ({owner},{other}); '
            f'DELETE FROM sys_user WHERE id IN ({owner},{other})')
        print('PROFILE_FIELD_FIXTURES_REMOVED')


if __name__ == '__main__':
    main()
