#!/usr/bin/env python3
"""Pin and package the unmodified ARM64 BusyBox 1.37.0 musl image executable.

Docker only extracts a fixture here. The runtime executes the resulting ELF itself.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.build_m6 import tar_bytes, write_layout

IMAGE = 'busybox@sha256:fc6dddc4c44b1bfe37f41cae8e67d1693828e8f42a91862816d7953e2c9d3f23'


def main():
    folder = ROOT / 'examples/m24'
    folder.mkdir(exist_ok=True)
    info = json.loads(subprocess.check_output(['docker', 'image', 'inspect', IMAGE], timeout=15))[0]
    if info['Architecture'] != 'arm64' or info['Os'] != 'linux':
        raise SystemExit('Expected pinned Linux ARM64 image')
    name = 'vmfree-m24-fixture-' + uuid.uuid4().hex
    try:
        subprocess.run(['docker', 'create', '--name', name, '--network=none', '--read-only',
                        info['Id']], check=True, capture_output=True, timeout=15)
        subprocess.run(['docker', 'cp', name + ':/bin/busybox', str(folder / 'busybox.elf')],
                       check=True, capture_output=True, timeout=15)
    finally:
        subprocess.run(['docker', 'rm', name], capture_output=True, timeout=15)
    data = (folder / 'busybox.elf').read_bytes()
    entries = [('bin/' + name, data, tarfile.REGTYPE, 0o755) for name in ['busybox', 'sh', 'cat', 'echo']]
    entries.append(('hello.txt', b'Hello from M24!\n', tarfile.REGTYPE, 0o644))
    write_layout(folder / 'shell-image', [tar_bytes(entries)],
                 dict(Entrypoint=['/bin/sh'], Cmd=['-c', 'echo M24'], Env=['PATH=/bin']), compressed=True)
    manifest = dict(image=IMAGE, image_id=info['Id'], architecture='arm64', os='linux',
                    modifications='none; executable copied verbatim from the pinned public image',
                    sha256={'examples/m24/busybox.elf': hashlib.sha256(data).hexdigest()})
    (folder / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Pinned M24 unmodified BusyBox fixture and OCI image')


if __name__ == '__main__':
    main()
