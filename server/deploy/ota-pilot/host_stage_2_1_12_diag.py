"""Reuse verified staging/download checks; no activation or fleet-selection action."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import host_stage_2_1_11_diag as stage

if __name__ == '__main__':
    os.umask(0o077)
    stage.DEPLOY = Path(__file__).resolve().parent
    assert stage.DEPLOY == stage.ROOT / 'deploy-20261021-ota-2.1.12-diag'
    manifest = json.loads((stage.DEPLOY / 'verification.json').read_text(encoding='utf-8-sig'))
    assert manifest['version'] == '2.1.12-diag'
    assert manifest['partitionSha256'] == '4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5'
    assert all(manifest[k] for k in ('imageChecksumValidated', 'imageSha256Validated', 'embeddedElfHashMatched'))
    assert stage.digest(stage.ROOT / 'manager-api/xiaozhi-esp32-api.jar') == '605a01f30b9e3805310d9a72966bb67ce0f4d33d23a797ea4150b11464915129'
    stage.VERSION = manifest['version']
    stage.SHA = manifest['appSha256']
    stage.SIZE = manifest['appSize']
    stage.ELF_SHA = manifest['elfSha256']
    stage.NAME = 'vanex-s3-' + stage.VERSION + '-' + stage.SHA[:12] + '.bin'
    stage.IMAGE = stage.ROOT / 'uploadfile' / stage.NAME
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['stage', 'verify', 'download-url'])
    action = parser.parse_args().action
    with (stage.DEPLOY / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        {'stage': stage.stage, 'verify': stage.verify, 'download-url': stage.download_url}[action]()
