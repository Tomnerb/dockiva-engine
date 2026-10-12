#!/usr/bin/env python3
"""Extract pinned, unmodified Alpine ARM64 musl and dynamic BusyBox fixtures."""
import hashlib,json,subprocess,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
IMAGE='alpine@sha256:4bcff63911fcb4448bd4fdacec207030997caf25e9bea4045fa6c8c44de311d1'
f=ROOT/'examples/m28';root=f/'root';(root/'bin').mkdir(parents=True,exist_ok=True);(root/'lib').mkdir(exist_ok=True)
i=json.loads(subprocess.check_output(['docker','image','inspect',IMAGE],timeout=15))[0]
assert i['Architecture']=='arm64' and i['Os']=='linux'
name='vmfree-m28-fixture-'+uuid.uuid4().hex
try:
    subprocess.run(['docker','create','--name',name,'--network=none','--read-only',i['Id']],check=True,capture_output=True,timeout=15)
    for path in ['bin/busybox','lib/ld-musl-aarch64.so.1']:
        subprocess.run(['docker','cp','-L',name+':/'+path,str(root/path)],check=True,capture_output=True,timeout=15)
finally:
    subprocess.run(['docker','rm',name],capture_output=True,timeout=15)
import shutil
shutil.copy2(root/'lib/ld-musl-aarch64.so.1',root/'lib/libc.musl-aarch64.so.1')
for app in ['sh','cat','echo']:shutil.copy2(root/'bin/busybox',root/'bin'/app)
(f/'build-manifest.json').write_text(json.dumps(dict(image=IMAGE,image_id=i['Id'],modifications='ELF files copied verbatim; link aliases materialized as regular files for current root policy',sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}),indent=2)+'\n')
