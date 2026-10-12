#!/usr/bin/env python3
"""Compare memory-advice semantics on both runtime engines and optionally Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid

ROOT=Path(__file__).resolve().parents[1]
NAMES=['normal','random','sequential','contents_preserved','unknown_advice','unaligned',
       'empty_unmapped','overflow','unmapped','prot_none','hole','free_unsupported','int_abi']
EXPECTED=[0,0,0,0,22,22,0,22,12,0,12,22,0]
IMAGE='sha256:3b0f54bf27a77c98d319dc3a6706ed500a8985309f0b6c6407599b869f4a2bd9'


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--linux',action='store_true');a=p.parse_args()
    folder=ROOT/'build/redis-cow';folder.mkdir(exist_ok=True)
    obj=folder/'madvise.o';elf=folder/'madvise.elf'
    linker=next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    subprocess.run(['clang','--target=aarch64-linux-gnu','-O0','-ffreestanding','-fno-builtin',
                    '-fno-stack-protector','-fno-pic','-mgeneral-regs-only','-c',str(ROOT/'tests/native/madvise_guest.c'),'-o',str(obj)],check=True)
    subprocess.run([str(linker),'-m','aarch64elf','-static','-e','_start',str(obj),'-o',str(elf)],check=True)
    rows=[]
    for backend,binary in [('interpreter','vmfree-c'),('hvf','vmfree-c-hvf')]:
        r=subprocess.run([str(ROOT/'build'/binary),'run',str(elf),'--dynamic','--cpu-backend',backend,*(['--process-tree','--scheduled-network'] if backend=='hvf' else [])],capture_output=True,timeout=10)
        rows.append(dict(backend=backend,exit=r.returncode,results=list(r.stdout),stderr=r.stderr.decode(errors='replace'),passed=r.returncode==0 and list(r.stdout)==EXPECTED))
    if a.linux:
        name='vmfree-madvise-'+uuid.uuid4().hex
        try:
            r=subprocess.run(['docker','run','--name',name,'--network=none','--read-only','--memory=128m','--pids-limit=16',
                '--mount',f'type=bind,src={elf},dst=/probe,readonly','--entrypoint','/probe',IMAGE],capture_output=True,timeout=30)
            linux_expected=EXPECTED.copy();linux_expected[11]=0
            rows.append(dict(backend='Linux reference',exit=r.returncode,results=list(r.stdout),stderr=r.stderr.decode(errors='replace'),passed=r.returncode==0 and list(r.stdout)==linux_expected))
        finally:
            cleanup=subprocess.run(['docker','rm','--force',name],capture_output=True,timeout=15)
            if cleanup.returncode:raise RuntimeError('Reference container cleanup failed: '+cleanup.stderr.decode())
    report=dict(cases=NAMES,expected_runtime=EXPECTED,declared_difference='MADV_FREE unsupported: runtime EINVAL; Linux reference supports it',elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),results=rows)
    out=ROOT/'evidence/redis-cow/madvise.json';out.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    return 0 if all(r['passed'] for r in rows) else 1


if __name__=='__main__':raise SystemExit(main())
