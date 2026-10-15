#!/usr/bin/env python3
"""Validate Linux ARM64 sysinfo ABI; compare invariants, not host memory totals."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid

ROOT=Path(__file__).resolve().parents[1]
IMAGE='sha256:3b0f54bf27a77c98d319dc3a6706ed500a8985309f0b6c6407599b869f4a2bd9'


def decode(data):
    fields=struct.unpack('<q3Q6QH6x2QI4x',data)
    names=['uptime','load1','load5','load15','totalram','freeram','sharedram','bufferram','totalswap','freeswap','procs','totalhigh','freehigh','mem_unit']
    return dict(zip(names,fields))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--linux',action='store_true');a=p.parse_args()
    folder=ROOT/'build/redis-sysinfo';folder.mkdir(exist_ok=True);out=ROOT/'evidence/redis-sysinfo';out.mkdir(exist_ok=True)
    obj=folder/'probe.o';elf=folder/'probe.elf'
    linker=next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    subprocess.run(['clang','--target=aarch64-linux-gnu','-O0','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mgeneral-regs-only','-c',str(ROOT/'tests/native/sysinfo_guest.c'),'-o',str(obj)],check=True)
    subprocess.run([str(linker),'-m','aarch64elf','-static','-e','_start',str(obj),'-o',str(elf)],check=True)
    runs=[(backend,[str(ROOT/'build'/binary),'run',str(elf),'--dynamic','--cpu-backend',backend,*(['--process-tree','--scheduled-network'] if backend=='hvf' else []),'--memory-limit-mib','64']) for backend,binary in [('interpreter','dockiva-runtime'),('hvf','dockiva-hvf')]]
    name='vmfree-sysinfo-'+uuid.uuid4().hex
    if a.linux:runs.append(('Linux reference',['docker','run','--name',name,'--network=none','--read-only','--memory=128m','--pids-limit=16','--mount',f'type=bind,src={elf},dst=/probe,readonly','--entrypoint','/probe',IMAGE]))
    rows=[]
    try:
        for backend,cmd in runs:
            r=subprocess.run(cmd,capture_output=True,timeout=30);row=dict(backend=backend,exit=r.returncode,stderr=r.stderr.decode(errors='replace'),passed=False)
            if r.returncode==0 and len(r.stdout)==228:
                first,second=decode(r.stdout[:112]),decode(r.stdout[112:224]);errors=list(r.stdout[224:])
                ok=errors==[14]*4 and second['uptime']>=first['uptime']>=0
                for info in (first,second):ok=ok and info['mem_unit']>0 and info['procs']>0 and 0<=info['freeram']<=info['totalram'] and info['totalram']>0
                if backend!='Linux reference':
                    ok=ok and first['totalram']==second['totalram']==64*1024*1024 and first['mem_unit']==second['mem_unit']==1 and second['freeram']<first['freeram'] and first['procs']==second['procs']==1
                    ok=ok and first['uptime']<30 and all(first[k]==second[k]==0 for k in ['load1','load5','load15','sharedram','bufferram','totalswap','freeswap','totalhigh','freehigh'])
                row.update(first=first,second=second,pointer_errors=errors,passed=bool(ok))
            rows.append(row)
    finally:
        if a.linux:
            cleanup=subprocess.run(['docker','rm','--force',name],capture_output=True,timeout=15)
            if cleanup.returncode:raise RuntimeError('Reference cleanup failed: '+cleanup.stderr.decode())
    report=dict(scope='LP64 ABI, EFAULT and field invariants; values are guest-policy scoped, not Linux host totals',elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),results=rows)
    out.joinpath('abi.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    return 0 if all(r['passed'] for r in rows) else 1


if __name__=='__main__':raise SystemExit(main())
