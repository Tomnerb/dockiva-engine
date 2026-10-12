#!/usr/bin/env python3
"""Build deterministic local OCI layouts from the bundled compiler-built ELFs."""
import gzip
import io
import json
from pathlib import Path
import sys
import tarfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import oci

def tar_bytes(entries):
    """entries: (path, bytes, tar type, mode), also useful for hostile fixtures."""
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        for name, data, kind, mode in entries:
            member = tarfile.TarInfo(name)
            member.type, member.mode = kind, mode
            member.size = len(data) if kind == tarfile.REGTYPE else 0
            if kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
                member.linkname = '/tmp/outside'
            archive.addfile(member, io.BytesIO(data) if member.isfile() else None)
    return output.getvalue()

def write_layout(destination, raws, settings=None, compressed=False, diffs=None, architecture='arm64'):
    destination = Path(destination)
    blobs = destination / 'blobs/sha256'
    blobs.mkdir(parents=True, exist_ok=True)
    def put(data, media):
        digest = oci.sha(data)
        (blobs / digest[7:]).write_bytes(data)
        return dict(mediaType=media, digest=digest, size=len(data))
    def encode(value):
        return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()
    layers = [put(gzip.compress(raw, mtime=0) if compressed else raw,
                  oci.TAR + ('+gzip' if compressed else '')) for raw in raws]
    config = dict(architecture=architecture, os='linux',
                  config=settings or dict(Entrypoint=['/app/server'], Cmd=[], Env=[]),
                  rootfs=dict(type='layers', diff_ids=diffs if diffs is not None else [oci.sha(r) for r in raws]))
    manifest = dict(schemaVersion=2, mediaType=oci.MANIFEST,
                    config=put(encode(config), oci.CONFIG), layers=layers)
    desc = put(encode(manifest), oci.MANIFEST)
    desc['annotations'] = {'org.opencontainers.image.ref.name': 'demo'}
    desc['platform'] = dict(os='linux', architecture=architecture)
    (destination / 'index.json').write_bytes(encode(dict(schemaVersion=2, manifests=[desc])))
    (destination / 'oci-layout').write_bytes(encode(dict(imageLayoutVersion='1.0.0')))
    return manifest

def main():
    for name, source, executable, cmd in [('http-image', 'm5/server.elf', 'server', []),
                                          ('args-image', 'm3/demo.elf', 'demo', ['12345'])]:
        raw = tar_bytes([('app/' + executable, (ROOT / 'examples' / source).read_bytes(), tarfile.REGTYPE, 0o755)])
        write_layout(ROOT / 'examples/m6' / name, [raw],
                     dict(Entrypoint=['/app/' + executable], Cmd=cmd, Env=['MODE=oci'], WorkingDir='/'), compressed=True)
        print('Built ' + name)
if __name__ == '__main__':
    main()
