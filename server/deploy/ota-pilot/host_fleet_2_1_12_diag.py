"""Publish the verified diagnostic app; preserve board isolation and device opt-outs."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

ROOT = Path('/opt/xiaozhi-server')
STAGING = ROOT / 'deploy-20261021-ota-2.1.12-diag'
DEPLOY = ROOT / 'deploy-20261021-ota-2.1.12-diag-fleet'
sys.path.insert(0, str(STAGING))
import host_stage_2_1_11_diag as stage
from host_pilot import NO_UPDATE_URL, post_check

database, literal, redis = stage.database, stage.literal, stage.redis
VERSION = '2.1.12-diag'
SHA = 'd403b04908f589803aeba9d1b9d72efa2491cd13f72b228183de68e7509db0ae'
API_SHA = '605a01f30b9e3805310d9a72966bb67ce0f4d33d23a797ea4150b11464915129'
SIZE = 3235104
NAME = 'vanex-s3-2.1.12-diag-d403b04908f5.bin'
IMAGE = ROOT / 'uploadfile' / NAME
TYPE = 'jiuchuan-s3'
FIXTURES = ('02:fa:21:12:12:01', '02:fa:21:12:12:02')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def configure_image_check():
    stage.VERSION, stage.SHA, stage.SIZE = VERSION, SHA, SIZE
    stage.ELF_SHA = '1ffd187ea3355f486b233972a9a731e419e198e4962d5cbe013eec49aac14b68'


def save(state):
    temporary = DEPLOY / 'state.json.tmp'
    with temporary.open('w') as output:
        json.dump(state, output, indent=2)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(DEPLOY / 'state.json')


def load():
    return json.loads((DEPLOY / 'state.json').read_text())


def latest_id():
    return database("SELECT id FROM ai_ota WHERE type='jiuchuan-s3' ORDER BY update_date DESC LIMIT 1")


def metadata(ota_id):
    return database('SELECT type,version,size,firmware_path FROM ai_ota WHERE id=' + literal(ota_id))


def expected_metadata(board_type):
    return '\t'.join((board_type, VERSION, str(SIZE), '/uploadfile/' + NAME))


def download_smoke(ota_id, phase):
    for index, base in enumerate(stage.BASES):
        env = dict(os.environ, OTA_SMOKE_BASE=base, OTA_SMOKE_IMAGE=str(IMAGE), OTA_SMOKE_ID=ota_id)
        result = subprocess.run([sys.executable, str(STAGING / 'smoke_download.py')], env=env,
                                capture_output=True, text=True, timeout=600)
        (DEPLOY / ('%s-download-%d.log' % (phase, index))).write_text(result.stdout + result.stderr)
        require(result.returncode == 0, 'Download verification failed; inspect protected deployment log')
        print('DOWNLOAD_OK', phase, base, flush=True)


def prepare():
    require(not (DEPLOY / 'state.json').exists(), 'Deployment state exists; inspect before retrying')
    stage.health()
    stage.verify_image(IMAGE)
    staged = json.loads((STAGING / 'state.json').read_text())
    require(staged.get('verified') is True and staged['version'] == VERSION and
            staged['sha256'] == SHA and staged['size'] == SIZE, 'Staging verification mismatch')
    before = stage.snapshot()
    require(before == staged['before'], 'Production configuration changed since staging')
    require(before['api_sha256'] == API_SHA, 'Version-compatible API is not deployed')
    require(json.loads(before['pilot_config']) == {'enabled': False}, 'Unexpected pilot policy')
    require(metadata(staged['ota_id']) == expected_metadata('pilot-jiuchuan-s3'), 'Staged row mismatch')
    require(database("SELECT COUNT(*) FROM ai_ota WHERE type='jiuchuan-s3' AND version=" +
                     literal(VERSION)) == '0', 'Release already exists; do not duplicate')
    owner = database('SELECT creator FROM ai_ota WHERE id=' + literal(staged['ota_id']))
    require(owner.isdigit(), 'Release owner is ambiguous')
    previous_id = latest_id()
    require(database('SELECT version FROM ai_ota WHERE id=' + literal(previous_id)) == '2.1.10',
            'Unexpected previous release')
    state = {'ota_id': uuid.uuid4().hex, 'version': VERSION, 'sha256': SHA, 'size': SIZE,
             'staged_id': staged['ota_id'], 'owner': owner, 'before': before,
             'previous_latest_id': previous_id, 'status': 'preparing', 'prepared_at': stage.now(),
             'board_type': TYPE, 'physical_device_tested': False, 'forced_reboots': False,
             'device_auto_update_settings_modified': False}
    save(state)
    (DEPLOY / 'device-settings-before.tsv').write_text(database(
        'SELECT id,board,auto_update FROM ai_device ORDER BY id') + '\n')
    download_smoke(staged['ota_id'], 'preflight')
    require(stage.snapshot() == before, 'Production changed during preflight')
    state['status'] = 'ready-to-publish'
    save(state)
    print('PREPARED; no public release or device settings changed', flush=True)


def assert_published(state):
    current = stage.snapshot()
    require(latest_id() == state['ota_id'], 'Latest public release changed')
    require(metadata(state['ota_id']) == expected_metadata(TYPE), 'Public image metadata mismatch')
    require(current['pilot_config'] == state['before']['pilot_config'], 'Pilot policy changed')
    require(current['api_sha256'] == API_SHA, 'API changed since preflight')
    remaining = '\n'.join(row for row in current['public_rows'].splitlines()
                          if not row.startswith(state['ota_id'] + '\t'))
    require(remaining == state['before']['public_rows'], 'Other public releases changed')
    stage.verify_image(IMAGE)


def selection_smoke(state):
    ids = [uuid.uuid4().hex for _ in FIXTURES]
    tickets = set()
    created = False
    counts = {}
    try:
        for mac in FIXTURES:
            require(database('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + literal(mac)) == '0',
                    'Fixture MAC collision; no customer row may be reused')
        sql = 'START TRANSACTION;\n'
        for item, mac in zip(ids, FIXTURES):
            values = [literal(item), literal(mac), literal(TYPE), literal('2.1.0'), '1',
                      literal('temporary 2.1.12 diagnostic OTA verification')]
            sql += ('INSERT INTO ai_device(id,mac_address,board,app_version,auto_update,alias) VALUES (' +
                    ','.join(values) + ');\n')
        database(sql + 'COMMIT;')
        created = True
        for base in stage.BASES:
            count = 0

            def check(mac, version, board=TYPE):
                nonlocal count
                result = post_check(mac, version, base, board)
                url = result.get('firmware', {}).get('url', '')
                if url.startswith('https://robot.suji.work/xiaozhi/otaMag/download/'):
                    tickets.add(url.rsplit('/', 1)[-1])
                require(result.get('server_time') and result.get('websocket', {}).get('url'),
                        'Configuration/time missing from OTA response')
                count += 1
                return result

            for mac in FIXTURES:
                for old in ('2.1.0', '2.1.10', '2.1.11-diag'):
                    firmware = check(mac, old)['firmware']
                    require(firmware['version'] == VERSION, 'Older device did not receive selected version')
                    require(firmware['url'].startswith('https://robot.suji.work/xiaozhi/otaMag/download/'),
                            'Unexpected public download origin')
                    ticket = firmware['url'].rsplit('/', 1)[-1]
                    require(json.loads(redis('GET', 'ota:id:' + ticket)) == state['ota_id'],
                            'Download ticket points to a different release')
            for installed in (VERSION, '2.1.12', '2.1.13'):
                require(check(FIXTURES[0], installed)['firmware']['url'] == NO_UPDATE_URL,
                        'Same or newer device offered an upgrade/downgrade')
            database('UPDATE ai_device SET auto_update=0 WHERE id=' + literal(ids[0]))
            require('firmware' not in check(FIXTURES[0], '2.1.0'), 'Device opt-out ignored')
            database('UPDATE ai_device SET auto_update=1 WHERE id=' + literal(ids[0]))
            require(check(FIXTURES[1], '2.1.0', 'unrelated-board')['firmware']['url'] == NO_UPDATE_URL,
                    'Unrelated board offered S3 firmware')
            require('firmware' not in check(FIXTURES[1], '2.1.0', 'pilot-jiuchuan-s3'),
                    'Staging board type exposed through public lookup')
            counts[base] = count
            print('SELECTION_OK', base, count, 'checks; opt-outs/board isolation preserved', flush=True)
    finally:
        try:
            if created:
                time.sleep(1)
                for item, mac in zip(ids, FIXTURES):
                    database('DELETE FROM ai_device WHERE id=' + literal(item) +
                             ' AND mac_address=' + literal(mac))
                for item in ids:
                    require(database('SELECT COUNT(*) FROM ai_device WHERE id=' + literal(item)) == '0',
                            'Fixture cleanup incomplete')
        finally:
            for ticket in tickets:
                redis('DEL', 'ota:id:' + ticket, 'ota:download:count:' + ticket)
    return counts


def rollback():
    state = load()
    require(metadata(state['ota_id']) == expected_metadata(TYPE), 'Release metadata changed; inspect first')
    sql = ("UPDATE ai_ota SET type='pilot-withdrawn-jiuchuan-s3',update_date=NOW() WHERE id=" +
           literal(state['ota_id']) + " AND type='jiuchuan-s3' AND version=" + literal(VERSION) +
           ' AND size=' + str(SIZE) + ' AND firmware_path=' + literal('/uploadfile/' + NAME) +
           '; SELECT ROW_COUNT();')
    require(database(sql) == '1', 'Withdrawal was not applied')
    state.update(status='withdrawn', withdrawn_at=stage.now())
    save(state)
    print('WITHDRAWN; no device settings or other releases changed; installed devices not downgraded', flush=True)


def verify_checks(state):
    assert_published(state)
    state['selection_checks'] = selection_smoke(state)
    download_smoke(state['ota_id'], 'public')
    stage.health()
    assert_published(state)
    state.update(status='fleet-active-verified', verified_at=stage.now())
    save(state)
    print('FLEET_VERIFIED', VERSION, TYPE, state['ota_id'], flush=True)


def verify():
    state = load()
    try:
        verify_checks(state)
    except Exception:
        rollback()
        raise


def publish():
    state = load()
    require(state['status'] == 'ready-to-publish', 'Successful preflight required')
    stage.health()
    stage.verify_image(IMAGE)
    require(stage.snapshot() == state['before'], 'Production changed since preflight')
    require(metadata(state['staged_id']) == expected_metadata('pilot-jiuchuan-s3'), 'Staged row changed')
    values = [literal(state['ota_id']), literal('Vanex S3 ' + VERSION + ' - customer-requested OTA'),
              literal(TYPE), literal(VERSION), str(SIZE),
              literal('Diagnostic release; physical validation pending; immutable SHA256 ' + SHA),
              literal('/uploadfile/' + NAME), '0', state['owner'], 'NOW()', state['owner'], 'NOW()']
    # Only insert our new release. Existing firmware, pilot policy and devices stay untouched.
    sql = 'START TRANSACTION; SET @allowed=0; SET @latest=NULL;\n'
    sql += ('SELECT (BINARY param_value=BINARY ' + literal(state['before']['pilot_config']) +
            ") INTO @allowed FROM sys_params WHERE param_code='server.ota_pilot' FOR UPDATE;\n")
    sql += ("SELECT id INTO @latest FROM ai_ota WHERE type='jiuchuan-s3' "
            'ORDER BY update_date DESC LIMIT 1 FOR UPDATE;\n')
    sql += ('INSERT INTO ai_ota(id,firmware_name,type,version,size,remark,firmware_path,sort,'
            'creator,create_date,updater,update_date) SELECT ' + ','.join(values) +
            ' WHERE @allowed=1 AND @latest=' + literal(state['previous_latest_id']) +
            " AND NOT EXISTS (SELECT 1 FROM ai_ota WHERE type='jiuchuan-s3' AND version=" +
            literal(VERSION) + '); SET @inserted=ROW_COUNT(); COMMIT; SELECT @inserted;')
    state.update(status='publishing', publish_started_at=stage.now())
    save(state)
    try:
        require(database(sql) == '1', 'Concurrent publication/policy change; insertion not applied')
        state.update(status='published-awaiting-live-checks', published_at=stage.now())
        save(state)
        print('PUBLISHED', VERSION, TYPE, state['ota_id'], flush=True)
        verify_checks(state)
    except Exception:
        if metadata(state['ota_id']) == expected_metadata(TYPE):
            rollback()
        raise


if __name__ == '__main__':
    os.umask(0o077)
    require(Path(__file__).resolve().parent == DEPLOY, 'Run only in the dedicated deployment folder')
    configure_image_check()
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['inspect', 'prepare', 'publish', 'verify', 'rollback'])
    action = parser.parse_args().action
    with (DEPLOY / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if action == 'inspect':
            print(json.dumps({'production': stage.snapshot(), 'release': load()}, indent=2))
        else:
            {'prepare': prepare, 'publish': publish, 'verify': verify, 'rollback': rollback}[action]()
