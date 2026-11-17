"""Real HTTP/MySQL checks with unique, isolated accounts; never edits customer data."""
import hashlib
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql

BASE = os.environ.get('PROFILE_API_BASE', 'http://127.0.0.1:8004/xiaozhi')
checks = 0


def api(path, key=None, token=None, method='GET', body=None, reject=False):
    global checks
    headers = {'Content-Type': 'application/json'}
    if key is not None:
        headers['X-API-Key'] = key
    if token is not None:
        headers['Authorization'] = 'Bearer ' + token
    req = urllib.request.Request(BASE + path, method=method, headers=headers,
        data=json.dumps(body).encode() if body is not None else None)
    try:
        response = urllib.request.urlopen(req, timeout=20)
    except urllib.error.HTTPError as exc:
        response = exc
    with response:
        raw = response.read()
        try:
            result = json.loads(raw)
        except ValueError:
            if reject and response.status >= 400:
                checks += 1
                return None
            raise AssertionError('Expected a JSON response') from None
        if reject:
            assert response.status >= 400 or result.get('code') != 0, 'Expected rejection'
        else:
            assert response.status == 200 and result.get('code') == 0, 'Unexpected API result'
            assert response.headers.get('Cache-Control') == 'no-store', 'Profile cache must be disabled'
        checks += 1
        return result.get('data')


def path(guid):
    return '/user/profile/' + urllib.parse.quote(guid, safe='')


def main():
    owner = int(time.time() * 1000000)
    other = owner + 1
    prefix = 'PrefixSmoke' + uuid.uuid4().hex
    ids = [secrets.token_hex(16) for _ in range(2)]
    keys = ['vxp_' + i + '_' + secrets.token_hex(32) for i in ids]
    token = secrets.token_hex(32)
    exact = path(prefix)
    query = exact + '?match=prefix'
    expected = [prefix] + [prefix + '-%02d' % i for i in range(29)]
    try:
        for uid, kid, key in zip((owner, other), ids, keys):
            database(f'INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES '
                f'({uid},{sql("prefix-smoke-"+str(uid))},NULL,0,1,NOW(),NOW()); '
                'INSERT INTO ai_profile_api_key(key_id,owner_id,secret_hash,label) VALUES (' +
                ','.join(map(sql, (kid, uid, hashlib.sha256(key.encode()).hexdigest(), 'prefix-smoke'))) + ')')
        database(f'INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES '
            f'({owner},{owner},{sql(token)},DATE_ADD(NOW(),INTERVAL 15 MINUTE),NOW(),NOW())')
        guids = [prefix] + [prefix + '-%02d' % i for i in range(35)]
        guids += [prefix + '_literal', prefix + 'Xliteral', 'not' + prefix, prefix.lower(), 'g' * 128]
        rows = [(owner, g, 'own-account', 'self', 3) for g in guids]
        rows += [(other, prefix, 'other-account', 'other', 99),
                 (other, prefix + '-only-other', 'other-account', 'other', 99)]
        database('INSERT INTO ai_user_profile(owner_id,user_guid,nickname,relationship,usage_count) VALUES ' +
                 ','.join('(' + ','.join(map(sql, row)) + ')' for row in rows))
        baseline = database(f'SELECT COUNT(*),SUM(profile_version) FROM ai_user_profile WHERE owner_id={owner}')

        for auth in ({'key': keys[0]}, {'token': token}):
            for suffix in ('', '&limit=1000&ownerId=' + str(other)):
                result = api(query + suffix, **auth)
                assert isinstance(result, list) and len(result) == 30
                assert [row['userGUID'] for row in result] == expected
                assert all(row['profile']['nickname'] == 'own-account' for row in result)
                assert all(row['profile']['usageCount'] == 3 for row in result)
                assert all(set(row['profile']) == {'nickname', 'language', 'interests',
                    'explanationStyle', 'relationship', 'usageCount'} for row in result)
        other_result = api(query, key=keys[1])
        assert [row['userGUID'] for row in other_result] == [prefix, prefix + '-only-other']
        assert all(row['profile']['usageCount'] == 99 for row in other_result)
        assert api(exact, key=keys[0])['userGUID'] == prefix
        assert api(exact + '?match=exact', key=keys[0])['profile']['nickname'] == 'own-account'
        assert api(path(prefix + '-missing'), key=keys[0]) is None
        assert api(path(prefix + '-missing') + '?match=prefix', key=keys[0]) == []
        assert api(path(prefix + '-only-other') + '?match=prefix', key=keys[0]) == []
        literal = api(path(prefix + '_') + '?match=prefix', key=keys[0])
        assert [row['userGUID'] for row in literal] == [prefix + '_literal']
        lower = api(path(prefix.lower()) + '?match=prefix', key=keys[0])
        assert [row['userGUID'] for row in lower] == [prefix.lower()]
        assert len(api(path('g' * 128) + '?match=prefix', key=keys[0])) == 1
        api(path('g' * 129) + '?match=prefix', key=keys[0], reject=True)
        api('/user/profile/?match=prefix', key=keys[0], reject=True)
        api(query, reject=True)
        api(query, key='invalid', reject=True)
        api(query, key=keys[0], token=token, reject=True)
        for suffix in ('/sessions', '/sessions/id/messages'):
            api(exact + suffix + '?match=prefix', key=keys[0], reject=True)
        for method in ('POST', 'DELETE', 'PATCH', 'HEAD'):
            api(query, key=keys[0], method=method, reject=True)
        for mode in ('contains', 'PREFIX', 'prefix,exact', 'prefix&match=exact'):
            api(exact + '?match=' + mode, key=keys[0], reject=True)
        api(query, key=keys[0], method='PUT', body={'nickname': 'must-not-save'}, reject=True)
        assert database(f'SELECT COUNT(*),SUM(profile_version) FROM ai_user_profile WHERE owner_id={owner}') == baseline
        assert api(exact, key=keys[0])['profile']['nickname'] == 'own-account'
        updated = api(exact, key=keys[0], method='PUT', body={'nickname': 'updated-exact'})
        assert updated['profile']['nickname'] == 'updated-exact'
        assert updated['profile']['relationship'] == 'self' and updated['profile']['usageCount'] == 3
        result = api(query, key=keys[0])
        assert result[0]['profile']['nickname'] == 'updated-exact'
        assert all(row['profile']['nickname'] == 'own-account' for row in result[1:])
        database(f'UPDATE sys_user SET status=0 WHERE id={owner}')
        api(query, key=keys[0], reject=True)
        database(f'UPDATE sys_user SET status=1 WHERE id={owner}')
        database('UPDATE ai_profile_api_key SET revoked_at=NOW(3) WHERE key_id=' + sql(ids[0]))
        api(query, key=keys[0], reject=True)
        print(json.dumps({'passed': checks, 'base': BASE, 'real_http_mysql': True,
                          'result_cap': 30, 'customer_records_modified': False}))
    finally:
        database(f'DELETE FROM ai_user_profile WHERE owner_id IN ({owner},{other}); '
                 f'DELETE FROM ai_profile_api_key WHERE owner_id IN ({owner},{other}); '
                 f'DELETE FROM sys_user_token WHERE user_id IN ({owner},{other}); '
                 f'DELETE FROM sys_user WHERE id IN ({owner},{other})')
        print('PROFILE_PREFIX_FIXTURES_REMOVED')


if __name__ == '__main__':
    main()
