"""Real HTTP/MySQL tests with unique temporary accounts; never edits customer profiles."""
import hashlib
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql

BASE = os.environ.get('PROFILE_API_BASE', 'http://127.0.0.1:8004/xiaozhi')
checks = 0


def request(method, path, key=None, token=None, body=None, expected=0):
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
        payload = response.read()
        if expected == 'reject':
            assert response.status >= 400 or json.loads(payload)['code'] != 0, 'Request was not rejected'
        else:
            result = json.loads(payload)
            assert result.get('code') == expected, 'Unexpected result code: ' + str(result.get('code'))
            if key is not None:
                assert response.headers.get('Cache-Control') == 'no-store'
            checks += 1
            return result.get('data')
    checks += 1


def main():
    owner = int(time.time() * 1000000)
    other = owner + 1
    marker = uuid.uuid4().hex
    guid = 'key-smoke-' + marker
    path = '/user/profile/' + guid
    key_ids = [secrets.token_hex(16), secrets.token_hex(16)]
    keys = ['vxp_' + kid + '_' + secrets.token_hex(32) for kid in key_ids]
    token = secrets.token_hex(32)
    try:
        for uid, kid, key in zip((owner, other), key_ids, keys):
            database(f'INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES '
                f'({uid},{sql("profile-key-smoke-"+str(uid))},NULL,0,1,NOW(),NOW()); '
                'INSERT INTO ai_profile_api_key(key_id,owner_id,secret_hash,label) VALUES (' +
                ','.join(map(sql, (kid, uid, hashlib.sha256(key.encode()).hexdigest(), 'smoke-' + marker))) + ')')
        database(f'INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES '
            f'({owner},{owner},{sql(token)},DATE_ADD(NOW(),INTERVAL 15 MINUTE),NOW(),NOW())')
        profile = {'nickname': 'Fixture A', 'language': 'zh-CN', 'interests': 'history', 'explanationStyle': 'brief'}
        request('PUT', path, keys[0], body=dict(profile, ownerId=other))
        assert request('GET', path, keys[0])['profile']['nickname'] == 'Fixture A'
        assert request('GET', path, keys[1]) is None
        request('PUT', path, keys[1], body=dict(profile, nickname='Fixture B'))
        assert request('GET', path, keys[0])['profile']['nickname'] == 'Fixture A'
        assert request('GET', path, keys[1])['profile']['nickname'] == 'Fixture B'
        assert request('GET', path, token=token)['profile']['nickname'] == 'Fixture A'
        request('PUT', path, token=token, body=dict(profile, nickname='Via token'))
        assert request('GET', path, keys[0])['profile']['nickname'] == 'Via token'
        request('PUT', path, keys[0], body={'nickname': 'Replacement'})
        stored = request('GET', path, keys[0])['profile']
        assert stored['nickname'] == 'Replacement' and not stored.get('interests')
        request('PUT', path, keys[0], body=dict(profile, nickname='n' * 65), expected='reject')
        request('GET', '/user/profile/' + 'g' * 128, keys[0])
        request('GET', '/user/profile/' + 'g' * 129, keys[0], expected='reject')
        request('GET', path, expected='reject')
        request('GET', path + '?X-API-Key=' + keys[0], expected='reject')
        request('GET', path, 'bad-key', expected='reject')
        request('GET', path, keys[0][:-1] + ('a' if keys[0][-1] != 'a' else 'b'), expected='reject')
        request('GET', path, keys[0] + ',' + keys[0], expected='reject')
        request('GET', path, keys[0], token=token, expected='reject')
        for method in ('DELETE', 'POST', 'PATCH', 'HEAD'):
            request(method, path, keys[0], expected='reject')
        for suffix in ('/sessions', '/sessions/fixture/messages', '/', '%2Fsessions', ';suffix'):
            request('GET', path + suffix, keys[0], expected='reject')
        for method, endpoint in [('POST', '/device/02:fe:00:00:00:00/user-binding'),
                ('GET', '/device/02:fe:00:00:00:00/mqtt-binding'), ('GET', '/sys/user/page'),
                ('POST', '/config/user-binding/resolve'), ('POST', '/agent/chat-history/report')]:
            request(method, endpoint, keys[0], expected='reject')
        database(f'UPDATE sys_user SET status=0 WHERE id={owner}')
        request('GET', path, keys[0], expected='reject')
        database(f'UPDATE sys_user SET status=1 WHERE id={owner}')
        database('UPDATE ai_profile_api_key SET expires_at=DATE_SUB(NOW(3),INTERVAL 1 SECOND) WHERE key_id=' + sql(key_ids[0]))
        request('GET', path, keys[0], expected='reject')
        database('UPDATE ai_profile_api_key SET expires_at=NULL,revoked_at=NOW(3) WHERE key_id=' + sql(key_ids[0]))
        request('GET', path, keys[0], expected='reject')
        database(f'DELETE FROM sys_user WHERE id={other}')
        request('GET', path, keys[1], expected='reject')
        print(json.dumps({'passed': checks, 'real_http_mysql': True, 'customer_profiles_modified': False,
            'base': BASE}))
    finally:
        database(f'DELETE FROM ai_profile_api_key WHERE owner_id IN ({owner},{other}); '
            f'DELETE FROM ai_user_profile WHERE owner_id IN ({owner},{other}); '
            f'DELETE FROM sys_user_token WHERE user_id IN ({owner},{other}); '
            f'DELETE FROM sys_user WHERE id IN ({owner},{other})')
        print('PROFILE_KEY_FIXTURES_REMOVED')


if __name__ == '__main__':
    main()
