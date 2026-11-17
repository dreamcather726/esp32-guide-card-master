"""Release the customer-approved 2.1.13 diagnostic app using proven OTA checks."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import sys
import time
import uuid

ROOT = Path('/opt/xiaozhi-server')
DEPLOY = ROOT / 'deploy-20261022-ota-2.1.13-diag-fleet'
sys.path.insert(0, str(ROOT / 'deploy-20261021-ota-2.1.12-diag-fleet'))
import host_fleet_2_1_12_diag as fleet
from host_pilot import command

stage = fleet.stage
require, database, literal, redis = fleet.require, fleet.database, fleet.literal, fleet.redis
VERSION = '2.1.13-diag'
SHA = '4273f51f4c92ca0cb50d22d3a6705417fea9321c0bef8665d0367ef2215bfdee'
ELF_SHA = 'c5b3d691a59e851341c7ca20330acbd5f981bb03004a3a7f422c82540f57da06'
SIZE = 3236640
NAME = 'vanex-s3-2.1.13-diag-4273f51f4c92.bin'
IMAGE = ROOT / 'uploadfile' / NAME
FIXTURES = ('02:fa:22:13:13:01', '02:fa:22:13:13:02')


def configure():
    manifest = json.loads((DEPLOY / 'verification.json').read_text(encoding='utf-8-sig'))
    require((manifest['version'], manifest['appSize'], manifest['appSha256'], manifest['elfSha256']) ==
            (VERSION, SIZE, SHA, ELF_SHA), 'Manifest identity mismatch')
    require(manifest['partitionSha256'] ==
            '4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5', 'Partition mismatch')
    require(all(manifest[k] for k in ('imageChecksumValidated', 'imageSha256Validated',
                                    'embeddedElfHashMatched', 'assetsMatchedPreviousBuild')),
            'Image verification incomplete')
    stage.DEPLOY = DEPLOY / 'staging'
    stage.VERSION, stage.SHA, stage.SIZE, stage.ELF_SHA = VERSION, SHA, SIZE, ELF_SHA
    stage.NAME, stage.IMAGE = NAME, IMAGE
    fleet.DEPLOY, fleet.STAGING = DEPLOY, stage.DEPLOY
    fleet.VERSION, fleet.SHA, fleet.SIZE, fleet.NAME, fleet.IMAGE = VERSION, SHA, SIZE, NAME, IMAGE
    fleet.FIXTURES = FIXTURES
    fleet.selection_smoke = selection_smoke


def stage_release():
    require(not (DEPLOY / 'state.json').exists(), 'Publication state already exists; inspect first')
    require(not (stage.DEPLOY / 'state.json').exists(), 'Staging already exists; inspect first')
    stage.verify_image(DEPLOY / 'xiaozhi.bin')
    stage.health()
    before = stage.snapshot()
    require(before['api_sha256'] == fleet.API_SHA, 'Unexpected management API build')
    require(json.loads(before['pilot_config']) == {'enabled': False}, 'Unexpected pilot policy')
    old_id = fleet.latest_id()
    row = database('SELECT version,firmware_path FROM ai_ota WHERE id=' + literal(old_id)).split('\t')
    require(len(row) == 2 and row[0] == '2.1.12-diag', 'Unexpected previous OTA version')
    old_image = (ROOT / row[1].lstrip('/')).resolve()
    require(old_image.parent == (ROOT / 'uploadfile').resolve(), 'Previous image path mismatch')
    require(stage.digest(old_image) ==
            'd403b04908f589803aeba9d1b9d72efa2491cd13f72b228183de68e7509db0ae',
            'Previous image hash mismatch')
    backup = DEPLOY / 'backup'
    backup.mkdir(mode=0o700)
    shutil.copy2(old_image, backup / old_image.name)
    shutil.copy2(ROOT / 'deploy-20261021-ota-2.1.12-diag-fleet/state.json', backup / 'previous-release.json')
    (backup / 'configuration.json').write_text(json.dumps(before, indent=2) + '\n')
    dump = command(['docker', 'exec', 'xiaozhi-esp32-server-db', 'sh', '-lc',
                    'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysqldump -uroot --single-transaction '
                    '--no-tablespaces --skip-lock-tables xiaozhi_esp32_server ai_ota'])
    require('CREATE TABLE' in dump and 'INSERT INTO' in dump, 'OTA metadata backup incomplete')
    (backup / 'ai_ota.sql').write_text(dump + '\n')
    stage.DEPLOY.mkdir(mode=0o700)
    shutil.copy2(DEPLOY / 'xiaozhi.bin', stage.DEPLOY / 'xiaozhi.bin')
    shutil.copy2(DEPLOY / 'smoke_download.py', stage.DEPLOY / 'smoke_download.py')
    require(stage.snapshot() == before, 'Production changed during backup')
    print('BACKUP_OK; previous app and OTA metadata preserved', flush=True)
    stage.stage()
    stage.verify()


def prepare():
    require(not (DEPLOY / 'state.json').exists(), 'Publication state exists; inspect first')
    stage.health()
    stage.verify_image(IMAGE)
    staged = stage.load()
    before = stage.snapshot()
    require(staged.get('verified') is True and staged['version'] == VERSION and
            staged['sha256'] == SHA and staged['size'] == SIZE, 'Staging verification mismatch')
    require(before == staged['before'], 'Production changed since staging')
    require(before['api_sha256'] == fleet.API_SHA, 'Unexpected API build')
    require(json.loads(before['pilot_config']) == {'enabled': False}, 'Unexpected pilot policy')
    require(fleet.metadata(staged['ota_id']) == fleet.expected_metadata('pilot-jiuchuan-s3'),
            'Staged metadata mismatch')
    require(database("SELECT COUNT(*) FROM ai_ota WHERE type='jiuchuan-s3' AND version=" +
                     literal(VERSION)) == '0', 'Release already exists')
    previous_id = fleet.latest_id()
    require(database('SELECT version FROM ai_ota WHERE id=' + literal(previous_id)) == '2.1.12-diag',
            'Previous release changed')
    owner = database('SELECT creator FROM ai_ota WHERE id=' + literal(staged['ota_id']))
    require(owner.isdigit(), 'Release owner is ambiguous')
    state = {'ota_id': uuid.uuid4().hex, 'version': VERSION, 'sha256': SHA, 'size': SIZE,
             'elf_sha256': ELF_SHA, 'staged_id': staged['ota_id'], 'owner': owner, 'before': before,
             'previous_latest_id': previous_id, 'status': 'preparing', 'prepared_at': stage.now(),
             'board_type': fleet.TYPE, 'physical_device_tested': False, 'forced_reboots': False,
             'device_auto_update_settings_modified': False, 'backup_directory': str(DEPLOY / 'backup')}
    fleet.save(state)
    (DEPLOY / 'device-settings-before.tsv').write_text(database(
        'SELECT id,board,auto_update FROM ai_device ORDER BY id') + '\n')
    fleet.download_smoke(staged['ota_id'], 'preflight')
    require(stage.snapshot() == before, 'Production changed during preflight')
    state['status'] = 'ready-to-publish'
    fleet.save(state)
    print('PREPARED; no public release or device settings changed', flush=True)


def selection_smoke(state):
    ids = [uuid.uuid4().hex for _ in FIXTURES]
    tickets, counts = set(), {}
    created = False
    try:
        for mac in FIXTURES:
            require(database('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + literal(mac)) == '0',
                    'Test MAC collision; customer rows must stay untouched')
        sql = 'START TRANSACTION;\n'
        for item, mac in zip(ids, FIXTURES):
            values = [literal(item), literal(mac), literal(fleet.TYPE), literal('2.1.0'), '1',
                      literal('temporary 2.1.13 diagnostic OTA verification')]
            sql += ('INSERT INTO ai_device(id,mac_address,board,app_version,auto_update,alias) VALUES (' +
                    ','.join(values) + ');\n')
        database(sql + 'COMMIT;')
        created = True
        for base in stage.BASES:
            count = 0

            def check(mac, version, board=fleet.TYPE):
                nonlocal count
                result = fleet.post_check(mac, version, base, board)
                url = result.get('firmware', {}).get('url', '')
                if url.startswith('https://robot.suji.work/xiaozhi/otaMag/download/'):
                    tickets.add(url.rsplit('/', 1)[-1])
                require(result.get('server_time') and result.get('websocket', {}).get('url'),
                        'Time/configuration missing from OTA response')
                count += 1
                return result

            for mac in FIXTURES:
                for old in ('2.1.0', '2.1.10', '2.1.11-diag', '2.1.12-diag'):
                    firmware = check(mac, old)['firmware']
                    require(firmware['version'] == VERSION, 'Older device did not receive new version')
                    require(firmware['url'].startswith('https://robot.suji.work/xiaozhi/otaMag/download/'),
                            'Unexpected download origin')
                    ticket = firmware['url'].rsplit('/', 1)[-1]
                    require(json.loads(redis('GET', 'ota:id:' + ticket)) == state['ota_id'],
                            'Download ticket references a different release')
            for installed in (VERSION, '2.1.13', '2.1.14'):
                require(check(FIXTURES[0], installed)['firmware']['url'] == fleet.NO_UPDATE_URL,
                        'Same/newer version offered a duplicate upgrade or downgrade')
            database('UPDATE ai_device SET auto_update=0 WHERE id=' + literal(ids[0]))
            require('firmware' not in check(FIXTURES[0], '2.1.0'), 'Device opt-out ignored')
            database('UPDATE ai_device SET auto_update=1 WHERE id=' + literal(ids[0]))
            require(check(FIXTURES[1], '2.1.0', 'unrelated-board')['firmware']['url'] == fleet.NO_UPDATE_URL,
                    'Other board offered Vanex firmware')
            require('firmware' not in check(FIXTURES[1], '2.1.0', 'pilot-jiuchuan-s3'),
                    'Staging board exposed via public lookup')
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
                            'Temporary device cleanup incomplete')
        finally:
            for ticket in tickets:
                redis('DEL', 'ota:id:' + ticket, 'ota:download:count:' + ticket)
    return counts


if __name__ == '__main__':
    os.umask(0o077)
    require(Path(__file__).resolve().parent == DEPLOY, 'Use the dedicated deployment directory')
    configure()
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['stage', 'prepare', 'publish', 'verify', 'inspect', 'rollback'])
    action = parser.parse_args().action
    with (DEPLOY / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if action == 'inspect':
            print(json.dumps({'production': stage.snapshot(), 'release': fleet.load()}, indent=2))
        else:
            {'stage': stage_release, 'prepare': prepare, 'publish': fleet.publish,
             'verify': fleet.verify, 'rollback': fleet.rollback}[action]()
