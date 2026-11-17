"""Read-only authentication audit; never print secrets, profiles or role definitions."""
import hashlib
import json
import os
from pathlib import Path
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.environ.get(
    'AUDIT_PILOT_DIR', str(Path(__file__).resolve().parents[1] / 'ota-pilot')
))
from host_pilot import database, literal

OLD_ID = os.environ.get('AUDIT_OLD_KEY_ID', '')
AGENT = os.environ.get('AUDIT_AGENT_ID', '')
USER_GUID = os.environ.get('AUDIT_USER_GUID', '')
CREDENTIAL = Path(os.environ.get(
    'AUDIT_CREDENTIAL_FILE', '/run/secrets/customer-api-key.json'
))


def metadata(key_id):
    return database('SELECT k.key_id,u.username,u.status,k.scopes,'
                    '(k.revoked_at IS NOT NULL),'
                    '(k.expires_at IS NOT NULL AND k.expires_at<=NOW(3)) '
                    'FROM ai_profile_api_key k JOIN sys_user u ON u.id=k.owner_id '
                    'WHERE k.key_id=' + literal(key_id))


def get(url, key):
    request = urllib.request.Request(url, headers={'X-API-Key': key})
    try:
        response = urllib.request.urlopen(request, timeout=30)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        payload = json.load(response)
        return {'httpStatus': response.status, 'code': payload.get('code')}


def main():
    os.umask(0o077)
    if not all((OLD_ID, AGENT, USER_GUID)):
        raise RuntimeError('Set AUDIT_OLD_KEY_ID, AUDIT_AGENT_ID and AUDIT_USER_GUID')
    result = {'originalKey': metadata(OLD_ID), 'replacementDeliveryAvailable': CREDENTIAL.exists()}
    if CREDENTIAL.exists():
        credential = json.loads(CREDENTIAL.read_text())
        key = credential['apiKey']
        key_id = key[4:36]
        result['integrationKey'] = metadata(key_id)
        stored = database('SELECT secret_hash FROM ai_profile_api_key WHERE key_id=' + literal(key_id))
        if hashlib.sha256(key.encode('ascii')).hexdigest() != stored:
            raise RuntimeError('Credential file does not match stored verifier')
        owner = database('SELECT owner_id FROM ai_profile_api_key WHERE key_id=' + literal(key_id))
        if owner != database('SELECT user_id FROM ai_agent WHERE id=' + literal(AGENT)):
            raise RuntimeError('Credential and requested agent have different owners')
        result['roles'] = get('https://robot.suji.work/xiaozhi/integration/agents/' + AGENT + '/roles', key)
        result['profile'] = get('https://robot.suji.work/xiaozhi/user/profile/' + USER_GUID, key)
        result['ownerMatched'] = True
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
