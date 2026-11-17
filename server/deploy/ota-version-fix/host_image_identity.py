"""Deploy the two tested OTA metadata classes; preserve all other live JAR entries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import uuid
import zipfile

sys.path.insert(0, '/opt/xiaozhi-server/deploy-20261006-ota-pilot')
from host_pilot import database, literal, redis, post_check, health

HERE = Path(__file__).resolve().parent
TARGET = Path('/opt/xiaozhi-server/manager-api/xiaozhi-esp32-api.jar')
CONTAINER = 'xiaozhi-esp32-server-web'
BASE_SHA = '605a01f30b9e3805310d9a72966bb67ce0f4d33d23a797ea4150b11464915129'
REPLACEMENTS = {
    'BOOT-INF/classes/xiaozhi/modules/device/service/impl/OtaServiceImpl.class': 'OtaServiceImpl.class',
    'BOOT-INF/classes/xiaozhi/modules/device/service/FirmwareImageIdentity.class': 'FirmwareImageIdentity.class',
}
OTA_ID = '1457bed80eff466491a26e9f35fcffdb'
VERSION = '2.1.13-diag'
IMAGE = Path('/opt/xiaozhi-server/uploadfile/vanex-s3-2.1.13-diag-4273f51f4c92.bin')
IMAGE_SHA = '4273f51f4c92ca0cb50d22d3a6705417fea9321c0bef8665d0367ef2215bfdee'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    return database('SELECT id,type,version,size,firmware_path,update_date FROM ai_ota ORDER BY id')


def prepare():
    assert not (HERE / 'state.json').exists(), 'Existing release state; inspect before rerunning'
    assert digest(TARGET) == BASE_SHA, 'Live API changed'
    assert digest(IMAGE) == IMAGE_SHA
    assert database('SELECT version FROM ai_ota WHERE id=' + literal(OTA_ID)) == VERSION
    hashes = json.loads((HERE / 'classes.json').read_text())
    for filename in REPLACEMENTS.values():
        assert digest(HERE / filename) == hashes[filename], 'Class upload differs from tested build'
    shutil.copy2(TARGET, HERE / 'backup.jar')
    output = HERE / 'patched.jar'
    with zipfile.ZipFile(TARGET) as old, zipfile.ZipFile(output, 'w') as new:
        for item in old.infolist():
            data = (HERE / REPLACEMENTS[item.filename]).read_bytes() if item.filename in REPLACEMENTS else old.read(item)
            new.writestr(item, data)
        for entry, filename in REPLACEMENTS.items():
            if entry not in old.namelist():
                new.writestr(entry, (HERE / filename).read_bytes())
    with zipfile.ZipFile(TARGET) as old, zipfile.ZipFile(output) as new:
        assert not set(old.namelist()) - set(new.namelist())
        changed = [name for name in new.namelist() if name not in old.namelist() or old.read(name) != new.read(name)]
        assert set(changed) == set(REPLACEMENTS), 'Unexpected JAR changes'
    state = {'status': 'prepared', 'before_sha256': BASE_SHA, 'after_sha256': digest(output),
             'changed_entries': changed, 'ota_rows_before': snapshot()}
    (HERE / 'state.json').write_text(json.dumps(state, indent=2))
    print(json.dumps({'status': state['status'], 'changed_entries': changed}), flush=True)


def verify():
    assert digest(IMAGE) == IMAGE_SHA
    mac, device_id = '02:fa:22:13:16:01', uuid.uuid4().hex
    assert database('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + literal(mac)) == '0'
    tickets, checks = set(), []
    created = False
    try:
        database('INSERT INTO ai_device(id,mac_address,board,app_version,auto_update,alias) VALUES (' +
                 ','.join([literal(device_id), literal(mac), literal('jiuchuan-s3'), literal('2.1.6'),
                           '1', literal('temporary OTA identity regression')]) + ')')
        created = True
        for base in ('https://robot.suji.work', 'http://47.94.91.32:8002'):
            for version in ('2.1.6', '2.1.10', '2.1.12-diag', VERSION, '2.1.13', '2.1.15'):
                response = post_check(mac, version, base)
                assert response.get('websocket', {}).get('url') and response.get('server_time')
                fw = response['firmware']
                assert fw['version'] == VERSION
                offered = fw['url'].startswith('https://robot.suji.work/xiaozhi/otaMag/download/')
                if offered:
                    ticket = fw['url'].rsplit('/', 1)[-1]
                    tickets.add(ticket)
                    assert json.loads(redis('GET', 'ota:id:' + ticket)) == OTA_ID
                assert offered == (version in ('2.1.6', '2.1.10', '2.1.12-diag'))
                checks.append({'endpoint': base, 'current': version, 'download_offered': offered})
            env = dict(os.environ, OTA_SMOKE_BASE=base, OTA_SMOKE_IMAGE=str(IMAGE), OTA_SMOKE_ID=OTA_ID)
            smoke = subprocess.run([sys.executable, '/opt/xiaozhi-server/deploy-20261006-ota-pilot/smoke_download.py'],
                                   env=env, capture_output=True, text=True, timeout=180)
            assert smoke.returncode == 0, 'Range/download smoke failed'
            print(smoke.stdout.strip(), base, flush=True)
    finally:
        if created:
            database('DELETE FROM ai_device WHERE id=' + literal(device_id) + ' AND mac_address=' + literal(mac))
        for ticket in tickets:
            redis('DEL', 'ota:id:' + ticket)
    assert database('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + literal(mac)) == '0'
    (HERE / 'verification.json').write_text(json.dumps({'selection_checks': checks, 'range_checks': 20,
        'image_version': VERSION, 'image_sha256': IMAGE_SHA, 'physical_device_tested': False}, indent=2))
    print('SELECTION_OK', len(checks), 'temporary fixtures cleaned', flush=True)


def deploy():
    state = json.loads((HERE / 'state.json').read_text())
    assert state['status'] == 'prepared' and digest(TARGET) == state['before_sha256']
    assert digest(HERE / 'patched.jar') == state['after_sha256'] and snapshot() == state['ota_rows_before']
    try:
        subprocess.run(['docker', 'stop', CONTAINER], check=True, timeout=45)
        shutil.copy2(HERE / 'patched.jar', TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True, timeout=30)
        health()
        verify()
        assert snapshot() == state['ota_rows_before'], 'OTA metadata changed during verification'
        state['status'] = 'deployed-verified'
    except Exception:
        subprocess.run(['docker', 'stop', CONTAINER], check=True, timeout=45)
        shutil.copy2(HERE / 'backup.jar', TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True, timeout=30)
        health()
        state['status'] = 'rolled-back'
        (HERE / 'state.json').write_text(json.dumps(state, indent=2))
        raise
    (HERE / 'state.json').write_text(json.dumps(state, indent=2))
    print(json.dumps({'status': state['status'], 'api_sha256': digest(TARGET)}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=('prepare', 'verify', 'deploy'))
    globals()[parser.parse_args().action]()
