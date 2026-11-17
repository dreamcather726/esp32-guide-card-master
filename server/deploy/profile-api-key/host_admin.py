"""Scoped profile-key administration, run explicitly as root on the API host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

sys.path.insert(0, '/opt/xiaozhi-server/verify-20261007-user-switch')
from smoke_api import database, sql

ROOT = Path('/opt/xiaozhi-server')
TARGET = ROOT / 'manager-api/xiaozhi-esp32-api.jar'
CONTAINER = 'xiaozhi-esp32-server-web'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def healthy():
    try:
        with urllib.request.urlopen('http://127.0.0.1:8004/xiaozhi/user/pub-config', timeout=3) as response:
            return response.status == 200 and json.load(response).get('code') == 0
    except Exception:
        return False


def wait_healthy():
    for _ in range(90):
        if healthy():
            return
        time.sleep(2)
    raise RuntimeError('API did not become healthy')


def jar_diff(staged):
    with zipfile.ZipFile(TARGET) as before, zipfile.ZipFile(staged) as after:
        old = {i.filename: i.CRC for i in before.infolist() if not i.is_dir()}
        new = {i.filename: i.CRC for i in after.infolist() if not i.is_dir()}
        return sorted(k for k in old.keys() | new.keys() if old.get(k) != new.get(k))


def deploy(args):
    folder = args.folder.resolve()
    staged = folder / 'xiaozhi-esp32-api.jar'
    if folder.parent != ROOT or not folder.name.startswith('deploy-'):
        raise RuntimeError('Unexpected deployment folder')
    if digest(TARGET) != args.expected_old or digest(staged) != args.expected_new:
        raise RuntimeError('JAR hash guard failed')
    differences = jar_diff(staged)
    exact = {'META-INF/MANIFEST.MF', 'BOOT-INF/classpath.idx', 'BOOT-INF/layers.idx',
        'META-INF/maven/xiaozhi/xiaozhi-esp32-api/pom.properties',
        'BOOT-INF/classes/db/changelog/db.changelog-master.yaml',
        'BOOT-INF/classes/db/changelog/202610091200.sql',
        'BOOT-INF/classes/xiaozhi/modules/security/config/ShiroConfig.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/controller/UserBindingController.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/controller/UserProfileController.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dao/ProfileApiKeyDao.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/entity/ProfileApiKeyEntity.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/service/ProfileApiKeyService.class',
        'BOOT-INF/classes/xiaozhi/modules/security/oauth2/ProfileApiKeyFilter.class',
        'BOOT-INF/classes/xiaozhi/modules/security/oauth2/ProfileApiKeyFilter$Identity.class'}
    unexpected = [name for name in differences if name not in exact]
    if unexpected:
        print(json.dumps({'unexpected_jar_changes': unexpected}))
        raise RuntimeError('Review unrelated JAR changes before deployment')
    backup = folder / 'backup'
    backup.mkdir(mode=0o700)
    shutil.copy2(TARGET, backup / TARGET.name)
    with (backup / 'database.sql').open('xb') as output:
        result = subprocess.run(['docker', 'exec', 'xiaozhi-esp32-server-db', 'sh', '-lc',
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysqldump -uroot --single-transaction --no-tablespaces xiaozhi_esp32_server'],
            stdout=output, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError('Database backup failed; deployment not started')
    state = {'old_sha256': args.expected_old, 'new_sha256': args.expected_new, 'changes': differences,
        'backup': str(backup), 'status': 'backed_up'}
    state_path = folder / 'state.json'
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    if digest(TARGET) != args.expected_old:
        raise RuntimeError('Production JAR changed during backup')
    subprocess.run(['docker', 'stop', CONTAINER], check=True)
    try:
        shutil.copy2(staged, TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True)
        wait_healthy()
        assert database("SELECT COUNT(*) FROM DATABASECHANGELOG WHERE ID='202610091200'") == '1'
        state['status'] = 'deployed_healthy'
    except Exception:
        subprocess.run(['docker', 'stop', CONTAINER], check=True)
        shutil.copy2(backup / TARGET.name, TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True)
        wait_healthy()
        state['status'] = 'rolled_back'
        state_path.write_text(json.dumps(state, indent=2) + '\n')
        raise
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps({'status': state['status'], 'jar_sha256': digest(TARGET)}))


def issue(args):
    output = args.output.resolve()
    if output.exists():
        raise RuntimeError('Refusing to overwrite an existing key delivery file')
    owner = database('SELECT id FROM sys_user WHERE username=' + sql(args.username) + ' AND status=1')
    if not owner.isdigit():
        raise RuntimeError('Expected exactly one enabled owning account')
    key_id = secrets.token_hex(16)
    key = 'vxp_' + key_id + '_' + secrets.token_hex(32)
    key_hash = hashlib.sha256(key.encode('ascii')).hexdigest()
    # Plaintext is an explicit one-time delivery artifact, not application storage.
    with output.open('x', encoding='utf-8') as stream:
        json.dump({'account': args.username, 'keyId': key_id, 'apiKey': key,
            'baseUrl': 'https://robot.suji.work/xiaozhi/user/profile/',
            'header': 'X-API-Key', 'methods': ['GET', 'PUT'], 'expiresAt': None}, stream, indent=2)
        stream.write('\n')
    try:
        database('INSERT INTO ai_profile_api_key(key_id,owner_id,secret_hash,label) VALUES (' +
            ','.join(map(sql, (key_id, int(owner), key_hash, args.label))) + ')')
    except Exception:
        output.unlink()
        raise
    print(json.dumps({'created_key_id': key_id, 'owner': owner, 'delivery_file': str(output)}))


def revoke(args):
    if not re.fullmatch('[0-9a-f]{32}', args.key_id):
        raise RuntimeError('Invalid key ID')
    database('UPDATE ai_profile_api_key SET revoked_at=COALESCE(revoked_at,NOW(3)) WHERE key_id=' + sql(args.key_id)
        + ' AND owner_id IN (SELECT id FROM sys_user WHERE username=' + sql(args.username) + ')')
    assert database('SELECT COUNT(*) FROM ai_profile_api_key WHERE key_id=' + sql(args.key_id)
        + ' AND revoked_at IS NOT NULL AND owner_id IN (SELECT id FROM sys_user WHERE username='
        + sql(args.username) + ')') == '1'
    print('KEY_REVOKED')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    actions = parser.add_subparsers(dest='action', required=True)
    command = actions.add_parser('deploy')
    command.add_argument('--folder', type=Path, required=True)
    command.add_argument('--expected-old', required=True)
    command.add_argument('--expected-new', required=True)
    command.set_defaults(run=deploy)
    command = actions.add_parser('issue')
    command.add_argument('--username', required=True)
    command.add_argument('--label', required=True)
    command.add_argument('--output', type=Path, required=True)
    command.set_defaults(run=issue)
    command = actions.add_parser('revoke')
    command.add_argument('--username', required=True)
    command.add_argument('--key-id', required=True)
    command.set_defaults(run=revoke)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
