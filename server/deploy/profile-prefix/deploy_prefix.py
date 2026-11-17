"""Guarded API-only prefix-query release; no migration or configuration changes."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, '/opt/xiaozhi-server/deploy-20261009-profile-api-key')
from host_admin import ROOT, TARGET, CONTAINER, digest, jar_diff, wait_healthy


def main(args):
    folder = Path(__file__).resolve().parent
    if folder.parent != ROOT or not folder.name.startswith('deploy-'):
        raise RuntimeError('Unexpected deployment folder')
    staged = folder / TARGET.name
    if digest(TARGET) != args.expected_old or digest(staged) != args.expected_new:
        raise RuntimeError('JAR checksum guard failed')
    changes = jar_diff(staged)
    allowed = {
        'BOOT-INF/classes/xiaozhi/modules/binding/controller/UserProfileController.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dao/UserBindingDao.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/service/UserBindingService.class',
        'BOOT-INF/classes/xiaozhi/modules/security/oauth2/ProfileApiKeyFilter.class',
    }
    print(json.dumps({'changed_entries': changes}))
    if set(changes) != allowed:
        raise RuntimeError('JAR changes differ from the reviewed four-class scope')
    if args.check_only:
        return
    backup = folder / 'backup'
    backup.mkdir(mode=0o700)
    shutil.copy2(TARGET, backup / TARGET.name)
    state = {'old_sha256': args.expected_old, 'new_sha256': args.expected_new,
             'changes': changes, 'backup': str(backup), 'status': 'backed_up'}
    state_path = folder / 'state.json'
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    if digest(TARGET) != args.expected_old:
        raise RuntimeError('Active JAR changed during backup')
    subprocess.run(['docker', 'stop', CONTAINER], check=True)
    try:
        # The container uses a single-file bind mount: preserve the target inode.
        shutil.copy2(staged, TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True)
        wait_healthy()
        state['status'] = 'deployed_healthy'
    except Exception:
        subprocess.run(['docker', 'stop', CONTAINER], check=True)
        shutil.copy2(backup / TARGET.name, TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True)
        wait_healthy()
        state['status'] = 'rolled_back'
        raise
    finally:
        state_path.write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps({'status': state['status'], 'jar_sha256': digest(TARGET)}))


if __name__ == '__main__':
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument('--expected-old', required=True)
    parser.add_argument('--expected-new', required=True)
    parser.add_argument('--check-only', action='store_true')
    main(parser.parse_args())
