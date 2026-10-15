#!/usr/bin/env python3
"""Real ash jobs/fg/bg, guest signal frames and stop/continue acceptance."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import pty
import select
import signal
import subprocess
import sys
import termios
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
PROBES = {
    'handler': b'signal frame, registers and SIMD restored\n',
    'bad-frame': b'bad signal frame terminated only its guest\n',
    'stop-continue': b'stop, continue and exit statuses preserved\n',
    'interrupt-read': b'blocked read signal policy preserved\n',
    'restart-read': b'blocked read signal policy preserved\n',
}
SCRIPTS = [
    ('trap_int', 'trap "echo caught" INT; kill -INT $$; echo resumed', b'caught\nresumed\n'),
    ('trap_usr1', 'trap "echo user" USR1; kill -USR1 $$; echo done', b'user\ndone\n'),
    ('child_handler', 'trap "echo child" CHLD; echo hi | cat; echo done', b'hi\nchild\ndone\n'),
    ('nested_shell', 'sh -c "echo nested | cat"; echo done', b'nested\ndone\n'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=ROOT / 'build/dockiva-runtime')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--linux-reference', action='store_true')
    args = parser.parse_args()
    native = str(args.native.resolve())
    probe = ROOT / 'examples/m25/probe.elf'
    image = json.loads((ROOT / 'examples/m24/build-manifest.json').read_text())['image_id']
    rows = []

    def check(name, good, **details):
        rows.append(dict(name=name, passed=bool(good), **details))
        print(('PASS ' if good else 'FAIL ') + name, flush=True)
        if not good:
            raise RuntimeError(str(details))

    def expect(name, result, output):
        check(name, result.returncode == 0 and result.stdout == output and not result.stderr,
              code=result.returncode, stdout=result.stdout.decode(errors='replace'), stderr=result.stderr.decode(errors='replace'))

    def oci(*command):
        return [sys.executable, str(ROOT / 'oci.py'), 'run', str(ROOT / 'examples/m24/shell-image'),
                '--native', native, '--job-control', '--max-steps', '100000000', '--', *command]

    def terminal_jobs():
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        def setup():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            os.tcsetpgrp(slave, os.getpgrp())
        child = None
        data = bytearray()
        def receive(needle, offset):
            end = time.monotonic() + 10
            def found():
                return data[offset:].count(needle[0]) >= needle[1] if isinstance(needle, tuple) else needle in data[offset:]
            while not found():
                if time.monotonic() > end:
                    raise RuntimeError('Job-control timeout: ' + repr(bytes(data)))
                if select.select([master], [], [], .05)[0]:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        chunk = b''
                    if not chunk:
                        raise RuntimeError('Terminal closed: ' + repr(bytes(data)))
                    data.extend(chunk)
        def send(payload, needle=b'/ # '):
            offset = len(data)
            os.write(master, payload)
            receive(needle, offset)
        try:
            child = subprocess.Popen(oci('-i'), stdin=slave, stdout=slave, stderr=slave, preexec_fn=setup)
            os.close(slave)
            slave = -1
            receive(b'/ # ', 0)
            send(b'cat\n', b'cat\r\n')
            send(b'foreground-data\n', (b'foreground-data\r\n', 2))
            send(b'\x1a')
            check('ctrl_z_stopped_job', b'Stopped' in data)
            send(b'jobs\n')
            send(b'bg\n')
            for _ in range(3):
                send(b'jobs\n')
                if b'Stopped (tty input)' in data:
                    break
            check('background_read_stops_group', b'Stopped (tty input)' in data)
            send(b'fg\n', b'cat\r\n')
            send(b'resumed-data\n', (b'resumed-data\r\n', 2))
            send(b'\x03')
            send(b'echo shell-survived\n')
            check('fg_and_ctrl_c_preserve_shell', b'\r\nshell-survived\r\n' in data)
            os.write(master, b'exit\n')
            end = time.monotonic() + 5
            while child.poll() is None:
                if time.monotonic() > end:
                    raise RuntimeError('Shell exit timed out')
                if select.select([master], [], [], .05)[0]:
                    try:
                        data.extend(os.read(master, 65536))
                    except OSError:
                        pass
            check('interactive_exit', child.returncode == 0, transcript=data.decode(errors='replace'))
            check('terminal_preserved', termios.tcgetattr(master) == before)
        finally:
            if child is not None and child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=5)
            if slave >= 0:
                os.close(slave)
            os.close(master)

    def linux(mode):
        name = 'vmfree-m25-reference-' + uuid.uuid4().hex
        try:
            return subprocess.run(['docker', 'run', '--rm', '--pull=never', '--name', name,
                '--network=none', '--read-only', '--cap-drop=ALL', '--pids-limit=16', '--memory=128m',
                '--cpus=1', '--mount', f'type=bind,source={probe},target=/probe,readonly',
                '--entrypoint=/probe', image, mode], capture_output=True, timeout=20)
        finally:
            subprocess.run(['docker', 'rm', '--force', name], capture_output=True, timeout=15)

    try:
        pins = json.loads((ROOT / 'examples/m25/conformance-manifest.json').read_text())
        check('fixture_hashes', all(hashlib.sha256((ROOT / k).read_bytes()).hexdigest() == v for k, v in pins.items()))
        for flags in [[], ['--no-decode-cache'], ['--no-lookup-cache']]:
            for mode, out in PROBES.items():
                result = subprocess.run([native, 'run', str(probe), '--job-control', '--max-steps',
                    '10000000', *flags, '--', mode], capture_output=True, timeout=20)
                expect(mode + ''.join(flags), result, out)
        for name, command, out in SCRIPTS:
            expect(name, subprocess.run(oci('-c', command), capture_output=True, timeout=20), out)
        terminal_jobs()
        if args.linux_reference:
            for mode, out in PROBES.items():
                expect('linux_' + mode, linux(mode), out)
    except Exception as exc:
        if not rows or rows[-1]['passed']:
            rows.append(dict(name='verification_error', passed=False, error=str(exc)))
            print('FAIL', exc)
    report = dict(milestone=25, host=platform.platform(), results=rows,
                  passed=bool(rows) and all(r['passed'] for r in rows),
                  linux_reference=image if args.linux_reference else 'not-run',
                  runtime_sha256=hashlib.sha256(Path(native).read_bytes()).hexdigest(),
                  source_hashes={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted((ROOT / 'native').glob('*')) if p.suffix in ('.h', '.c')})
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
