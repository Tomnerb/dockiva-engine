#!/usr/bin/env python3
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
NAMES=['demo','brk_regrow','partial_unmap','map_reuse','zero_length','invalid_unmap','readonly_zero','prot_none','unmap_hole','brk_below_base','readonly_fault','after_unmap_fault','none_fault','map_limit','brk_limit','unsupported_flags','after_shrink_fault']
def main():
    p=argparse.ArgumentParser();p.add_argument('--zig',default=shutil.which('zig'));a=p.parse_args()
    if not a.zig:p.error('Provide --zig /path/to/zig (tested 0.13.0)')
    zig=str(Path(a.zig).resolve());d=ROOT/'examples/m4'
    flags=['cc','-target','aarch64-linux-musl','-O0','-g0','-fno-sanitize=all','-fno-stack-protector','-ffreestanding','-fno-builtin','-fno-pie','-mgeneral-regs-only','-fno-vectorize','-fno-slp-vectorize','-nostdlib','-static','-no-pie','-Wl,--build-id=none','-Wl,-e,_start']
    sources=['start.S','heap.c'];manifest=dict(compiler='Zig cc / LLVM',version=subprocess.check_output([zig,'version'],text=True).strip(),flags=flags,sources={n:hashlib.sha256((d/n).read_bytes()).hexdigest() for n in sources},files=[])
    with tempfile.TemporaryDirectory() as tmp:
        for case,name in enumerate(NAMES):
            out=Path(tmp)/(name+'.elf');subprocess.run([zig,*flags,f'-DCASE={case}',*[str(d/n) for n in sources],'-o',str(out)],check=True)
            out.chmod(0o755);shutil.copy2(out,d/out.name)
            manifest['files'].append(dict(name=out.name,case=case,sha256=hashlib.sha256(out.read_bytes()).hexdigest()))
    (d/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Built 17 M4 fixtures')
if __name__=='__main__':main()
