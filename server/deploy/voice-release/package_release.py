"""Generate a source-only release bundle; excludes credentials, builds and device files."""
import hashlib
import io
import json
from pathlib import Path
import tarfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / 'main' / 'xiaozhi-server'
FILES = [
    'core/connection.py', 'core/providers/asr/aliyunbl_stream.py', 'core/providers/asr/base.py',
    'core/handle/textHandler/listenMessageHandler.py', 'core/handle/sendAudioHandle.py',
    'core/utils/audioRateController.py', 'core/utils/asr_queue.py',
    'core/providers/llm/openai/openai.py',
]
manifest = {'files': {name: hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() for name in FILES}}
with tarfile.open(HERE / 'bundle.tar.gz', 'w:gz') as bundle:
    for name in FILES:
        bundle.add(SOURCE / name, arcname=name)
    for path in sorted((SOURCE / 'tests').glob('test_*.py')):
        bundle.add(path, arcname='tests/' + path.name)
    for name in ('Dockerfile', '.dockerignore', 'release.py', 'model_admin.py', 'smoke_voice.py'):
        bundle.add(HERE / name, arcname=name)
    data = json.dumps(manifest, indent=2).encode()
    item = tarfile.TarInfo('manifest.json')
    item.size = len(data)
    bundle.addfile(item, io.BytesIO(data))
print(json.dumps({'bundle': str(HERE / 'bundle.tar.gz'),
    'sha256': hashlib.sha256((HERE / 'bundle.tar.gz').read_bytes()).hexdigest(), 'source_files': len(FILES)}))
