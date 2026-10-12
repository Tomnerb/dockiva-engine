#!/usr/bin/env python3
"""Compare portable guest signal statuses with a pinned ARM64 Linux image.

Docker is used only as an independent test reference, never by the runtime.
The image must already exist locally; this verifier never pulls images.
"""
import argparse
import hashlib
import json
import platform
import subprocess
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    probe = ROOT / 'examples/m22/probe.elf'
    inspected = subprocess.run(['docker', 'image', 'inspect', args.image],
                               capture_output=True, check=True, timeout=15)
    info = json.loads(inspected.stdout)[0]
    if info['Architecture'] != 'arm64' or info['Os'] != 'linux':
        parser.error('reference image must be Linux ARM64')
    native = ROOT / 'build/vmfree-c'
    actual = subprocess.run([str(native), 'run', str(probe), '--schedule', '--',
                             'portable-signal-status'], capture_output=True, timeout=20)
    name = 'vmfree-m22-reference-' + uuid.uuid4().hex
    try:
        reference = subprocess.run([
            'docker', 'run', '--pull=never', '--rm', '--name', name,
            '--network=none', '--read-only', '--cap-drop=ALL', '--pids-limit=8',
            '--memory=64m', '--cpus=1', '--user=65534:65534',
            '--mount', f'type=bind,source={probe},target=/signal-probe,readonly',
            '--entrypoint=/signal-probe', info['Id'], 'portable-signal-status',
        ], capture_output=True, timeout=30)
    finally:
        subprocess.run(['docker', 'rm', '--force', name], capture_output=True, timeout=15)
    expected = b'signal statuses: 2 9 13 15; reaped PIDs absent\n'
    def record(result):
        return dict(exit_code=result.returncode, stdout=result.stdout.decode(errors='replace'),
                    stderr=result.stderr.decode(errors='replace'))
    passed = all(r.returncode == 0 and r.stdout == expected and not r.stderr
                 for r in [actual, reference])
    report = dict(milestone=22, passed=passed, host=platform.platform(),
                  reference_kind='ARM64 Linux via local Docker; validation only',
                  reference_image=info['Id'], probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                  runtime_sha256=hashlib.sha256(native.read_bytes()).hexdigest(),
                  actual=record(actual), reference=record(reference),
                  scope='default child termination, raw wait status and reaped PID absence only')
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(('PASS' if passed else 'FAIL') + ' Linux signal-status reference')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
