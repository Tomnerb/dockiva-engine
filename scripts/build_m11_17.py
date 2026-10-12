#!/usr/bin/env python3
import argparse,hashlib,json,shutil,subprocess,sys,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from scripts.build_m6 import tar_bytes,write_layout
p=argparse.ArgumentParser();p.add_argument('--zig',default=shutil.which('zig'));a=p.parse_args()
if not a.zig:p.error('Provide --zig /path/to/zig (0.13.0)')
d=ROOT/'examples/m11_17';flags=['cc','-target','aarch64-linux-musl','-O0','-g0','-static','-fno-pie','-no-pie','-mgeneral-regs-only','-fno-sanitize=all','-fno-stack-protector','-ffreestanding','-fno-builtin','-nostdlib','-Wl,-e,_start']
command=[a.zig,*flags,str(ROOT/'examples/m5/start.S'),str(d/'server.c'),'-o',str(d/'server.elf')];subprocess.run(command,check=True)
subprocess.run([a.zig,*flags,str(ROOT/'examples/m5/start.S'),str(d/'net-check.c'),'-o',str(d/'net-check.elf')],check=True)
body=b'<!doctype html><title>VM-free</title><h1>Persistent VM-free HTTP server OK</h1>\n';(d/'index.html').write_bytes(body)
raw=tar_bytes([('app/server',(d/'server.elf').read_bytes(),tarfile.REGTYPE,0o755),('www/index.html',body,tarfile.REGTYPE,0o644)])
write_layout(d/'http-image',[raw],dict(Entrypoint=['/app/server'],Cmd=['18080'],Env=[]),compressed=True)
paths=[d/'net-check.c',d/'net-check.elf',d/'server.c',d/'server.elf',ROOT/'examples/m5/start.S',ROOT/'examples/m5/syscall.h']
(d/'build-manifest.json').write_text(json.dumps(dict(command=command,sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}),indent=2)+'\n')
print('Built persistent HTTP server and OCI image')
