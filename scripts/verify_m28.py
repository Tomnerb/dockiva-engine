#!/usr/bin/env python3
"""Unmodified dynamic-musl BusyBox and malformed loader acceptance."""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path,required=True);p.add_argument('--linux-reference',action='store_true');a=p.parse_args()
rows=[]
cases=[('echo',['echo','dynamic'],b'dynamic\n'),('shell',['sh','-c','x=4; echo $((x+3)); for x in a b; do echo "$x"; done'],b'7\na\nb\n'),('nested',['sh','-c','echo hello | cat; sh -c "echo child"; echo done'],b'hello\nchild\ndone\n'),('files',['sh','-c','cd /bin; echo abc >/out; cat /out; pwd'],b'abc\n/bin\n')]
for cache in [[],['--no-decode-cache'],['--no-lookup-cache']]:
    for name,args,wanted in cases:
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)/'root';shutil.copytree(ROOT/'examples/m28/root',root)
            r=subprocess.run([str(a.native.resolve()),'run',str(root/'bin/busybox'),'--root',str(root),'--dynamic','--process-tree','--writable','--env','PATH=/bin','--max-steps','20000000',*cache,'--',*args],capture_output=True,timeout=20)
            ok=r.returncode==0 and r.stdout==wanted and not r.stderr
            rows.append(dict(name=name+''.join(cache),passed=ok,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
            print(('PASS ' if ok else 'FAIL ')+rows[-1]['name'],flush=True)
            if not ok:print(rows[-1],flush=True)
for cache in [[],['--no-decode-cache'],['--no-lookup-cache']]:
    r=subprocess.run([str(a.native.resolve()),'run',str(ROOT/'examples/m28/root/bin/dynamic-probe'),'--root',str(ROOT/'examples/m28/root'),'--dynamic',*cache],capture_output=True,timeout=10)
    rows.append(dict(name='constructor_tls'+''.join(cache),passed=r.returncode==0 and r.stdout==b'shared library constructor and TLS passed\n' and not r.stderr,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
    print(('PASS ' if rows[-1]['passed'] else 'FAIL ')+rows[-1]['name'],flush=True)
if a.linux_reference:
    image=json.loads((ROOT/'examples/m28/build-manifest.json').read_text())['image_id']
    for name,args,wanted in cases:
        r=subprocess.run(['docker','run','--rm','--network=none','--entrypoint','/bin/busybox',image,*args],capture_output=True,timeout=20)
        rows.append(dict(name='linux_'+name,passed=r.returncode==0 and r.stdout==wanted and not r.stderr,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
        print(('PASS ' if rows[-1]['passed'] else 'FAIL ')+rows[-1]['name'],flush=True)
    r=subprocess.run(['docker','run','--rm','--network=none','--read-only','-v',str(ROOT/'examples/m28/root')+':/fixture:ro','-e','LD_LIBRARY_PATH=/fixture/lib','--entrypoint','/fixture/bin/dynamic-probe',image],capture_output=True,timeout=20)
    rows.append(dict(name='linux_constructor_tls',passed=r.returncode==0 and r.stdout==b'shared library constructor and TLS passed\n' and not r.stderr,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
    print(('PASS ' if rows[-1]['passed'] else 'FAIL ')+rows[-1]['name'],flush=True)
# Interpreter lookup must stay inside the guest tree and reject links.
for mode in ['missing','symlink','bad-elf','relative-interpreter','parent-interpreter','no-opt-in']:
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)/'root';shutil.copytree(ROOT/'examples/m28/root',root)
        loader=root/'lib/ld-musl-aarch64.so.1';binary=root/'bin/busybox'
        if mode=='missing':loader.unlink()
        if mode=='symlink':loader.unlink();loader.symlink_to(ROOT/'examples/m28/root/lib/ld-musl-aarch64.so.1')
        if mode=='bad-elf':loader.write_bytes(b'not ELF')
        if mode in ('relative-interpreter','parent-interpreter'):
            data=bytearray(binary.read_bytes());old=b'/lib/ld-musl-aarch64.so.1';off=data.index(old)
            new=b'relative' if mode=='relative-interpreter' else b'/../outside'
            # Keep the declared length, with no internal NUL, to test path policy.
            data[off:off+len(old)]=new+b'/'*(len(old)-len(new));binary.write_bytes(data)
        r=subprocess.run([str(a.native.resolve()),'run',str(binary),'--root',str(root),*([] if mode=='no-opt-in' else ['--dynamic']),'--','echo','unexpected'],capture_output=True,timeout=10)
        rows.append(dict(name=mode,passed=r.returncode==125 and not r.stdout,exit_code=r.returncode,stderr=r.stderr.decode()))
        print(('PASS ' if rows[-1]['passed'] else 'FAIL ')+mode,flush=True)
a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(dict(passed=all(r['passed'] for r in rows),checks=rows,runtime_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest()),indent=2)+'\n')
raise SystemExit(0 if all(r['passed'] for r in rows) else 1)
