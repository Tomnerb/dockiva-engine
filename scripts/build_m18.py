#!/usr/bin/env python3
"""Build unmodified BusyBox 1.37.0 on Linux with Zig 0.13.0, or package fixtures."""
import argparse,hashlib,json,os,shutil,subprocess,sys,tarfile,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from scripts.build_m6 import tar_bytes,write_layout
SHA='3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4'
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--zig');p.add_argument('--package-only',action='store_true');a=p.parse_args()
 d=ROOT/'examples/m18';source=ROOT/'third_party/busybox/busybox-1.37.0.tar.bz2'
 if hashlib.sha256(source.read_bytes()).hexdigest()!=SHA:raise SystemExit('BusyBox source hash mismatch')
 if not a.package_only:
  if not a.zig or not sys.platform.startswith('linux'):p.error('Build on Linux with --zig /path/to/zig; Mac uses the shipped guest ELF')
  zig=str(Path(a.zig).resolve());env=dict(os.environ,VMFREE_ZIG=zig)
  with tempfile.TemporaryDirectory() as temp:
   with tarfile.open(source) as tf:tf.extractall(temp,filter='data')
   tree=Path(temp)/'busybox-1.37.0'
   shutil.copy(d/'busybox.config',tree/'.config')
   wrapper=Path(temp)/'cc';shutil.copy(d/'compiler.py',wrapper);wrapper.chmod(0o755)
   subprocess.run(['make','oldconfig'],cwd=tree,env=env,stdin=subprocess.DEVNULL,check=True)
   subprocess.run(['make','-j4','CC='+str(wrapper),'SKIP_STRIP=y'],cwd=tree,env=env,check=True)
   shutil.copy(tree/'busybox',d/'busybox.elf')
  subprocess.run([zig,'cc','-target','aarch64-linux-musl','-static','-nostdlib','-fno-pie','-no-pie','-Wl,-e,_start',str(d/'conformance.S'),'-o',str(d/'conformance.elf')],check=True)
 raw=tar_bytes([('bin/busybox',(d/'busybox.elf').read_bytes(),tarfile.REGTYPE,0o755),('hello.txt',b'Hello from BusyBox on VM-free!\n',tarfile.REGTYPE,0o644)])
 write_layout(d/'busybox-image',[raw],dict(Entrypoint=['/bin/busybox'],Cmd=['echo','BusyBox on VM-free'],Env=[]),compressed=True)
 paths=[d/'conformance.S',d/'conformance.elf',d/'busybox.elf',d/'busybox.config',d/'compiler.py',source]
 (d/'build-manifest.json').write_text(json.dumps(dict(version='1.37.0',source_url='https://busybox.net/downloads/busybox-1.37.0.tar.bz2',source_modifications='none',configuration='Minimal five-applet static ARM64 Linux musl build, -O0 applet code',toolchain='Zig 0.13.0',sha256={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in paths}),indent=2)+'\n')
 print('Packaged M18 BusyBox ELF and OCI image')
if __name__=='__main__':main()
