"""Refresh SHA-256 hashes in sdk/manifest.json after editing SDK files.
Only entries already listed are updated; the toolchain contract is untouched.
Usage (repo root): python tools/sync_manifest.py"""
import hashlib, json, pathlib, re, sys
root = pathlib.Path(__file__).resolve().parent.parent
path = root / 'sdk' / 'manifest.json'
text = path.read_text(encoding='utf-8')
files = json.loads(text)['files']
changed = []
for name, old in files.items():
    new = hashlib.sha256((root / name).read_bytes()).hexdigest()
    if new != old:
        text = re.sub(r'("%s":\s*")%s(")' % (re.escape(name), old), r'\g<1>%s\g<2>' % new, text, count=1)
        changed.append(name)
path.write_text(text, encoding='utf-8', newline='')
print('updated:', ', '.join(changed) if changed else 'nothing')
