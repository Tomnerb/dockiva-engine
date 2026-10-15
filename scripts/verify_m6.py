#!/usr/bin/env python3
"""Validate local OCI loading, guest process settings, HTTP, and stop cleanup."""
import argparse
import json
import os
from pathlib import Path
import platform
import selectors
import socket
import subprocess
import sys
import tempfile
import time
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.verify_m5 import serve

class Results(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.rows = []
    def addSuccess(self, test):
        super().addSuccess(test)
        self.rows.append(dict(name=test._testMethodName.removeprefix('test_'), passed=True))
    def addFailure(self, test, err):
        super().addFailure(test, err)
        self.rows.append(dict(name=str(test), passed=False))
    def addError(self, test, err):
        super().addError(test, err)
        self.rows.append(dict(name=str(test), passed=False))

def stop_check(command):
    with tempfile.TemporaryDirectory() as temporary:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env={**os.environ, 'TMPDIR': temporary})
        try:
            ready = b''
            with selectors.DefaultSelector() as selector:
                selector.register(proc.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + 12
                while b'\n' not in ready:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('Stop check readiness timeout')
                    if not selector.select(.2):
                        continue
                    chunk = os.read(proc.stdout.fileno(), 128)
                    if not chunk:
                        raise RuntimeError('Stop check exited before readiness')
                    ready += chunk
            port = int(ready.splitlines()[0].removeprefix(b'PORT '))
            proc.terminate()
            out, err = proc.communicate(timeout=5)
            closed = False
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=1):
                    pass
            except ConnectionRefusedError:
                closed = True
            return dict(name='signal_cleanup', passed=proc.returncode == 143 and not out and not err
                        and closed and not list(Path(temporary).iterdir()), exit_code=proc.returncode)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
            proc.stdout.close(); proc.stderr.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=ROOT / 'build/dockiva-runtime')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.discover(str(ROOT / 'tests'), pattern='test_oci.py')
    result = unittest.TextTestRunner(verbosity=0, resultclass=Results).run(suite)
    rows = result.rows
    base = [sys.executable, str(ROOT / 'oci.py')]
    http = str(ROOT / 'examples/m6/http-image')
    native = ['--native', str(args.native.resolve())]
    try:
        p = subprocess.run([*base, 'inspect', http, '--ref', 'demo'], capture_output=True, timeout=10)
        info = json.loads(p.stdout) if p.returncode == 0 else {}
        rows.append(dict(name='image_inspect', passed=p.returncode == 0 and not p.stderr and
                         info.get('command') == ['/app/server'] and info.get('platform') == 'linux/arm64'))
        responses = json.loads((ROOT / 'examples/m5/responses.json').read_text())
        command = [*base, 'run', http, *native, '--', 'auto']
        for name, request, key in [('image_http', [b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n'], 'ok'),
                                   ('image_http_missing', [b'GET /missing HTTP/1.1\r\n\r\n'], 'missing')]:
            actual = serve(command, request)
            rows.append(dict(name=name, passed=actual['passed'] and
                             actual['response_hex'] == responses[key].encode().hex(), actual=actual))
        image = str(ROOT / 'examples/m6/args-image')
        for name, extra, total in [('image_cmd_env', [], 15), ('image_cmd_override', ['--', '999'], 27)]:
            p = subprocess.run([*base, 'run', image, *native, *extra], capture_output=True, timeout=10)
            rows.append(dict(name=name, passed=p.returncode == total and not p.stderr and
                             p.stdout == f'sum={total}\nMODE=oci\n'.encode(), exit_code=p.returncode))
        rows.append(stop_check(command))
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        rows.append(dict(name='integration_error', passed=False, error=str(exc)))
    report = dict(host=platform.platform(), passed=result.wasSuccessful() and all(r['passed'] for r in rows),
                  scope='OCI adapter and native interpreter; no Linux OCI reference run', cases=rows)
    for row in rows:
        print(('PASS ' if row['passed'] else 'FAIL ') + row['name'])
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1
if __name__ == '__main__':
    sys.exit(main())
