#!/usr/bin/env python3
"""Independent acquire/release instruction fixture and optional Linux reference."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import uuid
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--linux',action='store_true');a=p.parse_args()
linker=next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
with tempfile.TemporaryDirectory() as tmp:
    obj=Path(tmp)/'probe.o';elf=ROOT/'examples/m35/acquire_release.elf'
    subprocess.run(['clang','--target=aarch64-linux-gnu','-c',str(ROOT/'examples/m35/acquire_release.S'),'-o',str(obj)],check=True)
    subprocess.run([str(linker),'-m','aarch64elf','-static','-e','_start',str(obj),'-o',str(elf)],check=True)
    for native in ['dockiva-runtime','dockiva-runtime-sanitize']:
        for options in [[],['--no-decode-cache'],['--no-lookup-cache']]:
            subprocess.run([str(ROOT/'build'/native),'run',str(elf),*options],check=True,timeout=10)
            print('PASS',native,options)
    if a.linux:
        name='vmfree-m35-'+uuid.uuid4().hex
        try:
            subprocess.run(['docker','run','--name',name,'--network=none','--read-only','--mount','type=bind,src='+str(elf)+',dst=/probe,readonly','--entrypoint','/probe','alpine@sha256:4bcff63911fcb4448bd4fdacec207030997caf25e9bea4045fa6c8c44de311d1'],check=True,timeout=30)
            print('PASS Linux reference')
        finally:subprocess.run(['docker','rm','--force',name],capture_output=True,timeout=15)
