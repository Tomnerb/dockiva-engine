#!/usr/bin/env python3
"""fexecve of a script in a memfd (apk 3's package scripts) on both CPU backends:
tests/native/fexecve_guest.c, with execveat(fd, "", AT_EMPTY_PATH) and
execve("/proc/self/fd/N"). Run after scripts/build_native.py."""
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        root = t/'root'
        root.mkdir()
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
                        '-mgeneral-regs-only', '-c', str(ROOT/'tests/native/fexecve_guest.c'), '-o', str(t/'probe.o')], check=True)
        subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(t/'probe.o'), '-o', str(root/'fexec')], check=True)
        failed = 0
        for backend, binary in [('interpreter', 'dockiva-runtime'), ('hvf', 'dockiva-hvf')]:
            r = subprocess.run([str(ROOT/'build'/binary), 'run', str(root/'fexec'), '--argv0', '/fexec', '--dynamic', '--cpu-backend', backend, '--process-tree',
                                '--scheduled-network', '--memory-limit-mib', '64', '--root', str(root)], capture_output=True, timeout=60)
            ok = r.returncode == 0 and len(r.stdout) == 4 and struct.unpack('<I', r.stdout)[0] == 6
            failed += not ok
            print(f'{backend} fexecve_guest:', 'PASS' if ok else f'FAIL exit={r.returncode} {r.stdout!r} {r.stderr.decode(errors="replace")[-2000:]}')
        return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
