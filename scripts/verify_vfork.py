#!/usr/bin/env python3
"""Check shared vfork writes, parent suspension, exec detach and wait status."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import uuid

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--linux',action='store_true')
args=parser.parse_args()
linker=next((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
with tempfile.TemporaryDirectory(prefix='vmfree-vfork-') as temporary:
    root=Path(temporary);elf=root/'vfork.elf';obj=root/'probe.o'
    bad=root/'bad.elf';bad.write_bytes(b'not an ELF');bad.chmod(0o700)
    subprocess.run(['clang','--target=aarch64-linux-gnu','-c',str(ROOT/'examples/m35/vfork.S'),'-o',str(obj)],check=True)
    subprocess.run([str(linker),'-m','aarch64elf','-static','-e','_start',str(obj),'-o',str(elf)],check=True)
    check=root/'shared-check'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined',str(ROOT/'tests/native/m34_shared_image.c'),'-o',str(check)],check=True)
    subprocess.run([str(check)],check=True,timeout=10)
    for native in ['dockiva-runtime','dockiva-runtime-sanitize']:
        subprocess.run([str(ROOT/'build'/native),'run',str(elf),'--root',str(root),'--schedule','--max-steps','1000000'],check=True,timeout=10)
        print('PASS',native,'vfork shared writes, suspension, exec detach and wait')
    if args.linux:
        name='vmfree-vfork-'+uuid.uuid4().hex
        try:
            subprocess.run(['docker','run','--name',name,'--network=none','--read-only','--mount','type=bind,src='+str(elf)+',dst=/vfork.elf,readonly','--mount','type=bind,src='+str(bad)+',dst=/bad.elf,readonly','--entrypoint','/vfork.elf','alpine@sha256:4bcff63911fcb4448bd4fdacec207030997caf25e9bea4045fa6c8c44de311d1'],check=True,timeout=30)
            print('PASS Linux vfork reference')
        finally:
            subprocess.run(['docker','rm','--force',name],capture_output=True,timeout=15)
