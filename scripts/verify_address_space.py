#!/usr/bin/env python3
"""Test address-space ownership and backend mapping coherence."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, default=ROOT/'evidence/address-space/memory-tests.json')
    args = parser.parse_args()
    folder = ROOT / 'build/address-space'
    folder.mkdir(exist_ok=True)
    source = ROOT / 'tests/native/address_space_host.c'
    rows = []
    for name in ('interpreter-sanitize',):
        binary = folder / name
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
        libraries = []
        flags += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(['cc', *flags, str(source), *libraries, '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        rows.append(dict(test=name, passed=result.returncode == 0, exit=result.returncode,
                         stdout=result.stdout, stderr=result.stderr,
                         binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
        print(name, 'PASS' if result.returncode == 0 else rows[-1], flush=True)
    # Guest x0 shaped like an SMCCC function ID must survive the hypervisor's exits.
    guest = ROOT / 'tests/native/smccc_x0_guest.c'
    obj, elf = folder / 'smccc_x0.o', folder / 'smccc_x0.elf'
    linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-fno-pic', '-mgeneral-regs-only', '-c', str(guest), '-o', str(obj)], check=True)
    subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(obj), '-o', str(elf)], check=True)
    for backend, binary in [('interpreter', 'dockiva-runtime'), ('hvf', 'dockiva-hvf')]:
        cmd = [str(ROOT/'build'/binary), 'run', str(elf), '--dynamic', '--cpu-backend', backend, '--mmap-limit-mib', '4096',
               *(['--process-tree', '--scheduled-network'] if backend == 'hvf' else [])]
        result = subprocess.run(cmd, capture_output=True, timeout=60)
        checks = struct.unpack('<I', result.stdout)[0] if len(result.stdout) == 4 else None
        rows.append(dict(test='smccc-x0-'+backend, passed=result.returncode == 0 and checks == 640, exit=result.returncode,
                         checks=checks, stderr=result.stderr.decode(errors='replace')))
        print(rows[-1]['test'], 'PASS' if rows[-1]['passed'] else rows[-1], flush=True)
    # Linux's address space beyond the arena (reservations, MAP_FIXED anywhere, mremap),
    # and a static PIE loaded where Linux puts it.
    guests = []
    for name, expected, pie in (('wide_mmap_guest', 222, False), ('pie_base_guest', 5, True)):
        source_file = ROOT/'tests/native'/(name+'.c')
        obj, elf = folder/(name+'.o'), folder/(name+'.elf')
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                        '-fPIE' if pie else '-fno-pic', '-mgeneral-regs-only', '-c', str(source_file), '-o', str(obj)], check=True)
        subprocess.run([str(linker), '-m', 'aarch64elf', '-static', *(['-pie', '--no-dynamic-linker'] if pie else []), '-e', '_start',
                        str(obj), '-o', str(elf)], check=True)
        guests.append(source_file)
        for backend, binary in [('interpreter', 'dockiva-runtime'), ('hvf', 'dockiva-hvf')]:
            cmd = [str(ROOT/'build'/binary), 'run', str(elf), '--dynamic', '--cpu-backend', backend, '--mmap-limit-mib', '64',
                   '--process-tree', '--scheduled-network']
            result = subprocess.run(cmd, capture_output=True, timeout=120)
            checks = struct.unpack('<I', result.stdout)[0] if len(result.stdout) == 4 else None
            rows.append(dict(test=name.removesuffix('_guest').replace('_', '-')+'-'+backend, passed=result.returncode == 0 and checks == expected,
                             exit=result.returncode, checks=checks, stderr=result.stderr.decode(errors='replace')))
            print(rows[-1]['test'], 'PASS' if rows[-1]['passed'] else rows[-1], flush=True)
    report = args.report
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(dict(results=rows, source_hashes={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [source, guest, *guests, *sorted((ROOT/'native').glob('*.h')), ROOT/'native/runtime.c']}), indent=2)+'\n')
    return 0 if all(row['passed'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
