#!/usr/bin/env python3
"""Scheduler-aware loopback sockets, readiness, timeouts and cancellation."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    'roundtrip': b'network, CPU and pipe siblings completed\n',
    'read-timeout': b'network, CPU and pipe siblings completed\n',
    'cancel-accept': b'blocked socket cancelled; CPU and pipe siblings survived\n',
    'poll-timeout': b'poll timeout completed\n',
    'accept-timeout': b'accept timeout and guest nonblocking flags preserved\n',
    'socket-boundaries': b'socket isolation boundaries preserved\n',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=ROOT / 'build/vmfree-c')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--linux-reference', action='store_true')
    parser.add_argument('--lifecycle', action='store_true')
    args = parser.parse_args()
    native = str(args.native.resolve())
    probe = ROOT / 'examples/m26/probe.elf'
    rows = []
    image = json.loads((ROOT / 'examples/m24/build-manifest.json').read_text())['image_id']

    def check(name, result, wanted, elapsed):
        good = result.returncode == 0 and result.stdout == wanted and not result.stderr
        if name.startswith(('poll-timeout', 'accept-timeout', 'read-timeout')):
            good = good and elapsed >= .075
        rows.append(dict(name=name, passed=good, exit_code=result.returncode, elapsed_seconds=elapsed,
                         stdout=result.stdout.decode(errors='replace'), stderr=result.stderr.decode(errors='replace')))
        print(('PASS ' if good else 'FAIL ') + name, flush=True)
        if not good:
            raise RuntimeError(str(rows[-1]))
    try:
        pins = json.loads((ROOT / 'examples/m26/conformance-manifest.json').read_text())
        if not all(hashlib.sha256((ROOT / k).read_bytes()).hexdigest() == v for k, v in pins.items()):
            raise RuntimeError('Fixture hash mismatch')
        for flags in [[], ['--no-decode-cache'], ['--no-lookup-cache']]:
            for case, wanted in CASES.items():
                start = time.monotonic()
                result = subprocess.run([native, 'run', str(probe), '--scheduled-network',
                    '--max-steps', '10000000', *flags, '--', case], capture_output=True, timeout=10)
                check(case + ''.join(flags), result, wanted, time.monotonic() - start)
        start = time.monotonic()
        result = subprocess.run([sys.executable, str(ROOT / 'oci.py'), 'run',
            str(ROOT / 'examples/m26/network-image'), '--native', native,
            '--scheduled-network', '--max-steps', '10000000'], capture_output=True, timeout=10)
        check('oci_scheduled_network', result, CASES['roundtrip'], time.monotonic() - start)
        if args.lifecycle:
            with tempfile.TemporaryDirectory() as temp:
                state = Path(temp) / 'state'
                def container(*command):
                    result = subprocess.run([sys.executable, str(ROOT / 'containers.py'), '--state-dir',
                        str(state), '--native', native, *command], capture_output=True, timeout=15)
                    if result.returncode:
                        raise RuntimeError(result.stderr.decode(errors='replace'))
                    return result
                container('create', 'm26', str(ROOT / 'examples/m26/network-image'),
                          '--scheduled-network', '--max-steps', '10000000')
                for iteration in range(2):
                    container('start', 'm26')
                    end = time.monotonic() + 10
                    while True:
                        info = json.loads(container('inspect', 'm26').stdout)
                        if info['status']['state'] in ('exited', 'failed'):
                            break
                        if time.monotonic() > end:
                            raise RuntimeError('Scheduled network container timed out')
                        time.sleep(.05)
                    good = info['status']['exit_code'] == 0 and info['config']['scheduled_network']
                    rows.append(dict(name='managed_status_' + str(iteration), passed=bool(good)))
                    if not good:
                        raise RuntimeError(str(info))
                    check('managed_logs_' + str(iteration), container('logs', 'm26'), CASES['roundtrip'] * (iteration + 1), 0)
                container('rm', 'm26')
                rows.append(dict(name='managed_removed', passed=not (state / 'm26').exists()))
        if args.linux_reference:
            for case, wanted in CASES.items():
                if case == 'socket-boundaries':
                    continue
                name = 'vmfree-m26-reference-' + uuid.uuid4().hex
                start = time.monotonic()
                try:
                    result = subprocess.run(['docker', 'run', '--rm', '--pull=never', '--name', name,
                        '--network=none', '--read-only', '--cap-drop=ALL', '--pids-limit=16', '--memory=128m',
                        '--cpus=1', '--mount', f'type=bind,source={probe},target=/probe,readonly',
                        '--entrypoint=/probe', image, case], capture_output=True, timeout=15)
                    check('linux_' + case, result, wanted, time.monotonic() - start)
                finally:
                    subprocess.run(['docker', 'rm', '--force', name], capture_output=True, timeout=15)
    except Exception as exc:
        if not rows or rows[-1]['passed']:
            rows.append(dict(name='verification_error', passed=False, error=str(exc)))
            print('FAIL', exc)
    report = dict(milestone=26, host=platform.platform(), results=rows,
                  passed=bool(rows) and all(r['passed'] for r in rows),
                  linux_reference=image if args.linux_reference else 'not-run',
                  runtime_sha256=hashlib.sha256(Path(native).read_bytes()).hexdigest(),
                  source_hashes={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted((ROOT / 'native').glob('*')) if p.suffix in ('.h', '.c')})
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
