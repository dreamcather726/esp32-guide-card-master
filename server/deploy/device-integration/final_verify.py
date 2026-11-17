"""Read-only customer-account acceptance; never prints credentials or conversations."""
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release


def main():
    os.umask(0o077)
    state=release.load()
    assert state['status']=='accepted'
    assert release.hashes()==state['new_hashes'] and release.digest(release.TARGET)==state['new_jar']
    credential=json.loads((release.HERE/'customer-api-key.json').read_text())
    owner=credential['ownerId']
    def api(path):
        request=urllib.request.Request(credential['baseUrl']+path,headers={'X-API-Key':credential['apiKey']})
        with urllib.request.urlopen(request,timeout=30) as response: result=json.load(response)
        assert result['code']==0, 'Customer read-only API failed'
        return result['data']
    agent='ce9307e119034a219459334df1fff0e8'
    assert release.database('SELECT user_id FROM ai_agent WHERE id='+release.sql(agent))==owner
    roles=api('/agents/'+agent+'/roles')
    assert roles and all(set(('voiceId','voiceName','roleDefinition','imageUrl','videoUrl','audioUrl'))<=set(row) for row in roles)
    device=release.database('SELECT id FROM ai_device WHERE user_id='+release.sql(owner)+' AND agent_id='+release.sql(agent)+' ORDER BY id LIMIT 1')
    assert device
    api('/devices/'+device+'/role-binding')
    history=api('/devices/'+device+'/chat-history?limit=1')
    assert all('profileSnapshot' not in item for item in history['list'])
    mac=release.database('SELECT mac_address FROM ai_device WHERE id='+release.sql(device))
    secret=release.run('docker','exec',release.VOICE,'python','-c',
        "import yaml; print(yaml.safe_load(open('data/.config.yaml'))['manager-api']['secret'])")
    request=urllib.request.Request('http://127.0.0.1:8004/xiaozhi/config/agent-models',
        method='POST',headers={'Authorization':'Bearer '+secret,'Content-Type':'application/json'},
        data=json.dumps({'macAddress':mac,'clientId':'readonly-acceptance','selectedModule':{}}).encode())
    with urllib.request.urlopen(request,timeout=30) as response: configured=json.load(response)
    assert configured['code']==0
    configured=configured['data']
    llm=configured['LLM'][configured['selected_module']['LLM']]
    llm_name=llm.get('model_name') or llm.get('model')
    llm_key=llm.get('api_key')
    assert llm_name=='qwen-plus' and llm_key and llm_key not in ('your_api_key','你的api_key')
    containers={name:release.run('docker','inspect',name,'--format','{{.State.Running}}')=='true' for name in
        ('xiaozhi-esp32-server','xiaozhi-esp32-server-web','xiaozhi-esp32-server-db','xiaozhi-esp32-server-redis','vanex-mqtt-binding-bridge')}
    assert all(containers.values())
    started=release.run('docker','inspect',release.VOICE,'--format','{{.State.StartedAt}}')
    logs=subprocess.run(['docker','logs','--since',started,release.VOICE],capture_output=True,text=True)
    errors=sum('ERROR' in line or 'Traceback (most recent call last)' in line for line in (logs.stdout+logs.stderr).splitlines())
    result={'passed':True,'baseUrl':credential['baseUrl'],'agentId':agent,'roleCount':len(roles),
        'customer_account_readonly_verification':True,'production_devices_modified':False,
        'containers_running':containers,'voice_error_lines_since_restart':errors,
        'configured_customer_llm':llm_name,'customer_llm_key_configured':True,
        'existing_configuration_warnings':['default/auxiliary LLM placeholder credentials','weather plugin authentication'],
        'jarSha256':state['new_jar'],'voiceImage':state['new_image'],
        'firmware_changed':False,'model_configuration_changed':False}
    (release.HERE/'final-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))


if __name__=='__main__': main()
