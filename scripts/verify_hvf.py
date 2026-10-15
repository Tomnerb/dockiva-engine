#!/usr/bin/env python3
"""Build the Hypervisor.framework backend and run its milestone-1 guest; with
--root ROOTFS also run busybox from that root and compare sha256sum with the host."""
import argparse
import hashlib
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, help='an extracted alpine root (for the busybox checks)')
    args = parser.parse_args()
    subprocess.run([sys.executable, str(ROOT/'scripts/build_native.py'), '--with-hvf'], check=True, capture_output=True)
    runtime = ROOT/'build/dockiva-hvf'
    folder = ROOT/'build/hvf'
    folder.mkdir(parents=True, exist_ok=True)
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    obj, elf = folder/'hvf_hello.o', folder/'hvf_hello.elf'
    subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O1', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
                    '-c', str(ROOT/'tests/native/hvf_hello_guest.c'), '-o', str(obj)], check=True)
    subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(obj), '-o', str(elf)], check=True)
    failed = 0
    r = subprocess.run([str(runtime), 'run', str(elf), '--cpu-backend', 'hvf', '--dynamic'], capture_output=True, timeout=30)
    ok = r.returncode == 0 and r.stdout == b'hello from native arm64, 10/4*2=5\n'
    failed += not ok
    print('hvf hello:', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout!r} {r.stderr.decode().strip()}')
    # x86 translators' needs (docs/X86-IMAGES-RESEARCH.md): ID register reads, SIGBUS on
    # unaligned atomics, and hardware TSO through PR_SET_MEM_MODEL (a litmus test).
    for name in ('translator_guest', 'tso_guest'):
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O1', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
                        '-c', str(ROOT/'tests/native'/(name+'.c')), '-o', str(folder/(name+'.o'))], check=True)
        subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(folder/(name+'.o')), '-o', str(folder/(name+'.elf'))], check=True)
        extra = []
        if name == 'translator_guest':   # its root: the program as /t, /file, /d/file and /d/abs -> /file
            troot = folder/'translator-root'
            shutil.rmtree(troot, ignore_errors=True)
            (troot/'d').mkdir(parents=True)
            shutil.copy(folder/(name+'.elf'), troot/'t')
            (troot/'file').write_text('root')
            (troot/'d/file').write_text('d')
            (troot/'d/abs').symlink_to('/file')
            extra = ['--root', str(troot)]
        r = subprocess.run([str(runtime), 'run', str(folder/(name+'.elf')), '--cpu-backend', 'hvf', '--dynamic', '--process-tree', '--signals', '--cpus', '4', *extra],
                           capture_output=True, timeout=120)
        out = r.stdout.decode()
        if name == 'translator_guest':
            ok = r.returncode == 0 and out == 'translator PASS\n'
        else:   # reorderings seen without TSO prove the probe can see them; none may appear with it
            words = out.split()
            macos = int(platform.mac_ver()[0].split('.')[0])
            ok = r.returncode == 0 and (words[:1] == ['weak'] and int(words[1]) > 0 and (words[2:] == ['tso', '0'] or (macos < 15 and words[2:] == ['tso', 'unsupported'])))
        failed += not ok
        print(name.replace('_guest', '')+':', out.strip() if ok else f'FAIL exit={r.returncode} {out!r} {r.stderr.decode().strip()}')
    if args.root:
        common = ['--root', str(args.root), '--dynamic', '--cpu-backend', 'hvf', '--elf-limit-mib', '64', '--mmap-limit-mib', '2048']
        target = args.root/'lib/libcrypto.so.3'
        start = time.time()
        r = subprocess.run([str(runtime), 'run', str(args.root/'bin/busybox'), *common, '--argv0', 'sha256sum', '--', '/lib/libcrypto.so.3'], capture_output=True, timeout=60)
        elapsed = time.time()-start
        ok = r.returncode == 0 and r.stdout.split()[:1] == [hashlib.sha256(target.read_bytes()).hexdigest().encode()]
        failed += not ok
        print(f'hvf busybox sha256sum ({target.stat().st_size} bytes, {elapsed:.2f}s):', 'PASS' if ok else f'FAIL {r.stdout!r} {r.stderr.decode().strip()}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
