"""Package and deploy the device-list frontend only; preserve old hashed assets."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tarfile
from datetime import datetime, timezone


ROOT = Path('/opt/xiaozhi-server/manager-web')
EXPECTED_ENTRY = '7905107ecf69477a8d7c9f60108c94cc659dd6db15523caeac6bb77bd284a392'


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def safe_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name:
        raise ValueError('Unsafe archive path: ' + name)
    return path


def package(build, baseline, output):
    if digest(baseline / 'index.html') != EXPECTED_ENTRY:
        raise RuntimeError('Local baseline differs from the verified production entry')
    files = {}
    changed = []
    for source in sorted(build.rglob('*')):
        if not source.is_file():
            continue
        name = source.relative_to(build).as_posix()
        safe_path(name)
        files[name] = digest(source)
        previous = baseline / name
        if not previous.is_file() or digest(previous) != files[name]:
            changed.append(name)
    manifest = {'expected_entry': EXPECTED_ENTRY, 'files': files, 'changed': changed}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, 'w:gz') as archive:
        payload = json.dumps(manifest, indent=2).encode()
        info = tarfile.TarInfo('manifest.json')
        info.size = len(payload)
        archive.addfile(info, io.BytesIO(payload))
        for name in changed:
            archive.add(build / name, arcname='build/' + name, recursive=False)
    print(json.dumps({'files': len(files), 'changed': len(changed),
                      'archive_bytes': output.stat().st_size,
                      'archive_sha256': digest(output),
                      'index_sha256': files['index.html']}, indent=2))


def atomic_copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + '.device-list-new')
    shutil.copyfile(source, temporary)
    os.chmod(temporary, 0o644)
    os.replace(temporary, target)


def deploy(archive_path):
    deployment = archive_path.parent
    if ROOT.resolve() != ROOT or not ROOT.is_dir():
        raise RuntimeError('Unexpected web root')
    backup = deployment / 'web-before.tar.gz'
    if backup.exists():
        raise RuntimeError('Backup already exists; inspect the previous deployment first')
    if digest(ROOT / 'index.html') != EXPECTED_ENTRY:
        raise RuntimeError('Production entry changed; stop for review')
    staged = deployment / 'staged'
    staged.mkdir(exist_ok=False)
    with tarfile.open(archive_path, 'r:gz') as archive:
        for member in archive.getmembers():
            safe_path(member.name)
            if not member.isfile():
                raise RuntimeError('Archive must contain regular files only')
            target = staged / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as source, target.open('wb') as dest:
                shutil.copyfileobj(source, dest)
    manifest = json.loads((staged / 'manifest.json').read_text())
    if manifest['expected_entry'] != EXPECTED_ENTRY:
        raise RuntimeError('Unexpected package baseline')
    changed = set(manifest['changed'])
    for name, expected in manifest['files'].items():
        safe_path(name)
        if (ROOT / name).resolve() != ROOT / name:
            raise RuntimeError('Unexpected symlink in web root: ' + name)
        source = staged / 'build' / name if name in changed else ROOT / name
        if not source.is_file() or digest(source) != expected:
            raise RuntimeError('Package or reused asset mismatch: ' + name)
    with tarfile.open(backup, 'w:gz') as archive:
        archive.add(ROOT, arcname='manager-web')
    os.chmod(backup, 0o600)
    if digest(ROOT / 'index.html') != EXPECTED_ENTRY:
        raise RuntimeError('Production entry changed during backup')
    # Assets first, HTML entry next, service worker last to precache the new entry.
    priority = lambda name: (2 if name.startswith('service-worker.js') else
                             1 if name.startswith('index.html') else 0, name)
    for name in sorted(changed, key=priority):
        atomic_copy(staged / 'build' / name, ROOT / name)
    removed_sidecars = []
    for name in changed:
        if name.endswith(('.gz', '.br')):
            continue
        for suffix in ('.gz', '.br'):
            sidecar = name + suffix
            path = ROOT / sidecar
            if sidecar not in manifest['files'] and path.is_file():
                path.unlink()
                removed_sidecars.append(sidecar)
    for name, expected in manifest['files'].items():
        if digest(ROOT / name) != expected:
            raise RuntimeError('Installed asset mismatch: ' + name)
    result = {'deployed_at_utc': datetime.now(timezone.utc).isoformat(),
              'backup': str(backup), 'files_verified': len(manifest['files']),
              'files_changed': len(changed), 'removed_stale_sidecars': removed_sidecars,
              'old_index_sha256': EXPECTED_ENTRY,
              'new_index_sha256': manifest['files']['index.html'],
              'archive_sha256': digest(archive_path)}
    (deployment / 'deployment-result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest='command', required=True)
    pack = commands.add_parser('package')
    pack.add_argument('--build', type=Path, required=True)
    pack.add_argument('--baseline', type=Path, required=True)
    pack.add_argument('--output', type=Path, required=True)
    install = commands.add_parser('deploy')
    install.add_argument('--archive', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'package':
        package(args.build.resolve(), args.baseline.resolve(), args.output.resolve())
    else:
        deploy(args.archive.resolve())
