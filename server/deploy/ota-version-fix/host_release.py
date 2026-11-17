"""Patch only version-comparison bytecode; leave all rollout and user data intact."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, '/opt/xiaozhi-server/deploy-20261009-profile-api-key')
from host_admin import TARGET, CONTAINER, digest, wait_healthy
sys.path.insert(0, '/opt/xiaozhi-server/deploy-20261006-ota-pilot')
from host_pilot import database, literal, redis

HERE = Path(__file__).resolve().parent
BASE_SHA = 'e1fd5ee70b71da4ef35e9f7a24d15d31e0b5a5475a7e7ec9848aa04be7998479'
ENTRY = 'BOOT-INF/classes/xiaozhi/modules/device/service/impl/DeviceServiceImpl.class'
STATE = HERE / 'state.json'


def scope():
    return database("SELECT param_value FROM sys_params WHERE param_code='server.ota_pilot'")


def extract():
    assert digest(TARGET) == BASE_SHA
    with zipfile.ZipFile(TARGET) as archive:
        (HERE / 'before.class').write_bytes(archive.read(ENTRY))
    print(json.dumps({'api_sha256': BASE_SHA, 'pilot_policy': scope()}))


def smoke():
    mac = '02:fa:21:12:00:01'
    assert database('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + literal(mac)) == '0'
    key = 'ota:activation:data:' + mac.replace(':', '_')
    assert redis('EXISTS', key) == '0'
    codes = set()
    results = []
    try:
        for base in ('http://127.0.0.1:8004', 'https://robot.suji.work'):
            for version in ('2.1.10', '2.1.11-diag', '2.1.12-diag'):
                body = json.dumps({'application': {'version': version}, 'board': {'type': 'jiuchuan-s3'}}).encode()
                request = urllib.request.Request(base + '/xiaozhi/ota/', body, {
                    'Content-Type': 'application/json', 'Device-Id': mac,
                    'Client-Id': 'version-compatibility-fixture', 'Activation-Version': '1'})
                with urllib.request.urlopen(request, timeout=20) as response:
                    data = json.load(response)
                assert data.get('server_time', {}).get('timestamp', 0) > 0
                assert data.get('websocket', {}).get('url')
                code = data.get('activation', {}).get('code')
                if code: codes.add(code)
                assert code and 'code' not in data and 'error' not in data
                results.append({'endpoint': base, 'version': version, 'configuration_valid': True})
    finally:
        redis('DEL', key)
        for code in codes:
            code_key = 'ota:activation:code:' + code
            value = redis('GET', code_key)
            if value.strip('"') == mac: redis('DEL', code_key)
    assert database('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + literal(mac)) == '0'
    return results


def deploy():
    assert not STATE.exists() and digest(TARGET) == BASE_SHA
    review = json.loads((HERE / 'bytecode-review.json').read_text())
    replacement = (HERE / 'after.class').read_bytes()
    assert review['changed_methods'] == ['private static int compareVersions(java.lang.String, java.lang.String);']
    assert review['after_sha256'] == hashlib.sha256(replacement).hexdigest()
    with zipfile.ZipFile(TARGET) as original:
        assert review['before_sha256'] == hashlib.sha256(original.read(ENTRY)).hexdigest()
    backup = HERE / 'backup.jar'
    shutil.copy2(TARGET, backup)
    staged = HERE / TARGET.name
    with zipfile.ZipFile(backup) as old, zipfile.ZipFile(staged, 'w') as new:
        for item in old.infolist():
            new.writestr(item, replacement if item.filename == ENTRY else old.read(item.filename))
    with zipfile.ZipFile(backup) as old, zipfile.ZipFile(staged) as new:
        assert old.namelist() == new.namelist()
        assert [k for k in old.namelist() if old.read(k) != new.read(k)] == [ENTRY]
    state = {'status': 'backed_up', 'old_sha256': BASE_SHA, 'new_sha256': digest(staged),
             'pilot_before': scope(), 'changed_entries': [ENTRY]}
    STATE.write_text(json.dumps(state, indent=2))
    assert digest(TARGET) == BASE_SHA
    subprocess.run(['docker', 'stop', CONTAINER], check=True)
    try:
        shutil.copy2(staged, TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True)
        wait_healthy()
        state['smoke'] = smoke()
        assert scope() == state['pilot_before']
        state['status'] = 'accepted'
    except Exception:
        subprocess.run(['docker', 'stop', CONTAINER], check=True)
        shutil.copy2(backup, TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True)
        wait_healthy()
        state['status'] = 'rolled_back'
        raise
    finally:
        STATE.write_text(json.dumps(state, indent=2))
    print(json.dumps(state))


if __name__ == '__main__':
    os.umask(0o077)
    assert HERE == Path('/opt/xiaozhi-server/deploy-20261021-ota-version-fix')
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['extract', 'deploy'])
    {'extract': extract, 'deploy': deploy}[parser.parse_args().action]()
