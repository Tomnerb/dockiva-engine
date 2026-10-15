#!/usr/bin/env python3
"""Local prototype container lifecycle. No Docker daemon or Linux VM required."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import hmac
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
import oci

ROOT = Path(__file__).resolve().parent

def save(path, data):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(data, indent=2) + '\n')
    os.replace(tmp, path)

def read(path):
    return json.loads(path.read_text())

def container(store, name):
    if not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_-]{0,39}', name):
        raise ValueError('Invalid container name')
    path = store / name
    if path.is_symlink():
        raise ValueError('Container path is a symlink')
    return path

def lock(path):
    fd = os.open(path / 'lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        raise ValueError('Container is running or another operation is in progress')
    return fd

def reserve_capacity(store, target):
    """Serialize admission; each live supervisor retains its container lock.

    Reservations cover up to four interpreters / 1024 MiB of guest allocator
    budgets per state store. Host RSS, other stores and direct runs are separate.
    """
    gate=os.open(store / '.capacity.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(gate, fcntl.LOCK_EX)
        count=1;memory=int(read(target / 'config.json').get('memory_limit_mib',256))
        for item in store.iterdir():
            if item==target or item.is_symlink() or not item.is_dir() or not (item/'config.json').is_file():continue
            fd=os.open(item/'lock',os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW,0o600)
            try:
                try:fcntl.flock(fd,fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    count+=1;memory+=int(read(item/'config.json').get('memory_limit_mib',256))
            finally:os.close(fd)
        if count>4 or memory>1024:raise ValueError('State-store capacity exceeded: at most 4 active runtimes and 1024 MiB of reserved guest allocations')
        return gate
    except BaseException:
        os.close(gate);raise

def supervise(path, fd, native, generation):
    listener = None
    child = None
    status = dict(state='starting', generation=generation)
    def terminate(number, frame):
        if child and child.poll() is None:
            child.terminate()
    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    try:
        config = read(path / 'config.json')
        command = config['command']
        executable = path / 'rootfs' / oci.path_name(command[0][1:])
        # The image and private state contain no links. Reject local tampering too.
        current = path / 'rootfs'
        for part in oci.path_name(command[0][1:]).split('/'):
            current /= part
            if current.is_symlink():
                raise ValueError('Executable symlink rejected')
        invocation = [native, 'run', str(executable), '--root', str(path / 'rootfs'),
                      '--argv0', command[0], '--max-steps', str(config.get('max_steps',10000000))]
        if config.get('cpu_backend')=='hvf':
            invocation[0]=str(Path(native).with_name('dockiva-hvf').resolve(strict=True))
            invocation+=['--cpu-backend','hvf','--elf-limit-mib','64','--mmap-limit-mib','64']
        if config.get('schedule', False):invocation.append('--schedule')
        if config.get('process_tree', False):invocation.append('--process-tree')
        if config.get('scheduled_network', False):invocation.append('--scheduled-network')
        if config.get('dynamic', False):invocation.append('--dynamic')
        invocation += ['--memory-limit-mib', str(config.get('memory_limit_mib', 256))]
        if config.get('job_control', False):invocation.append('--job-control')
        if config['writable']:
            invocation.append('--writable')
        for value in config['env']:
            invocation += ['--env', value]
        invocation += ['--', *command[1:]]
        token = secrets.token_hex(32)
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(('127.0.0.1', 0)); listener.listen(4); listener.settimeout(.1)
        save(path / 'control.json', dict(port=listener.getsockname()[1], token=token))
        with (path / 'stdout.log').open('ab') as out, (path / 'stderr.log').open('ab') as err:
            child = subprocess.Popen([sys.executable,str(ROOT/'confined_exec.py'),*(['--seatbelt'] if config.get('host_sandbox') else []),*invocation], stdout=out, stderr=err, stdin=subprocess.DEVNULL)
            status.update(state='running', pid=child.pid, started_at=time.time())
            save(path / 'status.json', status)
            while child.poll() is None:
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    continue
                with conn:
                    conn.settimeout(1)
                    try:
                        request = b''
                        while b'\n' not in request and len(request)<128:
                            chunk=conn.recv(128-len(request))
                            if not chunk:break
                            request+=chunk
                        if hmac.compare_digest(request, (token+' stop\n').encode()):
                            child.terminate()
                            try: child.wait(timeout=3)
                            except subprocess.TimeoutExpired: child.kill(); child.wait()
                            conn.sendall(b'OK')
                    except (OSError, socket.timeout):
                        pass
            code = child.wait()
            status.update(state='exited', exit_code=code if code >= 0 else 128-code, finished_at=time.time())
    except Exception as exc:
        status.update(state='failed', error=str(exc), exit_code=125)
    finally:
        if child and child.poll() is None:
            child.kill(); child.wait()
        if listener:
            listener.close()
        (path / 'control.json').unlink(missing_ok=True)
        save(path / 'status.json', status)
        os.close(fd)

def main():
    os.umask(0o077)
    if len(sys.argv) > 1 and sys.argv[1] == '_supervise':
        supervise(Path(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5])
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, default=Path.home() / '.vmfree/containers')
    parser.add_argument('--native', type=Path, default=ROOT / 'build/dockiva-runtime')
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('create'); p.add_argument('name'); p.add_argument('image', type=Path)
    p.add_argument('--env', action='append', default=[]);p.add_argument('--entrypoint', action='append');p.add_argument('--labels-json', default='{}');
    p.add_argument('--schedule', action='store_true')
    p.add_argument('--process-tree', action='store_true')
    p.add_argument('--job-control', action='store_true')
    p.add_argument('--host-sandbox', action='store_true')
    p.add_argument('--memory-limit-mib', type=int, default=256)
    p.add_argument('--dynamic', action='store_true')
    p.add_argument('--cpu-backend', choices=['interpreter','hvf'], default='interpreter')
    p.add_argument('--scheduled-network', action='store_true')
    p.add_argument('--max-steps', type=int, default=10000000)
    p.add_argument('--read-only', action='store_true'); p.add_argument('--ref')
    p.add_argument('--arg', action='append', help='Replace image Cmd; repeat for multiple arguments')
    for action in ('start', 'stop', 'inspect', 'logs', 'rm'):
        p = sub.add_parser(action); p.add_argument('name')
        if action == 'logs': p.add_argument('--stderr', action='store_true')
    sub.add_parser('list')
    a = parser.parse_args()
    store = a.state_dir.expanduser().absolute()
    if store.is_symlink(): raise ValueError('State directory symlink rejected')
    store.mkdir(mode=0o700, parents=True, exist_ok=True)
    if store.stat().st_uid != os.getuid() or store.stat().st_mode & 0o077:
        raise ValueError('State directory must be owned by you and have permissions 0700')
    if a.action == 'list':
        rows = []
        for p in sorted(store.iterdir()):
            if p.is_dir() and not p.is_symlink() and (p / 'status.json').exists():
                rows.append(dict(name=p.name, **read(p / 'status.json')))
        print(json.dumps(rows, indent=2)); return 0
    path = container(store, a.name)
    if a.action == 'create':
        if a.cpu_backend=='hvf' and (not a.dynamic or not a.process_tree or not a.scheduled_network or a.job_control):raise ValueError('Native execution requires dynamic mode, process trees and scheduled networking')
        if not 1<=a.memory_limit_mib<=1024:raise ValueError('Memory budget must be 1..1024 MiB')
        if not 0<=a.max_steps<=2**64-1:raise ValueError('Invalid instruction budget')
        image = oci.Image(a.image, a.ref)
        if a.entrypoint is not None:image.entry=oci.string_array(a.entrypoint,'Entrypoint')
        if a.env:
            merged=dict(e.split('=',1) for e in image.env)
            for e in a.env:
                if '=' not in e or not e.split('=',1)[0]:raise ValueError('Env requires KEY=VALUE')
                k,v=e.split('=',1);merged[k]=v
            image.env=[k+'='+v for k,v in merged.items()]
            oci.process_config(dict(os='linux',architecture='arm64',config=dict(Env=image.env)))
        labels=json.loads(a.labels_json)
        if not isinstance(labels,dict) or len(labels)>64 or not all(isinstance(k,str) and isinstance(v,str) and len(k)+len(v)<=4096 for k,v in labels.items()):raise ValueError('Invalid labels')
        command, _ = image.command(a.arg)
        path.mkdir(mode=0o700)  # Exclusive name ownership, no overwrites.
        try:
            (path / 'rootfs').mkdir(mode=0o700)
            image.extract(path / 'rootfs')
            save(path / 'config.json', dict(image=image.digest, command=command, env=image.env,
                                           writable=not a.read_only, cpu_backend=a.cpu_backend, schedule=a.schedule, process_tree=a.process_tree, scheduled_network=a.scheduled_network, dynamic=a.dynamic, job_control=a.job_control, memory_limit_mib=a.memory_limit_mib, host_sandbox=a.host_sandbox, max_steps=a.max_steps, labels=labels, created_at=time.time()))
            save(path / 'status.json', dict(state='created'))
        except BaseException:
            shutil.rmtree(path); raise
        print(a.name); return 0
    if not path.is_dir(): raise ValueError('Container does not exist')
    if a.action == 'inspect':
        print(json.dumps(dict(name=a.name, config=read(path / 'config.json'), status=read(path / 'status.json')), indent=2)); return 0
    if a.action == 'logs':
        log = path / ('stderr.log' if a.stderr else 'stdout.log')
        if log.exists():
            with log.open('rb') as stream: shutil.copyfileobj(stream, sys.stdout.buffer)
        return 0
    if a.action == 'stop':
        status = read(path / 'status.json')
        if status['state'] not in ('starting', 'running'): print(status['state']); return 0
        control=read(path/'control.json')
        with socket.create_connection(('127.0.0.1', control['port']), timeout=5) as conn:
            conn.sendall((control['token']+' stop\n').encode())
            if conn.recv(16) != b'OK': raise ValueError('Supervisor did not confirm stop')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status = read(path / 'status.json')
            if status['state'] not in ('starting', 'running'): print(status['state']); return 0
            time.sleep(.02)
        raise ValueError('Stop status timeout')
    fd = lock(path)
    try:
        previous = read(path / 'status.json')
        if previous['state'] in ('starting', 'running'):
            raise ValueError('Supervisor state is stale; inspect manually before recovery')
        if a.action == 'rm':
            shutil.rmtree(path); print(a.name); return 0
        native = str(a.native.resolve(strict=True))
        generation = uuid.uuid4().hex
        gate=reserve_capacity(store,path)
        try:
            with (path / 'supervisor.log').open('ab') as log:
                proc = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '_supervise',
                    str(path), str(fd), native, generation], pass_fds=(fd,), start_new_session=True,
                    stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        finally:os.close(gate)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            status = read(path / 'status.json')
            if status.get('generation') == generation:
                if status['state'] == 'failed': raise ValueError(status['error'])
                print(a.name); return 0
            if proc.poll() is not None: raise ValueError('Supervisor exited; check supervisor.log')
            time.sleep(.02)
        raise ValueError('Start status timeout; inspect before retrying')
    finally:
        os.close(fd)  # Child keeps the shared lock until it finishes.

if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError, oci.ImageError) as exc:
        print('container error: ' + str(exc), file=sys.stderr); sys.exit(125)
