#!/usr/bin/env python3
"""Build executable and shared-library probes against the pinned musl DSO."""
import hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];f=ROOT/'examples/m28';root=f/'root'
candidates=list((Path.home()/'.rustup/toolchains').glob('*/lib/rustlib/*/bin/gcc-ld/ld.lld'))
ld=shutil.which('ld.lld') or str(candidates[0])
with tempfile.TemporaryDirectory() as temp:
    for source in ['library.c','dynamic.c','start.S']:
        subprocess.run(['clang','--target=aarch64-linux-gnu','-O0','-ffreestanding','-fno-builtin','-fno-stack-protector','-fPIC',*(['-mgeneral-regs-only'] if source.endswith('.c') else []),'-c',str(f/source),'-o',str(Path(temp)/(source+'.o'))],check=True)
    subprocess.run([ld,'-z','max-page-size=4096','-z','separate-code','-shared','-soname','libprobe.so',str(Path(temp)/'library.c.o'),'-o',str(root/'lib/libprobe.so')],check=True)
    subprocess.run([ld,'-z','max-page-size=4096','-z','separate-code','-pie','--dynamic-linker','/lib/ld-musl-aarch64.so.1','-e','_start',str(Path(temp)/'start.S.o'),str(Path(temp)/'dynamic.c.o'),'-L'+str(root/'lib'),'-lprobe','-l:libc.musl-aarch64.so.1','-o',str(root/'bin/dynamic-probe')],check=True)
(f/'probe-manifest.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [f/'library.c',f/'dynamic.c',f/'start.S',root/'lib/libprobe.so',root/'bin/dynamic-probe']},indent=2)+'\n')
