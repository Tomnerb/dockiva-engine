#!/usr/bin/env python3
"""Combined filesystem, writable state, static musl and lifecycle acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from scripts.verify_m5 import serve

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path);p.add_argument('--qemu');a=p.parse_args()
    if a.qemu and platform.system()!='Linux':p.error('QEMU reference requires Linux')
    native=str(a.native.resolve());d=ROOT/'examples/m7_10';rows=[]
    def record(name,good,**details):
        rows.append(dict(name=name,passed=bool(good),**details));print(('PASS ' if good else 'FAIL ')+name,flush=True)
    def run(cmd):return subprocess.run(cmd,capture_output=True,timeout=15)
    manifest=json.loads((d/'build-manifest.json').read_text())
    record('fixture_hashes',all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==h for n,h in manifest['sha256'].items()))
    with tempfile.TemporaryDirectory(prefix='vmfree-test-') as tmp:
        root=Path(tmp)/'root';root.mkdir();(root/'hello.txt').write_text('hello\n')
        outside=Path(tmp)/'outside';outside.mkdir();(outside/'file').write_text('outside')
        (root/'link').symlink_to(outside/'file');(root/'linked').symlink_to(outside,target_is_directory=True)
        for mode,name in [('r','read_seek_stat'),('w','write_create'),('d','directory_rename_unlink'),('o','readonly_rejection'),('t','traversal_rejection'),('l','symlink_rejection'),('p','bad_path_pointer'),('b','bad_buffer_pointer'),('n','missing_file'),('x','read_fd_write_rejection'),('e','zero_read'),('q','file_size_limit'),('f','file_descriptor_limit'),('a','append')]:
            cmd=[native,'run',str(d/'fs-check.elf'),'--root',str(root)]
            if mode!='o':cmd+=['--writable']
            result=run([*cmd,'--',mode])
            record(name,result.returncode==0 and not result.stderr,exit_code=result.returncode,stderr=result.stderr.decode(errors='replace'))
        record('host_file_unchanged',(outside/'file').read_text()=='outside')
        record('append_content',(root/'counter').read_bytes()==b'1A')
        record('directory_removed',not (root/'dir').exists())
        elf=str(ROOT/'examples/m9/libc-demo.elf')
        for name,args,expected,code in [('libc_startup',[],b'static musl OK\n',0),('libc_file_read',['cat','/hello.txt'],b'hello\n',0),('libc_errno',['cat','/missing'],b'',3),('libc_arguments',['wrong'],b'',7)]:
            result=run([native,'run',elf,'--root',str(root),'--',*args])
            good=(result.returncode,result.stdout,result.stderr)==(code,expected,b'')
            ref=None
            if a.qemu:
                reference_args=['cat',str(root/'hello.txt')] if name=='libc_file_read' else ['cat',str(root/'missing')] if name=='libc_errno' else args
                reference=run([a.qemu,elf,*reference_args]);ref=dict(exit_code=reference.returncode,stdout_hex=reference.stdout.hex(),stderr_hex=reference.stderr.hex())
                good=good and (reference.returncode,reference.stdout,reference.stderr)==(code,expected,b'')
            record(name,good,linux_reference=ref)
        result=run([native,'run',str(d/'isa.elf')]);good=(result.returncode,result.stdout,result.stderr)==(0,b'',b'')
        if a.qemu:
            ref=run([a.qemu,str(d/'isa.elf')]);good=good and (ref.returncode,ref.stdout,ref.stderr)==(0,b'',b'')
        record('integer_vector_conformance',good)
        response=serve([sys.executable,str(ROOT/'oci.py'),'run',str(d/'http-image'),'--native',native,'--','auto'],[b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n'])
        record('image_html_response',response['passed'] and response['response_hex']==(d/'index.http').read_bytes().hex())
        state=Path(tmp)/'state'
        cli=[sys.executable,str(ROOT/'containers.py'),'--state-dir',str(state),'--native',native]
        def call(*args,code=0):
            result=run([*cli,*args])
            if result.returncode!=code:raise RuntimeError(f'{args}: {result.returncode} {result.stderr.decode()}')
            return result.stdout
        def finished(name):
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                info=json.loads(call('inspect',name))
                if info['status']['state'] in ('exited','failed'):return info
                time.sleep(.03)
            raise RuntimeError('Container exit timeout: '+name)
        names=[]
        try:
            for name in ('one','two'):
                call('create',name,str(d/'counter-image'));names.append(name)
            record('create_instances',json.loads(call('inspect','one'))['status']['state']=='created')
            call('start','one');info=finished('one')
            record('exit_code',info['status'].get('exit_code')==0)
            record('stdout_logs',call('logs','one')==b'1\n')
            call('start','one');finished('one')
            record('restart_persists',(state/'one/rootfs/counter').read_bytes()==b'2' and call('logs','one')==b'1\n2\n')
            call('start','two');finished('two')
            record('instance_independence',(state/'two/rootfs/counter').read_bytes()==b'1' and (state/'one/rootfs/counter').read_bytes()==b'2')
            import oci
            record('image_unchanged','counter' not in oci.Image(d/'counter-image').tree)
            call('create','readonly',str(d/'counter-image'),'--read-only');names.append('readonly')
            call('start','readonly');info=finished('readonly')
            record('readonly_container',info['status'].get('exit_code')==20 and not (state/'readonly/rootfs/counter').exists())
            call('create','http',str(d/'http-image'),'--arg','auto');names.append('http')
            call('start','http')
            deadline=time.monotonic()+10;out=b''
            while time.monotonic()<deadline:
                out=call('logs','http')
                if b'\n' in out:break
                time.sleep(.03)
            port=int(out.splitlines()[0][5:])
            record('running_status',json.loads(call('inspect','http'))['status']['state']=='running')
            control=json.loads((state/'http/control.json').read_text())
            with socket.create_connection(('127.0.0.1',control['port']),timeout=2) as conn:
                conn.sendall(b'wrong-token stop\n');conn.recv(16)
            record('control_authentication',json.loads(call('inspect','http'))['status']['state']=='running')
            record('duplicate_start_rejected',run([*cli,'start','http']).returncode==125)
            record('running_remove_rejected',run([*cli,'rm','http']).returncode==125)
            call('stop','http');info=finished('http')
            record('stop_exit_status',info['status'].get('exit_code')==143)
            closed=False
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=1):pass
            except ConnectionRefusedError:closed=True
            record('stop_listener_closed',closed)
            record('list_instances',len(json.loads(call('list')))==4)
            record('stderr_logs',call('logs','one','--stderr')==b'')
            call('rm','two');names.remove('two')
            record('remove_private_state',not (state/'two').exists())
        finally:
            for name in names:
                run([*cli,'stop',name]);run([*cli,'rm',name])
    report=dict(host=platform.platform(),passed=all(r['passed'] for r in rows),cases=rows,reference='qemu-user-linux (musl and ISA only)' if a.qemu else 'not-run')
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1
if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,subprocess.TimeoutExpired) as e:print('Verification failed: '+str(e),file=sys.stderr);sys.exit(1)
