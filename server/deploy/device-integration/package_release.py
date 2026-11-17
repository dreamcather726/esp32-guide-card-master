"""Overlay reviewed compiled classes onto the exact production JAR, not an unrelated rebuild."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import zipfile

HERE = Path(__file__).resolve().parent
SERVER = HERE.parents[1]
API = SERVER / 'main/manager-api'
VOICE = SERVER / 'main/xiaozhi-server'
CLASSES = API / 'target/classes'
BASE = API / 'target/xiaozhi-esp32-api.jar'
JAVA = [
    'xiaozhi/modules/agent/entity/AgentRoleVoiceBindingEntity.class',
    'xiaozhi/modules/agent/dto/AgentRoleVoiceBindingDTO.class',
    'xiaozhi/modules/agent/dto/AgentChatHistoryReportDTO.class',
    'xiaozhi/modules/agent/service/impl/AgentRoleVoiceBindingServiceImpl.class',
    'xiaozhi/modules/agent/service/biz/impl/AgentChatHistoryBizServiceImpl.class',
    'xiaozhi/modules/binding/entity/ProfileApiKeyEntity.class',
    'xiaozhi/modules/binding/dao/ProfileApiKeyDao.class',
    'xiaozhi/modules/binding/service/ProfileApiKeyService.class',
    'xiaozhi/modules/security/config/ShiroConfig.class',
    'xiaozhi/modules/config/service/impl/ConfigServiceImpl.class',
]
JAVA += [str(p.relative_to(CLASSES)).replace('\\','/') for p in
         (CLASSES / 'xiaozhi/modules/integration').rglob('*.class')]
JAVA += [str(p.relative_to(CLASSES)).replace('\\','/') for p in
         (CLASSES / 'xiaozhi/modules/security/oauth2').glob('IntegrationApiKeyFilter*.class')]
RUNTIME = ['core/connection.py','core/http_server.py','core/api/device_control_handler.py',
           'core/handle/reportHandle.py','core/utils/report_outbox.py','config/manage_api_client.py']
overlays = {'BOOT-INF/classes/' + name: (CLASSES/name).read_bytes() for name in JAVA}
for name in ['db/changelog/db.changelog-master.yaml','db/changelog/202610191200.sql']:
    overlays['BOOT-INF/classes/'+name] = (API/'src/main/resources'/name).read_bytes()
with zipfile.ZipFile(BASE) as old, zipfile.ZipFile(HERE / BASE.name, 'w') as out:
    for entry in old.infolist():
        out.writestr(entry, overlays.pop(entry.filename, old.read(entry.filename)))
    for name, value in overlays.items():
        out.writestr(name, value, compress_type=zipfile.ZIP_DEFLATED)
manifest = {'base_jar': hashlib.sha256(BASE.read_bytes()).hexdigest(),
            'new_jar': hashlib.sha256((HERE/BASE.name).read_bytes()).hexdigest(),
            'jar_changes': sorted('BOOT-INF/classes/'+p for p in JAVA) +
                ['BOOT-INF/classes/db/changelog/db.changelog-master.yaml','BOOT-INF/classes/db/changelog/202610191200.sql'],
            'runtime': {p:hashlib.sha256((VOICE/p).read_bytes()).hexdigest() for p in RUNTIME}}
with tarfile.open(HERE/'bundle.tar.gz','w:gz') as bundle:
    for path in RUNTIME: bundle.add(VOICE/path, arcname=path)
    for path in (VOICE/'tests').glob('test_*.py'): bundle.add(path, arcname='tests/'+path.name)
    for name in ['Dockerfile','nginx-integration-location.conf','release.py','smoke_integration.py','final_verify.py',BASE.name]:
        bundle.add(HERE/name, arcname=name)
    data=json.dumps(manifest,indent=2).encode(); item=tarfile.TarInfo('manifest.json'); item.size=len(data)
    bundle.addfile(item,io.BytesIO(data))
print(json.dumps(manifest,indent=2))
