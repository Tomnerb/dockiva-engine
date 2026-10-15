#!/usr/bin/env python3
"""M23 real-PTY input, Ctrl-C, EOF, guest groups and terminal preservation."""
import argparse
import errno
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

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=ROOT / 'build/dockiva-runtime')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    native = str(args.native.resolve())
    probe = str(ROOT / 'examples/m23/probe.elf')
    rows = []

    def check(name, passed, **details):
        rows.append(dict(name=name, passed=bool(passed), **details))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(str(details))

    def command(mode, steps='10000000'):
        return [native, 'run', probe, '--interactive', '--max-steps', steps, '--', mode]

    def terminal_case(name, invocation, ready, payload, code, wanted):
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        def setup():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            os.tcsetpgrp(slave, os.getpgrp())
        child = None
        try:
            child = subprocess.Popen(invocation, stdin=slave, stdout=slave, stderr=slave,
                                     preexec_fn=setup)
            os.close(slave)
            slave = -1
            data = b''
            deadline = time.monotonic() + 15
            sent = False
            eof_sent = False
            while True:
                if not sent and ready in data:
                    if payload:
                        os.write(master, payload)
                    sent = True
                if name == "canonical_input_eof" and data.count(b"hello\r\n") >= 2 and not eof_sent:
                    os.write(master, b"\x04")
                    eof_sent = True
                readable, _, _ = select.select([master], [], [], .05)
                if readable:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError as exc:
                        if exc.errno != errno.EIO:
                            raise
                        chunk = b''
                    data += chunk
                if child.poll() is not None and (not readable or not chunk):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError('PTY timeout: ' + repr(data))
            check(name, sent and child.returncode == code and wanted in data,
                  exit_code=child.returncode, output=data.decode(errors='replace'))
            check(name + '_terminal_preserved', termios.tcgetattr(master) == before)
        finally:
            if child is not None and child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=5)
            os.close(master)
            if slave >= 0:
                os.close(slave)

    try:
        terminal_case('canonical_input_eof', command('echo-input'), b'input ready',
                      b'hello\n', 0, b'hello\r\nhello\r\n')
        terminal_case('ctrl_c_root', command('echo-input'), b'input ready', b'\x03', 130, b'input ready')
        terminal_case('ctrl_c_foreground_child', command('foreground-child', '100000000'),
                      b'foreground ready', b'\x03', 0, b'parent survived group cancellation')
        terminal_case('failure_restores_terminal', command('spin', '1000'), b'', b'', 125,
                      b'Instruction budget exhausted')
        oci = [sys.executable, str(ROOT / 'oci.py'), 'run', str(ROOT / 'examples/m23/signal-image'),
               '--native', native, '--interactive', '--max-steps', '10000000', '--', 'echo-input']
        terminal_case('oci_ctrl_c', oci, b'input ready', b'\x03', 130, b'input ready')
        terminal_case('oci_foreground_ctrl_c', [*oci[:-1], 'foreground-child'],
                      b'foreground ready', b'\x03', 0, b'parent survived group cancellation')
        terminal_case('terminal_errors', command('terminal-errors'), b'', b'', 0, b'terminal errors OK')
        for flags in [[], ['--no-decode-cache'], ['--no-lookup-cache']]:
            invocation = [native, 'run', probe, '--interactive', *flags, '--', 'group-cancel']
            result = subprocess.run(invocation, capture_output=True, timeout=10)
            check('guest_group' + ''.join(flags), result.returncode == 0 and
                  result.stdout == b'parent survived group cancellation\n' and not result.stderr)
        result = subprocess.run(command('echo-input'), input=b'piped input\n', capture_output=True, timeout=10)
        check('redirected_stdin', result.returncode == 0 and result.stdout == b'input ready\npiped input\n' and not result.stderr)
        result = subprocess.run([native, 'run', probe, '--', 'echo-input'], input=b'private\n', capture_output=True, timeout=10)
        check('default_stdin_eof', result.returncode == 0 and result.stdout == b'input ready\n' and not result.stderr)
    except Exception as exc:
        if not rows or rows[-1]['passed']:
            rows.append(dict(name='verification_error', passed=False, error=str(exc)))
            print('FAIL', exc)
    report = dict(milestone=23, host=platform.platform(), passed=bool(rows) and all(r['passed'] for r in rows),
                  results=rows, runtime_sha256=hashlib.sha256(Path(native).read_bytes()).hexdigest(),
                  source_hashes={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted((ROOT / 'native').glob('*')) if p.suffix in ('.h', '.c')})
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
