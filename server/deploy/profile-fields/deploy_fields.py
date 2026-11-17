"""Guarded API-only release. The additive migration does not rewrite existing profiles."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, '/opt/xiaozhi-server/deploy-20261009-profile-api-key')
from host_admin import ROOT, TARGET, CONTAINER, digest, jar_diff, wait_healthy, database


def deploy(args):
    folder = Path(__file__).resolve().parent
    if folder.parent != ROOT or not folder.name.startswith('deploy-'):
        raise RuntimeError('Unexpected deployment folder')
    staged = folder / TARGET.name
    if digest(TARGET) != args.expected_old or digest(staged) != args.expected_new:
        raise RuntimeError('JAR checksum guard failed')
    changes = jar_diff(staged)
    allowed = {
        'BOOT-INF/classes/db/changelog/db.changelog-master.yaml',
        'BOOT-INF/classes/db/changelog/202610101200.sql',
        'BOOT-INF/classes/xiaozhi/modules/binding/dto/UserBindingDTOs.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dto/UserBindingDTOs$Profile.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dto/UserBindingDTOs$ProfileView.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dto/UserBindingDTOs$Create.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dto/UserBindingDTOs$Resolve.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dto/UserBindingDTOs$Command.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dto/UserBindingDTOs$Binding.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/json/ProfileUsageCountDeserializer.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/entity/UserProfileEntity.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/dao/UserBindingDao.class',
        'BOOT-INF/classes/xiaozhi/modules/binding/service/UserBindingService.class',
    }
    unexpected = sorted(set(changes) - allowed)
    if unexpected:
        print(json.dumps({'unexpected_jar_entries': unexpected}))
        raise RuntimeError('Unrelated JAR change detected')
    backup = folder / 'backup'
    backup.mkdir(mode=0o700)
    shutil.copy2(TARGET, backup / TARGET.name)
    with (backup / 'database.sql').open('xb') as out:
        result = subprocess.run(['docker', 'exec', 'xiaozhi-esp32-server-db', 'sh', '-lc',
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysqldump -uroot --single-transaction --no-tablespaces xiaozhi_esp32_server'],
            stdout=out, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError('Database backup failed; no deployment performed')
    state = {'old_sha256': args.expected_old, 'new_sha256': args.expected_new,
        'changes': changes, 'backup': str(backup), 'status': 'backed_up'}
    state_path = folder / 'state.json'
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    if digest(TARGET) != args.expected_old:
        raise RuntimeError('Active JAR changed during backup')
    subprocess.run(['docker', 'stop', CONTAINER], check=True)
    try:
        # Copy into the existing single-file bind mount, rather than replacing its inode.
        shutil.copy2(staged, TARGET)
        subprocess.run(['docker', 'start', CONTAINER], check=True)
        wait_healthy()
        assert database("SELECT COUNT(*) FROM DATABASECHANGELOG WHERE ID='202610101200'") == '1'
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
    deploy(parser.parse_args())
