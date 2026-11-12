#!/usr/bin/env python3
"""x86 programs FEX runs get a vDSO: FEX finds its guest library in the bundle
through FEX_THUNKGUESTLIBS (native/fs.h x86_vdso_env). tests/native/x86_vdso.c, an
x86-64 program that calls its vDSO's clock_gettime, runs through the FEX bundle
(build/x86, from scripts/build_x86.py) on the hvf backend: as a container's first
program, with its own FEX_THUNKGUESTLIBS (kept), and exec'd by an x86 program with
an environment without it. Run after scripts/build_native.py --with-hvf."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    bundle = ROOT/'build/x86'
    if not (bundle/'bin/FEX').exists():
        print('SKIP: no FEX bundle in build/x86 (scripts/build_x86.py)')
        return 0
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        root = t/'root'
        for d in ('tmp', 'root'):                               # FEXServer's socket; FEX's state under HOME
            (root/d).mkdir(parents=True)
        (root/'s').write_text('#!/vdso\n')
        (root/'s').chmod(0o755)
        subprocess.run(['clang', '--target=x86_64-linux-gnu', '-O1', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                        '-fno-pic', '-nostdlib', '-c', str(ROOT/'tests/native/x86_vdso.c'), '-o', str(t/'v.o')], check=True)
        subprocess.run([str(linker), '-m', 'elf_x86_64', '-static', '-e', '_start', str(t/'v.o'), '-o', str(root/'vdso')], check=True)

        def run(*extra):
            cmd = [str(ROOT/'build/dockiva-hvf'), 'run', str(root/'vdso'), '--root', str(root), '--dynamic',
                   '--writable', '--cpu-backend', 'hvf', '--process-tree', '--scheduled-network', '--signals', '--memory-limit-mib', '2048',
                   '--elf-limit-mib', '1024', '--mmap-limit-mib', '12288', '--bind-ro', str(bundle), '/.dockiva/x86',
                   '--x86-translator', '/.dockiva/x86', '--env', 'HOME=/root', *extra]   # '--' and guest arguments last
            r = subprocess.run(cmd, capture_output=True, timeout=120)
            return r.returncode, r.stdout.decode(errors='replace').strip(), r.stderr.decode(errors='replace')[-300:]

        failed = 0
        for name, extra, want in [
            ('first program', (), 'vdso'),
            ('own FEX_THUNKGUESTLIBS kept', ('--env', 'FEX_THUNKGUESTLIBS=/nowhere'), 'none'),
            ('exec without it', ('--', 'exec'), 'vdso'),          # an exec with only A=1: the exec path adds it again
            ('#! script without it', ('--', 'script'), 'vdso'),   # /s's interpreter is x86: FEX runs itself again for it
        ]:
            code, out, err = run(*extra)
            ok = code == 0 and out == want
            failed += not ok
            print(f'{name}:', 'PASS' if ok else f'FAIL exit={code} got {out!r} want {want!r} {err}')
        return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
