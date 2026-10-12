#!/usr/bin/env python3
"""Build M23 freestanding ARM64 guests with Clang and LLVM's ELF linker."""
import argparse, hashlib, json, shutil, subprocess, sys, tarfile, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from scripts.build_m6 import tar_bytes,write_layout

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc',default=shutil.which('clang'))
    candidates=list((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
    p.add_argument('--ld',default=shutil.which('ld.lld') or (str(candidates[0]) if candidates else None))
    p.add_argument('--package-only',action='store_true');a=p.parse_args();d=ROOT/'examples/m23'
    if not a.package_only:
        if not a.cc or not a.ld:p.error('Clang and ld.lld required; provide --cc and --ld')
        with tempfile.TemporaryDirectory() as tmp:
            for name in ['shell','probe']:
                objects=[]
                for source in ['start.S',name+'.c']:
                    out=Path(tmp)/(source+'.o');objects.append(str(out))
                    subprocess.run([a.cc,'--target=aarch64-linux-gnu','-O0','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mgeneral-regs-only','-c',str(d/source),'-o',str(out)],check=True)
                subprocess.run([a.ld,'-m','aarch64elf','-static','-e','_start',*objects,'-o',str(d/(name+'.elf'))],check=True)
    entries=[('bin/sh',(d/'shell.elf').read_bytes(),tarfile.REGTYPE,0o755),('bin/probe',(d/'probe.elf').read_bytes(),tarfile.REGTYPE,0o755),('bin/busybox',(ROOT/'examples/m18/busybox.elf').read_bytes(),tarfile.REGTYPE,0o755),('hello.txt',b'Hello from M23!\n',tarfile.REGTYPE,0o644)]
    write_layout(d/'shell-image',[tar_bytes(entries)],dict(Entrypoint=['/bin/sh'],Cmd=['-c','busybox echo M23; true'],Env=[]),compressed=True)
    write_layout(d/'signal-image',[tar_bytes(entries)],dict(Entrypoint=['/bin/probe'],Cmd=['cancel-read'],Env=[]),compressed=True)
    files=[x for x in d.iterdir() if x.suffix in ('.c','.h','.S','.elf')]+[ROOT/'examples/m18/busybox.elf']
    (d/'build-manifest.json').write_text(json.dumps({'compiler':a.cc,'linker':a.ld,'sha256':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in files}},indent=2)+'\n')
    print('Built M23 guests and OCI image')
if __name__=='__main__':main()
