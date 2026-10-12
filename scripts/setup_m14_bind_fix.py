#!/usr/bin/env python3
"""Create a fresh benchmark rootfs and config using the updated guest."""
import json
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]

def main():
    source = ROOT / 'benchmark-config.json'
    specs = json.loads(source.read_text())
    native = [s for s in specs if s.get('kind') == 'vmfree']
    docker = [s for s in specs if s.get('kind') == 'linux-vm']
    if len(native) != 1 or len(docker) != 1:
        raise SystemExit('Expected one vmfree and one linux-vm backend')
    if docker[0]['command'][-1] != 'vmfree-benchmark:local':
        raise SystemExit('Docker command must end with vmfree-benchmark:local, without CMD overrides')
    name = 'bench-bind-' + uuid.uuid4().hex[:8]
    subprocess.run([sys.executable, str(ROOT / 'containers.py'), 'create', name,
                    str(ROOT / 'examples/m11_17/http-image'), '--read-only'], check=True)
    rootfs = Path.home() / '.vmfree/containers' / name / 'rootfs'
    native[0]['command'] = [str(ROOT / 'build/vmfree-c'), 'run', str(rootfs / 'app/server'),
                           '--root', str(rootfs), '--max-steps', '10000000', '--', '0']
    destination = ROOT / 'benchmark-config-bind-fixed.json'
    destination.write_text(json.dumps(specs, indent=2) + '\n')
    print(f'Config: {destination}')
    print(f'Private benchmark instance: {name}')

if __name__ == '__main__':
    main()
