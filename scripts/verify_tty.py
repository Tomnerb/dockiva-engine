#!/usr/bin/env python3
"""TTY output is byte-exact, CRLF as on Linux: exec -t (fast and slow readers, ONLCR/OPOST cleared, OCRNL/ONOCR/ONLRET) and run -t (development only)."""
import argparse
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost', timeout=60)
        self.path = str(path)
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, required=True)
    args = parser.parse_args()
    helper = args.helper.resolve(strict=True)
    # UTF-8 lines and binary bytes, many times the 1 KiB macOS pty buffer. No CR: the
    # terminal's ONLCR turns each LF into exactly CRLF, as on Linux (the runtime does it;
    # macOS's own would repeat a CR when its queue fills), and a guest clearing ONLCR or OPOST stops it.
    lines = b''.join(('✓ check line %d é中\U0001f600\n' % i).encode() for i in range(6000))
    binary = bytes(b for b in range(256) if b not in b'\r\n') * 400
    payload = lines + binary + lines
    with tempfile.TemporaryDirectory(prefix='dv-tty-', dir='/tmp') as tmp:
        home, data = Path(tmp)/'home', Path(tmp)/'data'
        data.mkdir()
        (data/'payload').write_bytes(payload)
        # Terminal writers that change c_oflag first (Linux bits: OPOST 1, ONLCR 4, OCRNL 8, ONOCR 16, ONLRET 32).
        linker = next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
        for name, defines in (('onlcr', ['-DSET=0', '-DCLEAR=4']), ('opost', ['-DSET=0', '-DCLEAR=1']),
                              ('ocrnl', ['-DSET=5|8|16|32', '-DCLEAR=0', '-DTEXT="\\rx\\ry\\n\\r"'])):
            subprocess.run(['clang', '--target=aarch64-linux-gnu', '-O0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
                            '-mgeneral-regs-only', *defines, '-c', str(ROOT/'tests/native/tty_oflag_guest.c'), '-o', str(data/(name+'.o'))], check=True)
            subprocess.run([str(linker), '-m', 'aarch64elf', '-static', '-e', '_start', str(data/(name+'.o')), '-o', str(data/name)], check=True)
        def token():
            return json.loads((home/'docker-client/config.json').read_text())['HttpHeaders']['X-VMFree-Token']
        def request(method, path, body=None):
            conn = UnixHTTP(home/'engine.sock')
            try:
                conn.request(method, '/v1.45'+path, json.dumps(body) if body is not None else None,
                             {'X-VMFree-Token': token(), 'Content-Type': 'application/json'})
                response = conn.getresponse()
                return response.status, response.read()
            finally:
                conn.close()
        def hijack(path, body):
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(60)
            s.connect(str(home/'engine.sock'))
            raw = json.dumps(body).encode()
            s.sendall(f'POST /v1.45{path} HTTP/1.1\r\nHost: x\r\nX-VMFree-Token: {token()}\r\nContent-Type: application/json\r\n'
                      f'Content-Length: {len(raw)}\r\nConnection: Upgrade\r\nUpgrade: tcp\r\n\r\n'.encode()+raw)
            head = b''
            while b'\r\n\r\n' not in head:
                chunk = s.recv(1)
                assert chunk, head
                head += chunk
            assert head.split()[1] == b'101', head
            return s
        def expect(method, path, code, body=None):
            actual, raw = request(method, path, body)
            assert actual == code, (method, path, actual, raw)
            return json.loads(raw) if raw.strip() else None
        def exact(name, out, crlf=True):
            want = crlf if isinstance(crlf, bytes) else payload.replace(b'\n', b'\r\n') if crlf else payload
            assert out == want, (name, out[:200], len(out), len(want), next((i for i, (a, b) in enumerate(zip(out, want)) if a != b), None))
            print('PASS', name, len(want), 'bytes', flush=True)
        engine = subprocess.Popen([str(helper), '--home', str(home)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  env=dict(os.environ, PATH='/usr/bin:/bin'))
        try:
            for _ in range(200):
                try:
                    if request('GET', '/_ping')[0] == 200: break
                except (OSError, ValueError, http.client.HTTPException): pass
                time.sleep(.05)
            mount = {'Binds': [f'{data}:/data:ro']}
            expect('POST', '/containers/create?name=held', 201, {'Image': 'dockiva/shell:demo', 'Entrypoint': ['/bin/busybox'],
                   'Cmd': ['cat'], 'OpenStdin': True, 'HostConfig': mount})
            expect('POST', '/containers/held/start', 204)
            cat = ['/bin/busybox', 'cat', '/data/payload']
            for name, delay, cmd, crlf in (('exec -t, fast reader', 0, cat, True), ('exec -t, slow reader', .002, cat, True),
                                           ('exec -t, -onlcr', 0, ['/bin/busybox', 'sh', '-c', '/data/onlcr'], False), ('exec -t, -opost, slow reader', .002, ['/bin/busybox', 'sh', '-c', '/data/opost'], False),
                                           ('exec -t, ocrnl onocr onlret', 0, ['/bin/busybox', 'sh', '-c', '/data/ocrnl'], b'x\ny\r\n')):
                job = expect('POST', '/containers/held/exec', 201, {'Cmd': cmd, 'Tty': True, 'AttachStdout': True, 'AttachStderr': True})
                s = hijack(f'/exec/{job["Id"]}/start', {'Detach': False, 'Tty': True})
                out = b''
                while chunk := s.recv(512 if delay else 65536):
                    out += chunk
                    time.sleep(delay)
                s.close()
                exact(name, out, crlf)
                assert expect('GET', f'/exec/{job["Id"]}/json', 200)['ExitCode'] == 0
            expect('POST', '/containers/create?name=run', 201, {'Image': 'dockiva/shell:demo', 'Entrypoint': ['/bin/busybox'],
                   'Cmd': ['cat', '/data/payload'], 'Tty': True, 'HostConfig': mount})
            expect('POST', '/containers/run/start', 204)
            assert expect('POST', '/containers/run/wait', 200)['StatusCode'] == 0
            exact('run -t (container log)', request('GET', '/containers/run/logs?stdout=1')[1])
            for name in ('held', 'run'):
                request('DELETE', f'/containers/{name}?force=1')
        finally:
            engine.terminate()
            try: engine.wait(10)
            except subprocess.TimeoutExpired: engine.kill(); engine.wait()

if __name__ == '__main__':
    main()
