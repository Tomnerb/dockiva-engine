#!/usr/bin/env python3
"""Assemble FEX's x86 bundle, through which Dockiva Engine runs linux/amd64 programs
(dockivad/x86.go): bin/FEX and bin/FEXServer, lib/ with the arm64 glibc they load,
and share/ with the packages' copyright files and where their sources are.

Everything comes from pinned Ubuntu 24.04 packages (FEX from the fex-emu PPA, built
for noble), each checked against its size and SHA-256 before it is read. FEX's is
kept in third_party/fex, as Launchpad expires a PPA's files once superseded; the
others are downloaded once into build/x86-cache. The bundle goes to build/x86,
beside build/dockiva-runtime, where the engine looks for it. Unpacking needs zstd:
Python 3.14+, or the zstd command.
"""
import argparse
import hashlib
import io
from pathlib import Path
import posixpath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LP = 'https://launchpad.net/ubuntu/+archive/primary/+files/'
LIB = 'usr/lib/aarch64-linux-gnu/'
VENDORED = ROOT/'third_party/fex'

# (file, url, size, sha256, source, {member: bundle path}). The libraries are the
# ones the ubuntu:24.04 arm64 image ships, which FEX 2610 was tested with. To move
# to a newer FEX: put its .deb in third_party/fex (and remove the old one), pin it
# here, and run the amd64 checks again.
PACKAGES = [
    ('fex-emu-armv8.4_2610-1~n_arm64.deb',
     'https://launchpad.net/~fex-emu/+archive/ubuntu/fex/+files/fex-emu-armv8.4_2610-1~n_arm64.deb',
     3375998, '61c48ea8a775ac24b90cb21d0a8c49daee52231895f49970adb4813c8078da3f',
     'https://github.com/FEX-Emu/FEX/tree/FEX-2610',
     {'usr/bin/FEX': 'bin/FEX', 'usr/bin/FEXServer': 'bin/FEXServer',
      'usr/share/doc/fex-emu-armv8.4/copyright': 'share/FEX-COPYRIGHT'}),
    ('libc6_2.39-0ubuntu8.9_arm64.deb', LP+'libc6_2.39-0ubuntu8.9_arm64.deb',
     2776414, 'ad109797c5f78a5eeb6d4c296bfbaae68b239111a0c9ac0eb97b8d430284ac45',
     'https://launchpad.net/ubuntu/+source/glibc/2.39-0ubuntu8.9',
     {LIB+'ld-linux-aarch64.so.1': 'lib/ld-linux-aarch64.so.1', LIB+'libc.so.6': 'lib/libc.so.6',
      LIB+'libm.so.6': 'lib/libm.so.6', 'usr/share/doc/libc6/copyright': 'share/GLIBC-COPYRIGHT'}),
    ('libgcc-s1_14.2.0-4ubuntu2~24.04.1_arm64.deb', LP+'libgcc-s1_14.2.0-4ubuntu2~24.04.1_arm64.deb',
     61884, '5b191f79ad985a9a653dbba45214619f729f9178a9bdf92f7c68db06c50d070b',
     'https://launchpad.net/ubuntu/+source/gcc-14/14.2.0-4ubuntu2~24.04.1',
     {LIB+'libgcc_s.so.1': 'lib/libgcc_s.so.1'}),
    ('libstdc++6_14.2.0-4ubuntu2~24.04.1_arm64.deb', LP+'libstdc++6_14.2.0-4ubuntu2~24.04.1_arm64.deb',
     750976, 'f84a05ac45a6884109b6527e911081a38263b89e042fc94a338cf975f61a330c',
     'https://launchpad.net/ubuntu/+source/gcc-14/14.2.0-4ubuntu2~24.04.1',
     {LIB+'libstdc++.so.6': 'lib/libstdc++.so.6'}),
    ('gcc-14-base_14.2.0-4ubuntu2~24.04.1_arm64.deb', LP+'gcc-14-base_14.2.0-4ubuntu2~24.04.1_arm64.deb',
     50984, 'bddbe21061fdc73a23e7de79ece91693bf61aa22d634d1c69c5048385c114968',
     'https://launchpad.net/ubuntu/+source/gcc-14/14.2.0-4ubuntu2~24.04.1',
     {'usr/share/doc/gcc-14-base/copyright': 'share/GCC-COPYRIGHT'}),  # libgcc-s1's and libstdc++6's
]


def fetch(name, url, size, sha256, cache):
    """The package's bytes: the repository's or cached copy, else downloaded; any must match."""
    path = cache/name
    for kept in (VENDORED/name, path):
        if kept.exists():
            data = kept.read_bytes()
            if len(data) == size and hashlib.sha256(data).hexdigest() == sha256:
                return data
    print('Downloading', url, file=sys.stderr)
    request = urllib.request.Request(url, headers={'User-Agent': 'dockiva-engine-build'})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = response.read(size+1)
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != size or digest != sha256:
        raise SystemExit(f'{name}: got {len(data)} bytes, SHA-256 {digest}; pinned {size} bytes, {sha256}')
    cache.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix('.partial')
    partial.write_bytes(data)
    partial.replace(path)
    return data


def deb_data(deb):
    """A .deb's data.tar.zst (an ar archive member), decompressed."""
    if not deb.startswith(b'!<arch>\n'):
        raise ValueError('not a .deb')
    pos = 8
    while pos+60 <= len(deb):
        name = deb[pos:pos+16].decode().strip().rstrip('/')
        size = int(deb[pos+48:pos+58])
        if name == 'data.tar.zst':
            return unzstd(deb[pos+60:pos+60+size])
        pos += 60+size+(size & 1)
    raise ValueError('no data.tar.zst in the .deb')


def unzstd(data):
    try:
        from compression import zstd  # Python 3.14+
        return zstd.decompress(data)
    except ImportError:
        pass
    tool = shutil.which('zstd')
    if not tool:
        raise SystemExit('Unpacking Ubuntu packages needs zstd: Python 3.14+, or the zstd command (brew install zstd)')
    return subprocess.run([tool, '-dcq'], input=data, stdout=subprocess.PIPE, check=True).stdout


def extract(data, members, out):
    """Writes each member (symlinks followed inside the package) to its bundle path."""
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        entries = {posixpath.normpath(m.name): m for m in tar.getmembers()}
        for member, target in members.items():
            entry = entries[member]
            for _ in range(8):
                if not entry.issym():
                    break
                entry = entries[posixpath.normpath(posixpath.join(posixpath.dirname(entry.name), entry.linkname))]
            if not entry.isfile():
                raise ValueError(member+' is not a file in its package')
            path = out/target
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(tar.extractfile(entry).read())
            path.chmod(0o755 if entry.mode & 0o111 else 0o644)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=ROOT/'build/x86')
    parser.add_argument('--cache', type=Path, default=ROOT/'build/x86-cache')
    args = parser.parse_args()
    output = args.output.absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.x86-', dir=output.parent) as temporary:
        stage = Path(temporary)/'x86'
        sources = ['FEX 2610 and the arm64 glibc it loads, from these Ubuntu 24.04 packages:', '']
        for name, url, size, sha256, source, members in PACKAGES:
            extract(deb_data(fetch(name, url, size, sha256, args.cache)), members, stage)
            sources += [name, '  from    '+url, '  sha256  '+sha256, '  source  '+source, '']
        (stage/'share/SOURCES').write_text('\n'.join(sources))
        if output.exists():
            shutil.rmtree(output)
        stage.rename(output)
    print(output)


if __name__ == '__main__':
    main()
