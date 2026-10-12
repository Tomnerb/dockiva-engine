#!/usr/bin/env python3
"""Build the independent assembly and process/signal ABI probes for M31."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.build_m6 import tar_bytes, write_layout


def main():
    folder = ROOT / 'examples/m31'
    candidates = list((Path.home() / '.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    linker = shutil.which('ld.lld') or (str(candidates[0]) if candidates else None)
    if not linker:
        raise SystemExit('ld.lld is required')
    with tempfile.TemporaryDirectory() as temp:
        for target, sources in [('probe', ['start.S', 'probe.c'])]:
            objects = []
            for source in sources:
                obj = str(Path(temp) / (source + '.o'))
                subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding',
                                '-fno-builtin', '-fno-stack-protector', '-fno-pic', *(['-mgeneral-regs-only'] if source.endswith('.c') else []),
                                '-c', str(folder / source), '-o', obj], check=True)
                objects.append(obj)
            subprocess.run([linker, '-m', 'aarch64elf', '-static', '-e', '_start', *objects,
                            '-o', str(folder / (target + '.elf'))], check=True)
    write_layout(folder / 'resource-image', [tar_bytes([('bin/probe', (folder / 'probe.elf').read_bytes(), tarfile.REGTYPE, 0o755)])],
                 dict(Entrypoint=['/bin/probe'], Cmd=[], Env=[]), compressed=True)
    files = [p for p in folder.iterdir() if p.suffix in ('.S', '.c', '.h', '.elf')]
    (folder / 'conformance-manifest.json').write_text(json.dumps({str(p.relative_to(ROOT)):
        hashlib.sha256(p.read_bytes()).hexdigest() for p in files}, indent=2) + '\n')


if __name__ == '__main__':
    main()
