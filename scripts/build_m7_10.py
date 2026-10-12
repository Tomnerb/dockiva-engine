#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from scripts.build_m6 import tar_bytes,write_layout

def main():
    p=argparse.ArgumentParser();p.add_argument('--zig',default=shutil.which('zig'));a=p.parse_args()
    if not a.zig:p.error('Provide --zig /path/to/zig (tested 0.13.0)')
    zig=str(Path(a.zig).resolve());d=ROOT/'examples/m7_10'
    base=['cc','-target','aarch64-linux-musl','-O0','-g0','-static','-fno-pie','-no-pie','-mgeneral-regs-only']
    raw=['-fno-sanitize=all','-fno-stack-protector','-ffreestanding','-fno-builtin','-nostdlib','-Wl,-e,_start']
    commands=[]
    subprocess.run([zig,*[f for f in base if f!='-mgeneral-regs-only'],*raw,str(d/'isa.S'),'-o',str(d/'isa.elf')],check=True)
    for name in ('fs-check','server'):
        command=[zig,*base,*raw,str(ROOT/'examples/m5/start.S'),str(d/(name+'.c')),'-o',str(d/(name+'.elf'))]
        subprocess.run(command,check=True);commands.append(command)
    command=[zig,*base,str(ROOT/'examples/m9/libc-demo.c'),'-o',str(ROOT/'examples/m9/libc-demo.elf')]
    subprocess.run(command,check=True);commands.append(command)
    body=b'<!doctype html><title>VM-free</title><h1>Hello from the image filesystem!</h1>\n'
    response=b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n'+body
    (d/'index.http').write_bytes(response)
    (d/'index.html').write_bytes(body)
    for name,elf,cmd in [('http-image',d/'server.elf',[]),('counter-image',d/'fs-check.elf',['w']),('libc-image',ROOT/'examples/m9/libc-demo.elf',['cat','/hello.txt'])]:
        entries=[('app/main',elf.read_bytes(),tarfile.REGTYPE,0o755),('hello.txt',b'hello\n',tarfile.REGTYPE,0o644),('www/index.html',body,tarfile.REGTYPE,0o644)]
        write_layout(d/name,[tar_bytes(entries)],dict(Entrypoint=['/app/main'],Cmd=cmd,Env=[]),compressed=True)
    paths=[d/'isa.S',d/'isa.elf',d/'fs-check.c',d/'server.c',d/'fs-check.elf',d/'server.elf',ROOT/'examples/m9/libc-demo.c',ROOT/'examples/m9/libc-demo.elf',ROOT/'examples/m5/start.S',ROOT/'examples/m5/syscall.h']
    (d/'build-manifest.json').write_text(json.dumps(dict(compiler=subprocess.check_output([zig,'version'],text=True).strip(),commands=commands,sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}),indent=2)+'\n')
    print('Built M7–M10 fixtures and three OCI images')
if __name__=='__main__':main()
