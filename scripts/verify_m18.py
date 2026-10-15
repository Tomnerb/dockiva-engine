#!/usr/bin/env python3
"""Validate the pinned BusyBox fixture, rooted filesystem policies and OCI lifecycle."""
import argparse,hashlib,json,os,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime');p.add_argument('--qemu');p.add_argument('--report',type=Path);a=p.parse_args()
 d=ROOT/'examples/m18';native=str(a.native.resolve());elf=str(d/'busybox.elf');results=[]
 def check(name,passed,**details):
  results.append(dict(name=name,passed=bool(passed),**details));print(('PASS ' if passed else 'FAIL ')+name,flush=True)
  if not passed:raise RuntimeError(name+': '+str(details))
 def run(args,**kw):return subprocess.run([str(v) for v in args],capture_output=True,timeout=15,**kw)
 try:
  manifest=json.loads((d/'build-manifest.json').read_text())
  check('fixture_hashes',all(hashlib.sha256((ROOT/k).read_bytes()).hexdigest()==v for k,v in manifest['sha256'].items()))
  for flags in ([],['--no-decode-cache']):
   probe=run([native,'run',d/'conformance.elf',*flags]);check('instruction_conformance'+('reference_dispatch' if flags else ''),probe.returncode==0 and not probe.stdout and not probe.stderr,stderr=probe.stderr.decode(errors='replace'))
  if a.qemu:
   probe=run([a.qemu,d/'conformance.elf']);check('linux_instruction_conformance',probe.returncode==0 and not probe.stdout and not probe.stderr)
  with tempfile.TemporaryDirectory() as temp:
   root=Path(temp)/'root';root.mkdir();(root/'hello.txt').write_bytes(b'hello file\n');(root/'dir').mkdir();(root/'dir/item').write_bytes(b'nested\n')
   def guest(args,writable=False,flags=()):return run([native,'run',elf,'--root',root,'--max-steps','10000000',*flags,*(['--writable'] if writable else []),'--',*args])
   cases=[('echo',['echo','hello','world'],b'hello world\n'),('cat',['cat','/hello.txt'],b'hello file\n'),('pwd',['pwd'],b'/\n'),('ls',['ls','/dir'],b'item\n')]
   for name,args,expected in cases:
    r=guest(args);check(name,r.returncode==0 and r.stdout==expected and not r.stderr,exit_code=r.returncode,stderr=r.stderr.decode(errors='replace'))
    for flag in ('--no-decode-cache','--no-lookup-cache'):
     ref=guest(args,flags=[flag]);check(name+flag, (ref.returncode,ref.stdout,ref.stderr)==(r.returncode,r.stdout,r.stderr))
    if a.qemu:
     # Relative paths and cwd root make file operations comparable without host chroot.
     qargs=[x[1:] if x.startswith('/') else x for x in args]
     ref=run([a.qemu,elf,*qargs],cwd=root)
     wanted=(str(root)+'\n').encode() if name=='pwd' else expected
     check('linux_reference_'+name,ref.returncode==0 and ref.stdout==wanted and not ref.stderr)
   r=guest(['mkdir','/new'],True);check('mkdir',r.returncode==0 and (root/'new').is_dir() and not r.stderr)
   if a.qemu:
    ref=run([a.qemu,elf,'mkdir','reference-new'],cwd=root);check('linux_reference_mkdir',ref.returncode==0 and not ref.stdout and not ref.stderr and (root/'reference-new').is_dir())
   r=guest(['mkdir','/readonly']);check('readonly_mkdir',r.returncode!=0 and r.returncode!=125 and not (root/'readonly').exists(),stderr=r.stderr.decode(errors='replace'))
   r=guest(['cat','/missing']);check('missing_file',r.returncode==1 and not r.stdout,stderr=r.stderr.decode(errors='replace'))
   outside=Path(temp)/'outside';outside.write_bytes(b'host-secret\n');(root/'link').symlink_to(outside)
   for name,args in [('traversal',['cat','../outside']),('absolute_traversal',['cat','/../outside']),('symlink',['cat','/link']),('stat_symlink',['ls','/link'])]:
    r=guest(args);check(name,r.returncode==1 and b'host-secret' not in r.stdout,stderr=r.stderr.decode(errors='replace'))
   many=root/'many';many.mkdir()
   for i in range(120):(many/f'file-{i:03}').write_bytes(b'')
   r=guest(['ls','/many']);check('directory_multiple_buffers',r.returncode==0 and sorted(r.stdout.splitlines())==[f'file-{i:03}'.encode() for i in range(120)],stderr=r.stderr.decode(errors='replace'))
   image=d/'busybox-image'
   for name,args,expected in [('image_echo',['echo','OCI BusyBox'],b'OCI BusyBox\n'),('image_cat',['cat','/hello.txt'],b'Hello from BusyBox on VM-free!\n')]:
    r=run([sys.executable,ROOT/'oci.py','run',image,'--native',native,'--',*args]);check(name,r.returncode==0 and r.stdout==expected and not r.stderr,stderr=r.stderr.decode(errors='replace'))
   state=Path(temp)/'state';cli=[sys.executable,ROOT/'containers.py','--state-dir',state,'--native',native]
   def call(*args):
    r=run([*cli,*args]);
    if r.returncode:raise RuntimeError(r.stderr.decode(errors='replace'))
    return r.stdout
   def finished(name):
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
     info=json.loads(call('inspect',name))
     if info['status']['state'] in ('exited','failed'):return info
     time.sleep(.05)
    raise RuntimeError('Container did not exit')
   call('create','bb',image,'--arg','mkdir','--arg','/persisted');call('start','bb');info=finished('bb')
   check('container_mkdir_persists',info['status']['exit_code']==0 and (state/'bb/rootfs/persisted').is_dir())
   call('start','bb');info=finished('bb');check('container_restart_existing_directory',info['status']['exit_code']==1 and (state/'bb/rootfs/persisted').is_dir())
   check('container_stderr_logs',b'persisted' in call('logs','bb','--stderr'))
   call('rm','bb');check('container_remove',not (state/'bb').exists())
 except Exception as exc:
  if not results or results[-1]['passed']:results.append(dict(name='verification_error',passed=False,error=str(exc)));print('FAIL verification_error',str(exc))
 finally:
  report=dict(milestone=18,results=results,linux_reference=a.qemu or 'not-run',passed=bool(results) and all(r['passed'] for r in results))
  if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
 return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
