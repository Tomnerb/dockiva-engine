#!/usr/bin/env python3
"""Compare bounded Linux ARM64 map_stack behavior on both CPU backends and Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid

ROOT=Path(__file__).resolve().parents[1]
IMAGE='sha256:3b0f54bf27a77c98d319dc3a6706ed500a8985309f0b6c6407599b869f4a2bd9'


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--linux',action='store_true');a=p.parse_args()
    folder=ROOT/'build/redis-map_stack';folder.mkdir(exist_ok=True);out=ROOT/'evidence/redis-map_stack';out.mkdir(exist_ok=True)
    obj=folder/'probe.o';elf=folder/'probe.elf'
    linker=next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    subprocess.run(['clang','--target=aarch64-linux-gnu','-O0','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mgeneral-regs-only','-c',str(ROOT/'tests/native/map_stack_guest.c'),'-o',str(obj)],check=True)
    subprocess.run([str(linker),'-m','aarch64elf','-static','-e','_start',str(obj),'-o',str(elf)],check=True)
    runs=[(backend,[str(ROOT/'build'/binary),'run',str(elf),'--dynamic','--cpu-backend',backend,*(['--process-tree','--scheduled-network'] if backend=='hvf' else []),'--memory-limit-mib','64']) for backend,binary in [('interpreter','vmfree-c'),('hvf','vmfree-c-hvf')]]
    name='vmfree-map_stack-'+uuid.uuid4().hex
    if a.linux:runs.append(('Linux reference',['docker','run','--name',name,'--network=none','--read-only','--memory=128m','--pids-limit=16','--mount',f'type=bind,src={elf},dst=/probe,readonly','--entrypoint','/probe',IMAGE]))
    rows=[]
    try:
        for backend,cmd in runs:
            r=subprocess.run(cmd,capture_output=True,timeout=30);row=dict(backend=backend,exit=r.returncode,stderr=r.stderr.decode(errors='replace'),passed=False)
            if len(r.stdout)==4:row.update(checks=struct.unpack('<I',r.stdout)[0],passed=r.returncode==0 and struct.unpack('<I',r.stdout)[0]==12)
            rows.append(row)
    finally:
        if a.linux:
            cleanup=subprocess.run(['docker','rm','--force',name],capture_output=True,timeout=15)
            if cleanup.returncode:raise RuntimeError('Reference cleanup failed: '+cleanup.stderr.decode())
    report=dict(scope='MAP_STACK allocation, zero-fill, guard access, mprotect, data retention, unmap and zero-length validation',elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),results=rows)
    out.joinpath('abi.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    return 0 if all(r['passed'] for r in rows) else 1


if __name__=='__main__':raise SystemExit(main())
