#!/usr/bin/env python3
"""Package a bounded HTTP artifact-fetching workload from pinned Alpine binaries."""
import hashlib,json,sys,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from scripts.build_m6 import tar_bytes,write_layout
f=ROOT/'examples/m29';f.mkdir(exist_ok=True);r=ROOT/'examples/m28/root'
paths=['bin/busybox','lib/ld-musl-aarch64.so.1','lib/libc.musl-aarch64.so.1']
entries=[(p,(r/p).read_bytes(),tarfile.REGTYPE,0o755) for p in paths]
write_layout(f/'fetch-image',[tar_bytes(entries)],dict(Entrypoint=['/bin/busybox','wget'],Cmd=['--help'],Env=[]),compressed=True)
(f/'build-manifest.json').write_text(json.dumps(dict(upstream=json.loads((ROOT/'examples/m28/build-manifest.json').read_text())['image'],scope='Unmodified BusyBox wget and musl, repackaged as a loopback HTTP artifact-fetching image; not the full Alpine image',sha256={p:hashlib.sha256((r/p).read_bytes()).hexdigest() for p in paths}),indent=2)+'\n')
