"""Install only the profile proxy route, preserving all existing Nginx routes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

FOLDER = Path(__file__).resolve().parent
API = Path('/www/server/panel/vhost/nginx/xiaozhi-api-proxy.conf')
PUBLIC = Path('/www/server/panel/vhost/nginx/robot.suji.work.conf')
EXTENSION = Path('/www/server/panel/vhost/nginx/extension/robot.suji.work/profile-api-key.conf')
NGINX = '/www/server/nginx/sbin/nginx'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run():
    os.umask(0o077)
    source = FOLDER / 'nginx-profile-location.conf'
    original = API.read_bytes()
    public = PUBLIC.read_bytes()
    assert b'include /www/server/panel/vhost/nginx/extension/robot.suji.work/*.conf;' in public
    anchor = b'    include /opt/xiaozhi-server/deploy-20261004-ota-resume/nginx-download-location.conf;'
    assert original.count(anchor) == 1
    assert not EXTENSION.exists()
    backup = FOLDER / 'proxy-backup'
    backup.mkdir(mode=0o700)
    shutil.copy2(API, backup / API.name)
    shutil.copy2(PUBLIC, backup / PUBLIC.name)
    updated = original.replace(anchor, anchor + b'\n    include ' + str(source).encode() + b';')
    state = {'before_sha256': sha(original), 'after_sha256': sha(updated),
        'api_config': str(API), 'extension': str(EXTENSION), 'status': 'prepared'}
    assert API.read_bytes() == original and PUBLIC.read_bytes() == public
    try:
        API.write_bytes(updated)
        shutil.copy2(source, EXTENSION)
        result = subprocess.run([NGINX, '-t'], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError('Nginx config check failed')
        subprocess.run([NGINX, '-s', 'reload'], check=True, capture_output=True)
        state['status'] = 'reloaded'
    except Exception:
        API.write_bytes(original)
        EXTENSION.unlink(missing_ok=True)
        subprocess.run([NGINX, '-t'], check=True, capture_output=True)
        subprocess.run([NGINX, '-s', 'reload'], check=True, capture_output=True)
        state['status'] = 'rolled_back'
        raise
    finally:
        (FOLDER / 'proxy-state.json').write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps(state))


if __name__ == '__main__':
    run()
