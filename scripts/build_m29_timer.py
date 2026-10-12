#!/usr/bin/env python3
"""Build the independent assembly and process/signal ABI probes for M29."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    folder = ROOT / 'examples/m29'
    candidates = list((Path.home() / '.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    linker = shutil.which('ld.lld') or (str(candidates[0]) if candidates else None)
    if not linker:
        raise SystemExit('ld.lld is required')
    with tempfile.TemporaryDirectory() as temp:
        for target, sources in [('timer', ['start.S', 'timer.c'])]:
            objects = []
            for source in sources:
                obj = str(Path(temp) / (source + '.o'))
                subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding',
                                '-fno-builtin', '-fno-stack-protector', '-fno-pic', *(['-mgeneral-regs-only'] if source.endswith('.c') else []),
                                '-c', str(folder / source), '-o', obj], check=True)
                objects.append(obj)
            subprocess.run([linker, '-m', 'aarch64elf', '-static', '-e', '_start', *objects,
                            '-o', str(folder / (target + '.elf'))], check=True)
    files = [p for p in folder.iterdir() if p.suffix in ('.S', '.c', '.h', '.elf')]
    (folder / 'conformance-manifest.json').write_text(json.dumps({str(p.relative_to(ROOT)):
        hashlib.sha256(p.read_bytes()).hexdigest() for p in files}, indent=2) + '\n')


if __name__ == '__main__':
    main()
