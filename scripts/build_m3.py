#!/usr/bin/env python3
"""Build freestanding Linux ARM64 C and ISA fixtures using Zig/LLVM."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
NAMES=['flags','memory','readonly_fault','stack_exec_fault','startup','unmapped_fault','unaligned_sp','control']

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--zig',default=shutil.which('zig'));a=p.parse_args()
    if not a.zig:p.error('Provide --zig /absolute/path/to/zig (tested: 0.13.0)')
    zig=str(Path(a.zig).resolve());d=ROOT/'examples/m3'
    flags=['cc','-target','aarch64-linux-musl','-O0','-g0','-fno-sanitize=all','-fno-stack-protector','-ffreestanding','-fno-builtin','-fno-pie','-fno-vectorize','-fno-slp-vectorize','-nostdlib','-static','-no-pie','-Wl,--build-id=none','-Wl,-e,_start']
    jobs=[('demo',[],['start.S','demo.c'])]+[(name,[f'-DCASE={i}'],['checks.S']) for i,name in enumerate(NAMES,1)]
    manifest={'compiler':'Zig cc / LLVM','version':subprocess.check_output([zig,'version'],text=True).strip(),'flags':flags,'sources':{n:hashlib.sha256((d/n).read_bytes()).hexdigest() for n in ['start.S','demo.c','checks.S']},'files':[]}
    with tempfile.TemporaryDirectory() as tmp:
        for name,extra,sources in jobs:
            out=Path(tmp)/(name+'.elf')
            subprocess.run([zig,*flags,*extra,*[str(d/n) for n in sources],'-o',str(out)],check=True)
            out.chmod(0o755);shutil.copy2(out,d/out.name)
            manifest['files'].append({'name':out.name,'sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'extra_flags':extra,'sources':sources})
    (d/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Built 9 M3 Linux ELF files')
if __name__=='__main__':main()
