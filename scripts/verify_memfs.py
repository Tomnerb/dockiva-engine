#!/usr/bin/env python3
"""memfs (--tmpfs): run tests/native/memfs_guest.c on every built CPU backend (and
the sanitizer builds when present); with --root ROOTFS (an alpine-based root with
busybox) also copy, archive, compare and remove a tree in a tmpfs with busybox."""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build(source, out, linker):
    obj = out.with_suffix('.o')
    flags = ['-mgeneral-regs-only'] if source.suffix == '.c' else []
    subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
                    *flags, '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(obj), '-o', str(out)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, help='a root with /bin/busybox for the end-to-end checks')
    parser.add_argument('--cpus', type=int, default=4)
    args = parser.parse_args()
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    failed = 0
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)/'root'
        root.mkdir()
        build(ROOT/'tests/native/memfs_guest.c', root/'t', linker)
        build(ROOT/'tests/native/fork_exec_leaf.S', root/'leaf', linker)
        (root/'hostfile').write_text('host\n')
        common = ['--root', str(root), '--tmpfs', '/m', '--tmpfs', '/s:size=64k', '--memory-limit-mib', '64', '--max-steps', '2000000000',
                  '--process-tree', '--scheduled-network', '--dynamic']
        for binary in ('dockiva-runtime', 'dockiva-hvf', 'dockiva-runtime-sanitize', 'dockiva-hvf-sanitize'):
            runtime = ROOT/'build'/binary
            if not runtime.exists():
                print(f'{binary}: skipped (not built)')
                continue
            extra = ['--cpu-backend', 'hvf', '--cpus', str(args.cpus)] if 'hvf' in binary else []
            r = subprocess.run([str(runtime), 'run', str(root/'t'), *common, *extra], capture_output=True, timeout=300)
            ok = r.returncode == 0 and r.stdout == b'memfs PASS\n' and b'ERROR: AddressSanitizer' not in r.stderr
            failed += not ok
            print(f'{binary} memfs_guest:', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout!r} {r.stderr.decode()[-2000:]}')
    if args.root:
        # busybox: a tree copied in, archived, unpacked again, compared, renamed and removed.
        script = ('set -e; cp -a /usr/lib /tmp/a; tar cf /tmp/a.tar -C /tmp a; mkdir /tmp/b; tar xf /tmp/a.tar -C /tmp/b; '
                  'diff -r /tmp/a /tmp/b/a; mv /tmp/b/a /tmp/c; cmp /tmp/a.tar /tmp/a.tar; rm -r /tmp/a /tmp/b /tmp/c /tmp/a.tar; '
                  'test -z "$(ls -A /tmp)"; grep -q "^tmpfs /tmp tmpfs" /proc/mounts; echo e2e PASS')
        for binary in ('dockiva-runtime', 'dockiva-hvf'):
            runtime = ROOT/'build'/binary
            if not runtime.exists():
                continue
            extra = ['--cpu-backend', 'hvf', '--cpus', str(args.cpus)] if binary == 'dockiva-hvf' else []
            r = subprocess.run([str(runtime), 'run', str(args.root/'bin/busybox'), '--root', str(args.root), '--tmpfs', '/tmp', '--dynamic', '--process-tree',
                                '--scheduled-network', '--signals', '--memory-limit-mib', '1024', '--mmap-limit-mib', '4096', '--max-steps', '18446744073709551615',
                                *extra, '--argv0', 'sh', '--', '-c', script], capture_output=True, timeout=600)
            ok = r.returncode == 0 and r.stdout.endswith(b'e2e PASS\n')
            failed += not ok
            print(f'{binary} busybox e2e:', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout[-500:]!r} {r.stderr.decode()[-1000:]}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
