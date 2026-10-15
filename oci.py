#!/usr/bin/env python3
"""M6 local OCI image packaging adapter. Not an OCI runtime or filesystem sandbox."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent
MANIFEST = 'application/vnd.oci.image.manifest.v1+json'
CONFIG = 'application/vnd.oci.image.config.v1+json'
TAR = 'application/vnd.oci.image.layer.v1.tar'
MAX_BLOB = 32 * 1024 * 1024
MAX_JSON = 1024 * 1024
MAX_EXPANDED = 64 * 1024 * 1024
MAX_MEMBERS = 4096
DIGEST = re.compile(r'sha256:[0-9a-f]{64}\Z')

class ImageError(ValueError):
    pass

def require(test, message):
    if not test:
        raise ImageError(message)

def sha(data):
    return 'sha256:' + hashlib.sha256(data).hexdigest()

def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result

def json_object(data):
    require(len(data) <= MAX_JSON, 'JSON exceeds 1 MiB')
    def invalid_constant(value):
        raise ImageError('Invalid JSON constant: ' + value)
    value = json.loads(data, object_pairs_hook=unique_object, parse_constant=invalid_constant)
    require(isinstance(value, dict), 'Expected JSON object')
    return value

def local_bytes(root, relative, limit):
    # Local layouts must not be modified concurrently during loading.
    current = root
    for component in relative.split('/'):
        current = current / component
        require(not current.is_symlink(), 'Symlink in OCI layout')
    fd = os.open(current, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= limit, 'Not a bounded regular file')
        data = stream.read(limit + 1)
    require(len(data) <= limit, 'File exceeds limit')
    return data

def blob(root, desc, media, limit=MAX_BLOB):
    require(isinstance(desc, dict), 'Invalid descriptor')
    require(desc.get('mediaType') in media, 'Unsupported descriptor media type')
    digest, size = desc.get('digest'), desc.get('size')
    require(isinstance(digest, str) and DIGEST.fullmatch(digest), 'Only sha256 descriptors supported')
    require(type(size) is int and 0 <= size <= limit, 'Invalid descriptor size')
    require(not desc.get('urls') and 'data' not in desc, 'External/embedded blobs unsupported')
    data = local_bytes(root, 'blobs/sha256/' + digest[7:], limit)
    require(len(data) == size and sha(data) == digest, 'Blob size/digest mismatch')
    return data

def path_name(name):
    require(isinstance(name, str) and len(name) <= 1024, 'Invalid layer path')
    require(not name.startswith('/') and '\\' not in name and ':' not in name, 'Absolute/ambiguous layer path')
    require(all(32 <= ord(c) < 127 for c in name), 'Only printable ASCII layer paths supported')
    parts = name.split('/')
    require('..' not in parts, 'Path traversal rejected')
    parts = [p for p in parts if p not in ('', '.')]
    require(len(parts) <= 32, 'Path too deep')
    return '/'.join(parts)

def string_array(value, field):
    if value is None:
        return []
    require(isinstance(value, list) and len(value) <= 64, 'Invalid ' + field)
    require(all(isinstance(v, str) and '\x00' not in v for v in value), 'Invalid strings in ' + field)
    return value

def process_config(config):
    require(config.get('os') == 'linux' and config.get('architecture') == 'arm64', 'Only linux/arm64 images supported')
    settings = config.get('config', {})
    require(isinstance(settings, dict), 'Invalid process config')
    require(settings.get('WorkingDir', '') in ('', '/'), 'Only WorkingDir / supported')
    require(settings.get('User', '') == '', 'User identity settings unsupported')
    require(not settings.get('Volumes'), 'Volumes unsupported')
    require(settings.get('StopSignal', '') in ('', 'SIGTERM'), 'Custom StopSignal unsupported')
    entry = string_array(settings.get('Entrypoint'), 'Entrypoint')
    args = string_array(settings.get('Cmd'), 'Cmd')
    env = string_array(settings.get('Env'), 'Env')
    require(all('=' in v and v.split('=', 1)[0] for v in env), 'Env requires KEY=VALUE')
    require(len({v.split('=', 1)[0] for v in env}) == len(env), 'Duplicate environment keys unsupported')
    return entry, args, env

def remove_tree(tree, name):
    for key in list(tree):
        if key == name or key.startswith(name + '/') or not name:
            del tree[key]

def parent_dirs(tree, name):
    parts = name.split('/')
    for i in range(1, len(parts)):
        parent = '/'.join(parts[:i])
        require(parent not in tree or tree[parent][0] == 'dir', 'File used as directory')
        tree.setdefault(parent, ('dir', b'', False))

def layer_apply(tree, raw, budget):
    entries = []
    names = set()
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
        for member in archive:
            budget[0] += 1
            require(budget[0] <= MAX_MEMBERS, 'Too many tar members')
            name = path_name(member.name)
            if not name:
                require(member.isdir(), 'Empty file path')
                continue
            require(name not in names, 'Duplicate path within a layer')
            names.add(name)
            require(member.isfile() or member.isdir(), 'Links/devices/FIFOs unsupported')
            require(member.sparse is None and not any(k.startswith('GNU.sparse') for k in member.pax_headers), 'Sparse files unsupported')
            require(0 <= member.size <= MAX_BLOB, 'Layer file exceeds 32 MiB')
            if member.isdir():
                require(member.size == 0, 'Directory has file content')
                data = b''
            else:
                source = archive.extractfile(member)
                require(source is not None, 'Missing tar content')
                data = source.read(member.size + 1)
                require(len(data) == member.size, 'Truncated tar content')
            entries.append((name, 'dir' if member.isdir() else 'file', data, bool(member.mode & 0o111)))
    # OCI whiteouts remove only lower-layer content, independent of tar ordering.
    for name, kind, data, executable in entries:
        leaf = name.rsplit('/', 1)[-1]
        if not leaf.startswith('.wh.'):
            continue
        require(kind == 'file' and not data, 'Whiteouts must be empty regular files')
        parent = name.rpartition('/')[0]
        if leaf == '.wh..wh..opq':
            require(parent not in tree or tree[parent][0] == 'dir', 'Opaque marker parent is not a directory')
            for key in list(tree):
                if (not parent) or key.startswith(parent + '/'):
                    del tree[key]
        else:
            target = leaf[4:]
            require(target not in ('', '.', '..') and not target.startswith('.wh.'), 'Invalid whiteout target')
            remove_tree(tree, parent + '/' + target if parent else target)
    for name, kind, data, executable in sorted(entries, key=lambda item: (item[1] != 'dir', item[0].count('/'))):
        if name.rsplit('/', 1)[-1].startswith('.wh.'):
            continue
        parent_dirs(tree, name)
        if kind == 'file' or (name in tree and tree[name][0] != kind):
            remove_tree(tree, name)
        tree[name] = (kind, data, executable)
    require(len(tree) <= MAX_MEMBERS, 'Too many extracted paths')
    require(len({name.lower() for name in tree}) == len(tree), 'Case-colliding paths unsupported on Mac')

class Image:
    def __init__(self, layout, ref=None):
        root = Path(layout).resolve(strict=True)
        require(root.is_dir(), 'Expected local OCI layout directory')
        header = json_object(local_bytes(root, 'oci-layout', MAX_JSON))
        require(header.get('imageLayoutVersion') == '1.0.0', 'Unsupported OCI layout version')
        index = json_object(local_bytes(root, 'index.json', MAX_JSON))
        manifests = index.get('manifests')
        require(index.get('schemaVersion') == 2 and isinstance(manifests, list) and 1 <= len(manifests) <= 32, 'Invalid OCI index')
        if ref is not None:
            manifests = [d for d in manifests if isinstance(d, dict) and isinstance(d.get('annotations', {}), dict) and d.get('annotations', {}).get('org.opencontainers.image.ref.name') == ref]
        require(len(manifests) == 1, 'Select exactly one image with --ref')
        desc = manifests[0]
        manifest = json_object(blob(root, desc, {MANIFEST}, MAX_JSON))
        require(manifest.get('schemaVersion') == 2 and manifest.get('mediaType') == MANIFEST, 'Invalid image manifest')
        self.digest = desc['digest']
        platform = desc.get('platform')
        if platform is not None:
            require(isinstance(platform, dict) and platform.get('os') == 'linux' and platform.get('architecture') == 'arm64', 'Descriptor platform is not linux/arm64')
        config = json_object(blob(root, manifest.get('config'), {CONFIG}, MAX_JSON))
        self.entry, self.args, self.env = process_config(config)
        layers = manifest.get('layers')
        require(isinstance(layers, list) and 0 < len(layers) <= 32, 'Expected 1..32 layers')
        rootfs = config.get('rootfs', {})
        require(isinstance(rootfs, dict) and rootfs.get('type') == 'layers', 'Invalid rootfs config')
        diffs = rootfs.get('diff_ids')
        require(isinstance(diffs, list) and len(diffs) == len(layers), 'Layer/DiffID count mismatch')
        self.tree = {}
        total, budget = 0, [0]
        for desc, diff in zip(layers, diffs):
            packed = blob(root, desc, {TAR, TAR + '+gzip'})
            if desc['mediaType'] == TAR + '+gzip':
                with gzip.GzipFile(fileobj=io.BytesIO(packed)) as stream:
                    raw = stream.read(MAX_EXPANDED - total + 1)
            else:
                raw = packed
            total += len(raw)
            require(total <= MAX_EXPANDED, 'Expanded layers exceed 64 MiB')
            require(isinstance(diff, str) and DIGEST.fullmatch(diff) and sha(raw) == diff, 'Uncompressed layer DiffID mismatch')
            layer_apply(self.tree, raw, budget)
        self.layers = len(layers)
        self.command()  # Fail early on incompatible entrypoint or arguments.

    def command(self, override=None):
        command = self.entry + (self.args if override is None else override)
        require(command and len(command) <= 64, 'Expected executable and at most 63 arguments')
        require(all(isinstance(v, str) and '\x00' not in v for v in command), 'Invalid argument strings')
        require(command[0].startswith('/'), 'Entrypoint must be an absolute guest path; PATH lookup unsupported')
        path = path_name(command[0][1:])
        require(path in self.tree and self.tree[path][0] == 'file' and self.tree[path][2], 'Entrypoint missing or not executable')
        require(sum(len(v.encode()) + 1 for v in command + self.env) <= 65536, 'Process strings exceed 64 KiB')
        require(self.tree[path][1][:7] == b'\x7fELF\x02\x01\x01', 'Entrypoint must be a little-endian ELF64 file')
        return command, path

    def extract(self, destination):
        # Destination must be a fresh private directory owned by this invocation.
        root = Path(destination)
        require(root.is_dir() and not root.is_symlink() and not any(root.iterdir()), 'Extraction requires empty private directory')
        for name, (kind, data, executable) in sorted(self.tree.items(), key=lambda item: (item[1][0] != 'dir', item[0].count('/'))):
            path = root.joinpath(*name.split('/'))
            if kind == 'dir':
                path.mkdir(mode=0o700, parents=True, exist_ok=True)
            else:
                path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                with path.open('xb') as stream:
                    stream.write(data)
                path.chmod(0o700 if executable else 0o600)

    def run(self, native, override=None, writable=False, schedule=False, max_steps=None, interactive=False, process_tree=False, job_control=False, scheduled_network=False, dynamic=False, memory_limit_mib=256, host_sandbox=False):
        interactive = interactive or job_control
        command, path = self.command(override)
        native = Path(native).resolve(strict=True)
        with tempfile.TemporaryDirectory(prefix='vmfree-oci-') as tmp:
            self.extract(tmp)
            invocation = [str(native), 'run', str(Path(tmp) / path), '--argv0', command[0], '--root', tmp]
            if schedule:invocation.append('--schedule')
            if interactive:invocation += ['--interactive', '--forwarded-interrupts']
            if process_tree:invocation.append('--process-tree')
            if job_control:invocation.append('--job-control')
            if scheduled_network:invocation.append('--scheduled-network')
            if dynamic:invocation.append('--dynamic')
            invocation += ['--memory-limit-mib', str(memory_limit_mib)]
            if max_steps is not None:invocation += ['--max-steps',str(max_steps)]
            if writable:
                invocation.append('--writable')
            for value in self.env:
                invocation += ['--env', value]
            invocation += ['--', *command[1:]]
            if host_sandbox:invocation=[sys.executable,str(Path(__file__).resolve().with_name('confined_exec.py')),'--seatbelt',*invocation]
            child = subprocess.Popen(invocation)
            previous = {}
            def forward(number, frame):
                if child.poll() is None:
                    child.send_signal(signal.SIGUSR2 if interactive and number == signal.SIGINT else
                                      signal.SIGURG if job_control and number == signal.SIGTSTP else number)
            try:
                for number in ((signal.SIGTERM, signal.SIGINT, signal.SIGTSTP) if job_control else (signal.SIGTERM, signal.SIGINT)):
                    previous[number] = signal.signal(number, forward)
                result = child.wait()
                return result if result >= 0 else 128 - result
            finally:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        child.kill(); child.wait()
                for number, handler in previous.items():
                    signal.signal(number, handler)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['inspect', 'run'])
    parser.add_argument('layout', type=Path)
    parser.add_argument('--ref')
    parser.add_argument('--host-sandbox', action='store_true', help='Experimental macOS client-only host confinement')
    parser.add_argument('--memory-limit-mib', type=int, default=256)
    parser.add_argument('--dynamic', action='store_true', help='Enable bounded musl dynamic loading')
    parser.add_argument('--scheduled-network', action='store_true', help='Enable scheduler-aware loopback sockets')
    parser.add_argument('--job-control', action='store_true', help='Enable experimental guest signal handlers and terminal job control')
    parser.add_argument('--process-tree', action='store_true', help='Enable bounded nested guest processes and scheduling')
    parser.add_argument('--interactive', action='store_true', help='Forward foreground stdin and interrupts to scheduled guests')
    parser.add_argument('--schedule', action='store_true', help='Enable cooperative guest process scheduling')
    parser.add_argument('--max-steps', type=int, help='Shared guest instruction budget')
    parser.add_argument('--writable', action='store_true', help='Allow writes in the temporary guest root (discarded after exit)')
    parser.add_argument('--native', type=Path, default=ROOT / 'build/dockiva-runtime')
    # Everything after -- replaces image Cmd without invoking a shell.
    raw = sys.argv[1:]
    override = None
    if '--' in raw:
        at = raw.index('--'); override, raw = raw[at+1:], raw[:at]
    args = parser.parse_args(raw)
    try:
        img = Image(args.layout, args.ref)
        if args.action == 'inspect':
            command, _ = img.command(override)
            print(json.dumps(dict(manifest=img.digest, platform='linux/arm64', layers=img.layers,
                paths=len(img.tree), command=command, env_keys=[e.split('=', 1)[0] for e in img.env],
                rootfs_isolation=False), indent=2))
            return 0
        return img.run(args.native, override, writable=args.writable, schedule=args.schedule, max_steps=args.max_steps, interactive=args.interactive, process_tree=args.process_tree, job_control=args.job_control, scheduled_network=args.scheduled_network, dynamic=args.dynamic, memory_limit_mib=args.memory_limit_mib, host_sandbox=args.host_sandbox)
    except (ImageError, OSError, ValueError, tarfile.TarError, EOFError, RecursionError, zlib.error) as exc:
        print('OCI error: ' + str(exc), file=sys.stderr)
        return 125

if __name__ == '__main__':
    sys.exit(main())
