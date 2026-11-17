"""Check the live OTA contract with temporary fixtures, without publishing or pushing."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

RELEASE = Path('/opt/xiaozhi-server/deploy-20261022-ota-2.1.13-diag-fleet')
sys.path.insert(0, str(RELEASE))
import host_fleet_2_1_13_diag as release

release.configure()
fleet, stage = release.fleet, release.stage
db, lit, redis, require = release.database, release.literal, release.redis, release.require
registered = '02:fa:22:13:14:01'
unknown = '02:fa:22:13:14:02'
fixture_id = uuid.uuid4().hex
state = fleet.load()
tickets, activation_codes = set(), set()
results = {'version': state['version'], 'physical_device_tested': False, 'forced_upgrade': False,
           'checked_at': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'endpoints': {}}


def request(base, method='POST', mac=registered, version='2.1.12-diag', board='jiuchuan-s3', path='/xiaozhi/ota/'):
    headers = {'Content-Type': 'application/json'}
    if mac is not None:
        headers['Device-Id'] = mac
    # Deliberately omit API Key, Bearer token and optional Client-Id.
    data = json.dumps({'application': {'version': version}, 'board': {'type': board}}).encode()
    req = urllib.request.Request(base + path, data=data if method == 'POST' else None,
                                 headers=headers, method=method)
    try:
        response = urllib.request.urlopen(req, timeout=45)
    except urllib.error.HTTPError as exc:
        response = exc
    with response:
        status, response_headers, raw = response.status, dict(response.headers), response.read()
    try:
        payload = json.loads(raw)
    except (ValueError, UnicodeDecodeError):
        payload = raw.decode('utf-8', errors='replace')
    if isinstance(payload, dict):
        url = payload.get('firmware', {}).get('url', '')
        if url.startswith('https://robot.suji.work/xiaozhi/otaMag/download/'):
            tickets.add(url.rsplit('/', 1)[-1])
        code = payload.get('activation', {}).get('code')
        if mac == unknown and code:
            activation_codes.add(str(code))
    return status, response_headers, payload


def main():
    before = stage.snapshot()
    fleet.assert_published(state)
    for mac in (registered, unknown):
        require(db('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + lit(mac)) == '0', 'Fixture collision')
        require(redis('EXISTS', 'ota:activation:data:' + mac.replace(':', '_')) == '0', 'Activation fixture collision')
    created = False
    try:
        db('INSERT INTO ai_device(id,mac_address,board,app_version,auto_update,alias) VALUES (' +
           ','.join([lit(fixture_id), lit(registered), lit('jiuchuan-s3'), lit('2.1.12-diag'), '1',
                     lit('temporary OTA documentation contract test')]) + ')')
        created = True
        for base in stage.BASES:
            checks = []
            status, headers, payload = request(base)
            require(status == 200 and 'code' not in payload and 'data' not in payload,
                    'Unexpected OTA response envelope')
            require(payload.get('server_time') and payload.get('websocket', {}).get('url'), 'Config missing')
            require(payload['firmware']['version'] == state['version'] and
                    payload['firmware']['url'].startswith('https://robot.suji.work/xiaozhi/otaMag/download/'),
                    'No upgrade selected for registered older fixture')
            ticket = payload['firmware']['url'].rsplit('/', 1)[-1]
            require(json.loads(redis('GET', 'ota:id:' + ticket)) == state['ota_id'], 'Wrong image selected')
            ttl = int(redis('TTL', 'ota:id:' + ticket))
            require(86000 < ttl <= 86400, 'Unexpected ticket TTL')
            checks.append('POST minimal body without authentication or Client-Id selects current app; ticket TTL 24h')
            example = {'server_time': payload['server_time'],
                       'firmware': {'version': payload['firmware']['version'],
                                    'url': 'https://robot.suji.work/xiaozhi/otaMag/download/<temporary-ticket>'},
                       'websocket': {'url': '<redacted>', 'token': '<redacted>'}}
            for version in ('2.1.0', '2.1.10', '2.1.11-diag'):
                status, _, body = request(base, version=version)
                require(status == 200 and body['firmware']['version'] == state['version'] and
                        body['firmware']['url'] != fleet.NO_UPDATE_URL, 'Older version check failed')
                checks.append('upgrade from ' + version)
            for version in ('2.1.13-diag', '2.1.13', '2.1.14'):
                status, _, body = request(base, version=version)
                require(status == 200 and body['firmware']['url'] == fleet.NO_UPDATE_URL, 'Unexpected upgrade')
                checks.append('no update/downgrade for ' + version)
            db('UPDATE ai_device SET auto_update=0 WHERE id=' + lit(fixture_id))
            status, _, body = request(base)
            require(status == 200 and 'firmware' not in body, 'Opt-out ignored')
            checks.append('auto_update=0 omits firmware')
            db('UPDATE ai_device SET auto_update=1 WHERE id=' + lit(fixture_id))
            status, _, body = request(base, board='unrelated-board')
            require(status == 200 and body['firmware']['url'] == fleet.NO_UPDATE_URL, 'Board mismatch accepted')
            checks.append('unrelated board gets no-update placeholder')
            status, _, body = request(base, mac=unknown)
            require(status == 200 and body.get('activation', {}).get('code') and
                    body['firmware']['url'] == fleet.NO_UPDATE_URL, 'Unknown device contract changed')
            checks.append('unregistered MAC gets activation and no-update placeholder')
            status, _, body = request(base, mac='864479086531093s3')
            require(status == 200 and body.get('error') == 'Invalid device ID', 'cabGuid error contract changed')
            checks.append('cabGuid instead of MAC returns error even with HTTP 200')
            status, _, body = request(base, mac=None)
            require(status == 200 and isinstance(body, dict) and body.get('code') == 500,
                    'Missing Device-Id error contract changed')
            checks.append('known caveat: missing Device-Id returns HTTP 200 with code=500, not a successful check')
            status, _, body = request(base, method='GET')
            require(status == 200 and isinstance(body, str) and '运行正常' in body, 'Health endpoint changed')
            checks.append('GET is text health check, not an upgrade decision')
            results['endpoints'][base] = {'checks': checks, 'sanitized_example': example,
                                         'download_ticket_initial_ttl_seconds': ttl}
            print('CONTRACT_OK', base, len(checks), 'checks', flush=True)
            env = dict(os.environ, OTA_SMOKE_BASE=base, OTA_SMOKE_IMAGE=str(release.IMAGE),
                       OTA_SMOKE_ID=state['ota_id'])
            tested = subprocess.run([sys.executable, str(RELEASE / 'smoke_download.py')], env=env,
                                    capture_output=True, text=True, timeout=600)
            require(tested.returncode == 0, 'Download/Range contract failed')
            results['endpoints'][base]['download_checks'] = tested.stdout.strip().splitlines()
            print('DOWNLOAD_OK', base, '10 checks', flush=True)
    finally:
        if created:
            time.sleep(1)
            db('DELETE FROM ai_device WHERE id=' + lit(fixture_id) + ' AND mac_address=' + lit(registered))
        for ticket in tickets:
            redis('DEL', 'ota:id:' + ticket, 'ota:download:count:' + ticket)
        for code in activation_codes:
            key = 'ota:activation:code:' + code
            value = redis('GET', key)
            if value and json.loads(value) == unknown:
                redis('DEL', key)
        redis('DEL', 'ota:activation:data:' + unknown.replace(':', '_'))
        for mac in (registered, unknown):
            require(db('SELECT COUNT(*) FROM ai_device WHERE mac_address=' + lit(mac)) == '0', 'Cleanup incomplete')
    require(stage.snapshot() == before, 'Publication changed during audit')
    results['cleanup_verified'] = True
    results['production_release_unchanged'] = True
    results['app_sha256'] = state['sha256']
    results['app_size'] = state['size']
    Path(__file__).with_name('ota-contract-result.json').write_text(json.dumps(results, indent=2) + '\n')
    print('AUDIT_OK; no publication, customer-device change or remote upgrade performed', flush=True)


if __name__ == '__main__':
    os.umask(0o077)
    main()
