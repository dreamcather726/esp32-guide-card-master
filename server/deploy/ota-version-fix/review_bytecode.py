"""Prove the compiled class changes only compareVersions before replacing it."""
import hashlib
import difflib
import json
from pathlib import Path
import re
import subprocess
import sys

folder = Path(sys.argv[1])


def methods(path):
    output = subprocess.check_output(['javap', '-J-Dfile.encoding=UTF-8',
        '-J-Dstdout.encoding=UTF-8', '-p', '-c', str(path)], text=True, encoding='utf-8')
    result = {}
    current = None
    for line in output.splitlines():
        if re.match(r'^  (public|private|protected|static).*\(.*\);$', line) or line == '  static {};':
            current = line.strip()
            result[current] = []
        elif current and line != '}':
            line = re.sub(r'#\d+', '#CP', line)
            line = re.sub(r'\s+//', ' //', line)
            result[current].append(line.rstrip())
    return result


old, new = methods(folder / 'before.class'), methods(folder / 'after.class')
changed = sorted(k for k in old.keys() | new.keys() if old.get(k) != new.get(k))
for key in changed:
    if 'compareVersions(' not in key:
        print('\n'.join(difflib.unified_diff(old.get(key, []), new.get(key, []), fromfile=key, tofile=key)))
assert changed == ['private static int compareVersions(java.lang.String, java.lang.String);'], changed
result = {'changed_methods': changed}
for name in ('before', 'after'):
    result[name + '_sha256'] = hashlib.sha256((folder / (name + '.class')).read_bytes()).hexdigest()
(folder / 'bytecode-review.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result))
