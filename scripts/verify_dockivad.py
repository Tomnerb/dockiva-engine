#!/usr/bin/env python3
"""Go/C packaged control-plane recovery and legacy-state migration (development only)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost', timeout=15)
        self.path = str(path)
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)

def until(fn, seconds=10):
    end = time.monotonic()+seconds
    last = None
    while time.monotonic() < end:
        try:
            value = fn()
            if value:
                return value
        except (OSError, ValueError, http.client.HTTPException) as exc:
            last = exc
        time.sleep(.05)
    raise AssertionError(f'timeout: {last}')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    helper = args.helper.resolve(strict=True)
    report = {'checks': [], 'helper_sha256': hashlib.sha256(helper.read_bytes()).hexdigest()}
    def check(name, condition=True):
        assert condition, name
        report['checks'].append(name)
        print('PASS', name, flush=True)
    with tempfile.TemporaryDirectory(prefix='dv-go-', dir='/tmp') as tmp:
        home = Path(tmp)
        engine = None
        def token():
            return json.loads((home/'docker-client/config.json').read_text())['HttpHeaders']['X-VMFree-Token']
        def api(method, path, body=None, secret=None):
            conn = UnixHTTP(home/'engine.sock')
            try:
                conn.request(method, '/v1.45'+path, json.dumps(body) if body is not None else None,
                             {'X-VMFree-Token': token() if secret is None else secret, 'Content-Type': 'application/json'})
                response = conn.getresponse()
                return response.status, response.read()
            finally:
                conn.close()
        def expect(method, path, code, body=None):
            actual, raw = api(method, path, body)
            assert actual == code, (method, path, actual, raw)
            return raw
        def launch():
            with (home/'engine.log').open('ab') as log:
                p = subprocess.Popen([str(helper), '--home', str(home), '--max-containers', '8'], stdout=log, stderr=log,
                                     env=dict(os.environ, PATH='/usr/bin:/bin'))
            def ready():
                if p.poll() is not None:
                    raise AssertionError((home/'engine.log').read_text())
                return api('GET', '/_ping')[0] == 200
            try:
                until(ready)
                return p
            except BaseException:
                p.kill(); p.wait(); raise
        def status(name):
            return json.loads((home/'containers'/name/'status.json').read_text())
        def port(name):
            raw = expect('GET', f'/containers/{name}/logs?stdout=1&tail=1', 200)
            match = re.search(rb'PORT (\d+)', raw)
            return int(match[1]) if match else None
        def fetch(p):
            conn = http.client.HTTPConnection('127.0.0.1', p, timeout=3)
            try:
                conn.request('GET', '/')
                response = conn.getresponse()
                assert response.status == 200
                assert b'Persistent VM-free HTTP server OK' in response.read()
                return True
            finally:
                conn.close()
        def closed(p):
            try:
                conn = socket.create_connection(('127.0.0.1', p), timeout=.2)
                conn.close()
                return False
            except OSError:
                return True
        def legacy(*arguments):
            return subprocess.run([sys.executable, str(ROOT/'containers.py'), '--state-dir', str(home/'containers'),
                                   '--native', str(helper.parent/'_internal/build/dockiva-runtime'), *arguments],
                                  capture_output=True, text=True, check=True, timeout=20)
        def processes():
            rows = []
            for line in subprocess.check_output(['ps', '-axo', 'pid=,ppid=,command='], text=True).splitlines():
                pid, parent, command = line.split(None, 2)
                rows.append((int(pid), int(parent), command))
            return rows
        try:
            # Prepare a running Python supervisor before Go starts, using the old disk/control format.
            legacy('create', 'legacy', str(ROOT/'examples/m11_17/http-image'), '--arg=0', '--read-only')
            legacy('start', 'legacy')
            old = status('legacy')
            engine = launch()
            check('running legacy state discovered without duplicate worker', status('legacy')['pid'] == old['pid'])
            until(lambda: port('legacy') and fetch(port('legacy')))
            expect('POST', '/containers/legacy/stop', 204)
            expect('POST', '/containers/legacy/start', 204)
            check('legacy container restarted by Go supervisor', status('legacy')['supervisor'] == 'dockivad')
            until(lambda: port('legacy') and fetch(port('legacy')))
            legacy('stop', 'legacy')
            check('old CLI can stop Go supervisor for rollback', status('legacy')['state'] == 'exited')
            expect('DELETE', '/containers/legacy', 204)
            version = json.loads(expect('GET', '/version', 200))
            check('Go control plane reports its implementation', version['GoVersion'].startswith('go') and version['Components'][0]['Name'] == 'dockivad')
            check('authentication rejects wrong token', api('GET', '/_ping', secret='wrong')[0] == 401)
            duplicate = subprocess.run([str(helper), '--home', str(home)], capture_output=True, timeout=10)
            check('duplicate daemon refused', duplicate.returncode == 125)
            check('three prepared images discovered', len(json.loads(expect('GET', '/images/json', 200))) == 3)
            expect('POST', '/containers/create?name=shell', 201,
                   {'Image':'dockiva/shell:demo', 'Cmd':['-c', 'busybox echo go-control; probe exit 7']})
            expect('POST', '/containers/shell/start', 204)
            result = json.loads(expect('POST', '/containers/shell/wait', 200))
            check('exit status and framed logs preserved', result['StatusCode'] == 7 and b'go-control' in expect('GET', '/containers/shell/logs?stdout=1', 200))
            for _ in range(20):
                expect('POST', '/containers/shell/restart', 204)
            expect('POST', '/containers/shell/wait', 200)
            expect('DELETE', '/containers/shell', 204)
            check('20 immediate restarts and removal tolerate natural exit and supervisor cleanup')
            # Image layers: each container's changes stay in its own directory, over the image's
            # files, which all its containers share and none changes; diff, export and commit read them.
            image_root = next(p.parent for p in (home/'images').glob('*/image.json') if json.loads(p.read_text())['name'] == 'dockiva/shell:demo')/'rootfs'
            def image_files():
                return sorted((str(p.relative_to(image_root)), p.lstat().st_mode, p.lstat().st_mtime_ns, p.lstat().st_size, p.lstat().st_ino) for p in image_root.rglob('*'))
            before = image_files()
            def run(name, image, script):
                expect('POST', f'/containers/create?name={name}', 201, {'Image':image, 'Entrypoint':['/bin/busybox'], 'Cmd':['sh', '-c', script]})
                expect('POST', f'/containers/{name}/start', 204)
                return json.loads(expect('POST', f'/containers/{name}/wait', 200))['StatusCode'], expect('GET', f'/containers/{name}/logs?stdout=1', 200)
            code, _ = run('layered', 'dockiva/shell:demo', 'busybox mkdir /new && busybox echo hi > /new/f && busybox echo changed > /hello.txt')
            changes = {(c['Path'], c['Kind']) for c in json.loads(expect('GET', '/containers/layered/changes', 200))}
            check('layered container: docker diff shows its new and changed files (deletions: verify_overlay)', code == 0 and
                  changes == {('/hello.txt', 0), ('/new', 1), ('/new/f', 1)})
            exported = tarfile.open(fileobj=io.BytesIO(expect('GET', '/containers/layered/export', 200)))
            check('docker export: the files as the container sees them', 'new/f' in exported.getnames() and 'bin/busybox' in exported.getnames() and
                  exported.extractfile('hello.txt').read() == b'changed\n')
            expect('POST', '/commit?container=layered&repo=layered-commit&tag=1', 201)
            code, out = run('committed', 'layered-commit:1', 'busybox cat /new/f /hello.txt')
            check('docker commit: the new image holds the changes', code == 0 and b'hi' in out and b'changed' in out)
            code, out = run('pristine', 'dockiva/shell:demo', 'busybox cat /hello.txt; busybox ls /new 2>&1')
            check('the image files are unchanged, and another container sees them', b'Hello from' in out and b'No such file' in out and image_files() == before)
            for n in ('layered', 'committed', 'pristine'):
                expect('DELETE', f'/containers/{n}', 204)
            expect('DELETE', '/images/layered-commit:1', 200)
            # docker run --rm, as the docker CLI issues it: create (an engine-made name), wait
            # for removal, start. A new executable path per container once cost each start
            # an online macOS malware scan (~280 ms) whenever the last one was not recent.
            def run_rm():
                began = time.monotonic()
                cid = json.loads(expect('POST', '/containers/create', 201,
                                        {'Image':'dockiva/shell:demo', 'Cmd':['-c', 'exit 3'], 'HostConfig':{'AutoRemove':True}}))['Id']
                conn = UnixHTTP(home/'engine.sock')
                try:
                    conn.request('POST', f'/v1.45/containers/{cid}/wait?condition=removed', None, {'X-VMFree-Token': token()})
                    waiting = conn.getresponse() # headers come once the wait is registered
                    expect('POST', f'/containers/{cid}/start', 204)
                    code = json.loads(waiting.read())['StatusCode']
                finally:
                    conn.close()
                return time.monotonic()-began, code
            runs = []
            for _ in range(20):
                time.sleep(.25)
                runs.append(run_rm())
            seconds = sorted(t for t, _ in runs)
            report['run_rm_seconds'] = [round(t, 3) for t, _ in runs]
            check('20 docker run --rm: exit statuses kept, all removed', all(code == 3 for _, code in runs) and not list((home/'containers').glob('[0-9a-f]'*24)))
            check(f'20 docker run --rm: median {seconds[10]:.3f} s under 0.2 s, at most 3 over 0.25 s', seconds[10] < .2 and seconds[-4] < .25)
            for i in range(9):
                expect('POST', f'/containers/create?name=server-{i}', 201,
                       {'Image':'dockiva/server:demo', 'Cmd':['0'], 'HostConfig':{'ReadonlyRootfs':True}})
            with ThreadPoolExecutor(max_workers=9) as pool:
                results = list(pool.map(lambda i: api('POST', f'/containers/server-{i}/start')[0], range(9)))
            check('concurrent admission: --max-containers 8 admits eight runs, ninth rejected', sorted(results) == [204]*8+[409])
            active = [f'server-{i}' for i, code in enumerate(results) if code == 204]
            ports = {n:until(lambda n=n: port(n)) for n in active}
            old_states = {n:status(n) for n in active}
            for p in ports.values(): fetch(p)
            rows = processes()
            owned = {pid for pid, parent, cmd in rows if str(home) in cmd}
            while True:
                expanded = owned | {pid for pid, parent, cmd in rows if parent in owned}
                if expanded == owned: break
                owned = expanded
            commands = [cmd for pid, parent, cmd in rows if pid in owned]
            check('live daemon/supervisor/guard/runtime tree contains no Python', len(commands) >= 13 and all('python' not in cmd.lower() for cmd in commands))
            report['runtime_process_count'] = len(commands)
            report['executables'] = sorted({cmd.split()[0] for cmd in commands})
            secret = token()
            engine.kill(); engine.wait()
            for p in ports.values(): fetch(p)
            engine = launch()
            check('token rotates after daemon crash', token() != secret and api('GET', '/_ping', secret=secret)[0] == 401)
            for n in active:
                check('daemon recovery preserves worker '+n, status(n) == old_states[n])
                fetch(ports[n])
                expect('POST', f'/containers/{n}/start', 204)
                assert status(n)['pid'] == old_states[n]['pid']
            name = active[0]
            os.kill(status(name)['supervisor_pid'], signal.SIGKILL)
            until(lambda: closed(ports[name]))
            until(lambda: json.loads(expect('GET', f'/containers/{name}/json', 200))['State']['Dead'])
            check('supervisor crash closes worker listener and reconciles state')
            expect('POST', f'/containers/{name}/start', 204)
            until(lambda: port(name) and fetch(port(name)))
            check('failed supervisor can be restarted safely')
            os.kill(status(name)['pid'], signal.SIGKILL)
            until(lambda: status(name)['state'] == 'exited')
            until(lambda: closed(port(name)))
            check('guard crash also removes native worker')
            for n in active:
                expect('POST', f'/containers/{n}/stop', 204)
            check('no hidden partial container publication', not list((home/'containers').glob('.creating-*')))
            files = list(helper.parent.rglob('*'))
            check('bundle has no CPython, .py or .pyc files', all('python' not in p.name.lower() and p.suffix not in ('.py','.pyc') for p in files))
        finally:
            # Only this test's private containers; compatible control protocol works across both engines.
            for folder in (home/'containers').glob('*'):
                if folder.is_dir() and (folder/'status.json').exists():
                    try: legacy('stop', folder.name)
                    except (subprocess.SubprocessError, OSError): pass
            if engine is not None and engine.poll() is None:
                engine.terminate()
                try: engine.wait(5)
                except subprocess.TimeoutExpired: engine.kill(); engine.wait()
    report['source_hashes'] = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in sorted((ROOT/'dockivad').glob('*.go'))}
    report['source_hashes']['native/worker_launcher.c'] = hashlib.sha256((ROOT/'native/worker_launcher.c').read_bytes()).hexdigest()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n')

if __name__ == '__main__':
    main()
