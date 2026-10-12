#!/usr/bin/env python3
"""Build Linux ELF probes with Zig's LLVM compiler/linker, not our ELF builder."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NAMES = ['stdout', 'stderr_exit42', 'write_return', 'zero_write', 'bad_fd', 'exit_group']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', default=shutil.which('zig'))
    args = parser.parse_args()
    if not args.zig:
        parser.error('Provide --zig /path/to/zig (tested with Zig 0.13.0)')
    compiler = str(Path(args.zig).resolve())
    source = ROOT / 'examples/independent/probe.S'
    destination = source.parent
    version = subprocess.check_output([compiler, 'version'], text=True).strip()
    manifest = {'compiler': 'Zig cc / LLVM', 'version': version,
                'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'files': []}
    with tempfile.TemporaryDirectory() as tmp:
        for case, name in enumerate(NAMES, 1):
            output = Path(tmp) / (name + '.elf')
            flags = ['cc', '-target', 'aarch64-linux-musl', '-nostdlib', '-static',
                     '-fno-pie', '-no-pie', '-Wl,--build-id=none', '-Wl,-e,_start',
                     f'-DCASE={case}']
            subprocess.run([compiler, *flags, str(source), '-o', str(output)], check=True)
            output.chmod(0o755)
            shutil.copy2(output, destination / output.name)
            manifest['files'].append({'name': output.name,
                'sha256': hashlib.sha256(output.read_bytes()).hexdigest(), 'flags': flags})
    (destination / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Built {len(NAMES)} independent ELF probes with Zig {version}')


if __name__ == '__main__':
    main()
