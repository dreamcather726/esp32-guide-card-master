"""Host-side, guarded voice-only rollout with image/config rollback."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time

import yaml
from model_admin import main as model_action

ROOT = Path('/opt/xiaozhi-server')
HERE = Path(__file__).resolve().parent
CONTAINER = 'xiaozhi-esp32-server'
SERVICE_ROOT = '/opt/xiaozhi-esp32-server'
BASE_TAG = 'xiaozhi-esp32-server:rollback-20261015-voice'
NEW_TAG = 'xiaozhi-esp32-server:custom-20261015-voice'
OVERRIDE = ROOT / 'docker-compose.user-binding.yml'
STATE = HERE / 'release-state.json'
FILES = [
    'core/connection.py', 'core/providers/asr/aliyunbl_stream.py', 'core/providers/asr/base.py',
    'core/handle/textHandler/listenMessageHandler.py', 'core/handle/sendAudioHandle.py',
    'core/utils/audioRateController.py', 'core/utils/asr_queue.py',
    'core/providers/llm/openai/openai.py',
]


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def live_info():
    data = json.loads(run('docker', 'inspect', CONTAINER))[0]
    return {'image': data['Image'], 'tag': data['Config']['Image']}


def live_hashes():
    program = 'import hashlib,json,pathlib; files=' + repr(FILES) + '; print(json.dumps({' + \
        'p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() if pathlib.Path(p).exists() else None for p in files}))'
    return json.loads(run('docker', 'exec', CONTAINER, 'python', '-c', program))


def save(state):
    STATE.write_text(json.dumps(state, indent=2) + '\n')


def recreate():
    subprocess.run(['docker', 'compose', '-f', str(ROOT / 'docker-compose_all.yml'), '-f', str(OVERRIDE),
        'up', '-d', '--no-deps', '--force-recreate', CONTAINER], check=True, cwd=ROOT)


def wait_ready():
    for _ in range(90):
        try:
            running = run('docker', 'inspect', CONTAINER, '--format', '{{.State.Running}}')
            if running != 'true':
                raise RuntimeError('voice_container_not_running')
            with socket.create_connection(('127.0.0.1', 8000), timeout=1):
                pass
            with socket.create_connection(('127.0.0.1', 8003), timeout=1):
                pass
            return
        except (OSError, RuntimeError):
            time.sleep(1)
    raise RuntimeError('voice_start_deadline')


def prepare():
    assert not STATE.exists(), 'release_already_prepared'
    manifest = json.loads((HERE / 'manifest.json').read_text())
    assert set(manifest['files']) == set(FILES)
    for path in FILES:
        assert digest(HERE / path) == manifest['files'][path], 'staged_file_changed'
    old = live_info()
    assert old['tag'] == 'xiaozhi-esp32-server:custom-20261002-user-binding', 'unexpected_base'
    backup = HERE / 'backup'
    backup.mkdir(mode=0o700, exist_ok=True)
    shutil.copy2(OVERRIDE, backup / OVERRIDE.name)
    hashes = live_hashes()
    run('docker', 'tag', old['image'], BASE_TAG)
    state = {'status': 'prepared', 'previous': old, 'old_hashes': hashes,
             'old_compose_sha256': digest(OVERRIDE), 'new_hashes': manifest['files']}
    save(state)
    subprocess.run(['docker', 'build', '--network=host', '--build-arg', 'BASE_IMAGE=' + BASE_TAG,
                    '-t', NEW_TAG, str(HERE)], check=True)
    state['new_image'] = run('docker', 'image', 'inspect', NEW_TAG, '--format', '{{.Id}}')
    state['status'] = 'built_tested'
    save(state)
    print(json.dumps({'status': state['status'], 'image': state['new_image']}))


def rollback(state):
    shutil.copy2(HERE / 'backup' / OVERRIDE.name, OVERRIDE)
    if (HERE / 'backup' / 'model.json').exists():
        model_action(argparse.Namespace(action='rollback', model='qwen-plus'))
    recreate()
    wait_ready()
    assert live_info()['image'] == state['previous']['image']
    state['status'] = 'rolled_back'
    save(state)
    print(json.dumps({'status': state['status']}))


def deploy():
    state = json.loads(STATE.read_text())
    assert state['status'] == 'built_tested'
    assert live_info()['image'] == state['previous']['image']
    assert live_hashes() == state['old_hashes'], 'active_source_changed'
    assert digest(OVERRIDE) == state['old_compose_sha256'], 'compose_changed'
    state['status'] = 'deploying'
    save(state)
    try:
        model_action(argparse.Namespace(action='apply', model='qwen-plus'))
        config = yaml.safe_load(OVERRIDE.read_text())
        config['services'][CONTAINER]['image'] = NEW_TAG
        OVERRIDE.write_text(yaml.safe_dump(config, sort_keys=False))
        recreate()
        wait_ready()
        assert live_info()['image'] == state['new_image']
        assert live_hashes() == state['new_hashes'], 'live_source_verification_failed'
        state['status'] = 'deployed_pending_smoke'
        save(state)
        print(json.dumps({'status': state['status'], 'image': state['new_image']}))
    except Exception:
        rollback(state)
        raise


if __name__ == '__main__':
    os.umask(0o077)
    assert HERE == ROOT / 'deploy-20261015-voice'
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['prepare', 'deploy', 'rollback', 'accept'])
    args = parser.parse_args()
    if args.action == 'prepare':
        prepare()
    elif args.action == 'deploy':
        deploy()
    elif args.action == 'rollback':
        rollback(json.loads(STATE.read_text()))
    else:
        state = json.loads(STATE.read_text())
        assert state['status'] == 'deployed_pending_smoke'
        assert (HERE / 'smoke-result.json').exists()
        assert json.loads((HERE / 'smoke-result.json').read_text())['passed'] is True
        assert live_info()['image'] == state['new_image']
        assert live_hashes() == state['new_hashes']
        state['status'] = 'accepted'
        save(state)
        print(json.dumps({'status': state['status'], 'image': state['new_image']}))
