#!/usr/bin/env python3
"""x86 programs FEX runs get GODEBUG=asyncpreemptoff=1 (native/fs.h x86_godebug):
Go's asynchronous preemption breaks under FEX. tests/native/x86_godebug.c, an x86-64
program printing its GODEBUG, runs through the FEX bundle (build/x86, from
scripts/build_x86.py) on the hvf backend: as a container's first program with
several GODEBUG values, and exec'd by an x86 program. Run after
scripts/build_native.py --with-hvf."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = [  # GODEBUG given (None: unset) -> what the program sees
    (None, 'asyncpreemptoff=1'),
    ('', 'asyncpreemptoff=1'),
    ('netdns=go', 'netdns=go,asyncpreemptoff=1'),
    ('asyncpreemptoff=0', 'asyncpreemptoff=0'),                 # the program's own choice stays
    ('gctrace=1,asyncpreemptoff=0', 'gctrace=1,asyncpreemptoff=0'),
]


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
        subprocess.run(['clang', '--target=x86_64-linux-gnu', '-O1', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                        '-fno-pic', '-nostdlib', '-c', str(ROOT/'tests/native/x86_godebug.c'), '-o', str(t/'g.o')], check=True)
        subprocess.run([str(linker), '-m', 'elf_x86_64', '-static', '-e', '_start', str(t/'g.o'), '-o', str(root/'godebug')], check=True)

        def run(env, *args):
            cmd = [str(ROOT/'build/dockiva-hvf'), 'run', str(root/'godebug'), '--root', str(root), '--dynamic',
                   '--writable', '--cpu-backend', 'hvf', '--process-tree', '--scheduled-network', '--signals', '--memory-limit-mib', '2048',
                   '--elf-limit-mib', '1024', '--mmap-limit-mib', '12288', '--bind-ro', str(bundle), '/.dockiva/x86',
                   '--x86-translator', '/.dockiva/x86', '--env', 'HOME=/root']
            if env is not None:
                cmd += ['--env', 'GODEBUG='+env]
            cmd += list(args)                                   # '--' and guest arguments last
            r = subprocess.run(cmd, capture_output=True, timeout=120)
            return r.returncode, r.stdout.decode(errors='replace').strip(), r.stderr.decode(errors='replace')[-300:]

        failed = 0
        for env, want in CASES:
            code, out, err = run(env)
            ok = code == 0 and out == want
            failed += not ok
            print(f'GODEBUG={env!r}:', 'PASS' if ok else f'FAIL exit={code} got {out!r} want {want!r} {err}')
        code, out, err = run('netdns=go', '--', 'exec')      # an exec with only A=1: the exec path adds it again
        ok = code == 0 and out == 'asyncpreemptoff=1'
        failed += not ok
        print('exec without GODEBUG:', 'PASS' if ok else f'FAIL exit={code} got {out!r} {err}')
        return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
