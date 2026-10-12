#!/usr/bin/env python3
"""Check shipped independent probes; optionally compare with Linux execution."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MESSAGE = b'Independent ARM64 Linux probe\n'
EXPECTED = {
    'stdout': (MESSAGE, b'', 0),
    'stderr_exit42': (b'', MESSAGE, 42),
    'write_return': (MESSAGE, b'', len(MESSAGE)),
    'zero_write': (b'', b'', 0),
    'bad_fd': (b'', b'', 247),
    'exit_group': (b'', b'', 42),
}


def capture(command):
    result = subprocess.run(command, capture_output=True, timeout=10)
    return result.stdout, result.stderr, result.returncode


def serialize(result):
    return dict(stdout_hex=result[0].hex(), stderr_hex=result[1].hex(), exit_code=result[2])


def verify(reference=None):
    directory = ROOT / 'examples/independent'
    manifest = json.loads((directory / 'build-manifest.json').read_text())
    if hashlib.sha256((directory / 'probe.S').read_bytes()).hexdigest() != manifest['source_sha256']:
        raise ValueError('Source differs from build manifest; rebuild the probes')
    hashes = {item['name']: item['sha256'] for item in manifest['files']}
    report = {'host': {'system': platform.system(), 'machine': platform.machine(),
                      'python': platform.python_version()},
              'reference': reference, 'cases': [], 'passed': True}
    for name, expected in EXPECTED.items():
        elf = directory / (name + '.elf')
        digest = hashlib.sha256(elf.read_bytes()).hexdigest()
        if digest != hashes[elf.name]:
            raise ValueError(f'Build hash mismatch: {elf.name}')
        actual = capture([sys.executable, str(ROOT / 'runtime.py'), 'run', str(elf)])
        passed = actual == expected
        row = {'name': name, 'sha256': digest, 'runtime': serialize(actual),
               'expected': serialize(expected), 'passed': passed}
        if reference is not None:
            baseline = capture([*reference, str(elf)])
            row['linux_reference'] = serialize(baseline)
            row['passed'] = passed and baseline == expected and baseline == actual
        report['cases'].append(row)
        report['passed'] = report['passed'] and row['passed']
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--native-linux', action='store_true', help='Run probes directly on ARM64 Linux')
    group.add_argument('--qemu', help='Path to qemu-aarch64 on Linux; user-mode reference only')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.native_linux and (platform.system() != 'Linux' or platform.machine() not in ('aarch64', 'arm64')):
        parser.error('--native-linux requires an ARM64 Linux host')
    if args.qemu and platform.system() != 'Linux':
        parser.error('--qemu reference requires a Linux host')
    reference = [] if args.native_linux else [args.qemu] if args.qemu else None
    try:
        report = verify(reference)
        report['reference_kind'] = 'native-arm64-linux' if args.native_linux else 'qemu-user-linux' if args.qemu else 'not-run'
        for row in report['cases']:
            print(f"{'PASS' if row['passed'] else 'FAIL'} {row['name']}")
        print('Linux reference: ' + report['reference_kind'])
        if args.report:
            args.report.write_text(json.dumps(report, indent=2) + '\n')
        return 0 if report['passed'] else 1
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
        print(f'verification failed: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
