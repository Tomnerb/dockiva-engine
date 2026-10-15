#!/usr/bin/env python3
"""Probe unchanged cached database ELF binaries; no service-support claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import socket
import shutil
import subprocess
import tarfile
import tempfile
import time
import uuid
ROOT=Path(__file__).resolve().parents[1]
TARGETS=[('postgresql','sha256:d394728dee24b7791f1969c683f8b8410fe57450713a1db2aff5a500f0f98ab0','usr/local/bin/postgres'),('redis','sha256:3b0f54bf27a77c98d319dc3a6706ed500a8985309f0b6c6407599b869f4a2bd9','usr/local/bin/redis-server'),('valkey','sha256:d33b8c582a23d02e46689d5d98a19853e1ae9a1215595c68b64da4476911d67e','usr/local/bin/valkey-server')]

def run(args,timeout=30):
    started=time.monotonic()
    try:
        p=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
        return dict(exit_code=p.returncode,stdout=p.stdout[-12000:],stderr=p.stderr[-12000:],wall_seconds=time.monotonic()-started)
    except subprocess.TimeoutExpired as error:
        def partial(value):return (value.decode(errors='replace') if isinstance(value,bytes) else value or '')[-12000:]
        return dict(exit_code=None,stdout=partial(error.stdout),stderr=partial(error.stderr),timed_out=True,wall_seconds=time.monotonic()-started)

def startup_probe(command,root,label):
    """Bounded server probe in the disposable fixture; never a Docker fallback."""
    (root/'data').mkdir(exist_ok=True)
    if label=='postgresql':
        return dict(stage='server startup probe; PostgreSQL protocol and persistence not yet verified',
                    ready=False,process=run([*command,'--writable','--','-D','/data']))
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
    args=[*command,'--writable','--','--bind','127.0.0.1','--port',str(port),
          '--dir','/data','--save','','--appendonly','no','--daemonize','no']
    result=dict(stage='temporary memory-only server and PING/SET/GET; persistence untested',ready=False,queries=[])
    with tempfile.TemporaryFile() as output,tempfile.TemporaryFile() as errors:
        proc=subprocess.Popen(args,stdout=output,stderr=errors,stdin=subprocess.DEVNULL)
        started=time.monotonic()
        try:
            deadline=started+15
            while proc.poll() is None and time.monotonic()<deadline:
                logs=os.pread(output.fileno(),65536,0).decode(errors='replace')
                if 'ready to accept connections' in logs.lower():
                    with socket.create_connection(('127.0.0.1',port),timeout=2) as client:
                        client.settimeout(2);stream=client.makefile('rb')
                        key=('vmfree-probe-'+uuid.uuid4().hex).encode()
                        for request,expected in [(b'PING\r\n',b'+PONG\r\n'),(b'SET '+key+b' value\r\n',b'+OK\r\n'),(b'GET '+key+b'\r\n',b'$5\r\nvalue\r\n')]:
                            client.sendall(request);response=stream.readline(1024)
                            if response.startswith(b'$5\r\n'):response+=stream.readline(1024)
                            result['queries'].append(dict(request=request.decode().strip(),response=response.decode(errors='replace'),passed=response==expected))
                        result['ready']=all(q['passed'] for q in result['queries'])
                        client.sendall(b'SHUTDOWN NOSAVE\r\n')
                        stream.close()
                    break
                time.sleep(.05)
            if proc.poll() is None:
                try:proc.wait(timeout=2)
                except subprocess.TimeoutExpired:result['stopped_by_verifier']=True;proc.terminate()
        except (OSError,TimeoutError) as error:
            result['probe_error']=str(error)
        finally:
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
        output.seek(0);errors.seek(0)
        result['process']=dict(exit_code=proc.returncode,stdout=output.read(65536).decode(errors='replace'),stderr=errors.read(65536).decode(errors='replace'),wall_seconds=time.monotonic()-started)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,default=ROOT/'evidence/database-probes.json')
    parser.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime')
    parser.add_argument('--readelf',default='/opt/homebrew/opt/llvm/bin/llvm-readelf')
    parser.add_argument('--expanded-limits',action='store_true')
    parser.add_argument('--target',choices=[row[0] for row in TARGETS])
    parser.add_argument('--trace',action='store_true')
    parser.add_argument('--keep-fixtures',type=Path,help='Save extracted files and manifest for repeatable local debugging')
    parser.add_argument('--startup',action='store_true',help='Also try disposable server startup; Redis/Valkey PING/SET/GET if ready')
    parser.add_argument('--max-steps',type=int,default=20000000)
    a=parser.parse_args();results=[]
    if not 1 <= a.max_steps <= 1000000000:parser.error('--max-steps must be 1..1000000000')
    for label,image,binary in TARGETS:
        if a.target and a.target!=label:continue
        print('Probing',label,flush=True)
        info=json.loads(subprocess.check_output(['docker','image','inspect',image]))[0]
        if info['Architecture']!='arm64':raise ValueError('ARM64 image required')
        name='vmfree-db-probe-'+uuid.uuid4().hex
        row=dict(database=label,image_id=image,repository_digests=info.get('RepoDigests',[]),binary='/'+binary,
                 service_qualified=False,files={},adaptation='Direct executable and transitive ELF libraries; archive link targets materialized as regular files. Original ELF bytes unchanged. Image entrypoint, environment and full filesystem are not reproduced.')
        with tempfile.TemporaryDirectory(prefix='vmfree-db-') as temporary:
            work=Path(temporary);root=work/'root';root.mkdir()
            try:
                subprocess.run(['docker','create','--name',name,'--network=none','--entrypoint','/'+binary,image,'--version'],check=True,capture_output=True,timeout=30)
                subprocess.run(['docker','export','--output',str(work/'image.tar'),name],check=True,capture_output=True,timeout=120)
                with tarfile.open(work/'image.tar') as archive:
                    members={m.name.lstrip('./'):m for m in archive.getmembers()}
                    def canonical(path):
                        value=posixpath.normpath('/'+path).lstrip('/')
                        if '\\' in value or '\0' in value:raise ValueError('invalid path')
                        return value
                    def resolve(path,depth=0):
                        if depth>32:raise ValueError('link depth')
                        path=canonical(path);parts=path.split('/')
                        for i in range(1,len(parts)+1):
                            prefix='/'.join(parts[:i]);m=members.get(prefix)
                            if m is not None and m.issym():
                                target=m.linkname if m.linkname.startswith('/') else posixpath.join(posixpath.dirname(prefix),m.linkname)
                                return resolve(posixpath.join(target,*parts[i:]),depth+1)
                        m=members.get(path)
                        if m is None:raise FileNotFoundError(path)
                        if m.islnk():return resolve(m.linkname,depth+1)
                        if not m.isfile() or m.size>64*1024*1024:raise ValueError('nonregular or oversized fixture')
                        return m
                    total=0
                    def copy(path):
                        nonlocal total
                        path=canonical(path);dest=root/path
                        if dest.exists():return dest
                        member=resolve(path);total+=member.size
                        if total>128*1024*1024:raise ValueError('fixture exceeds root budget')
                        dest.parent.mkdir(parents=True,exist_ok=True)
                        data=archive.extractfile(member).read();dest.write_bytes(data);dest.chmod(0o700)
                        row['files']['/'+path]=dict(sha256=hashlib.sha256(data).hexdigest(),bytes=len(data))
                        return dest
                    pending=[binary];seen=set();missing=[]
                    if a.keep_fixtures and label=='postgresql':
                        pending+=['usr/local/bin/initdb','bin/sh']
                        for extra in ['etc/passwd','etc/group','etc/hosts','etc/nsswitch.conf']:
                            try:copy(extra)
                            except FileNotFoundError:pass
                        for member_name,member in members.items():
                            if member_name.startswith('usr/local/share/postgresql/') and member.isfile():copy(member_name)
                            if member_name=='usr/local/lib/postgresql/plpgsql.so' and member.isfile():pending.append(member_name)

                    while pending:
                        path=pending.pop()
                        if path in seen:continue
                        seen.add(path);dest=copy(path)
                        metadata=subprocess.check_output([a.readelf,'-l','-d',str(dest)],text=True)
                        match=re.search(r'Requesting program interpreter:\s*([^\]]+)',metadata)
                        if match:
                            row['interpreter']=match[1];pending.append(match[1].lstrip('/'))
                        for lib in re.findall(r'Shared library: \[([^\]]+)\]',metadata):
                            candidates=['usr/local/lib/'+lib,'lib/'+lib,'usr/lib/'+lib,'lib/aarch64-linux-gnu/'+lib,'usr/lib/aarch64-linux-gnu/'+lib]
                            chosen=None
                            for candidate in candidates:
                                try:resolve(candidate);chosen=candidate;break
                                except FileNotFoundError:pass
                            if chosen:pending.append(chosen)
                            else:missing.append(lib)
                    row['missing_libraries']=sorted(set(missing))
                if a.keep_fixtures:
                    saved=a.keep_fixtures/label
                    if saved.exists():raise ValueError('Refusing to replace existing fixture: '+str(saved))
                    shutil.copytree(root,saved)
                    (a.keep_fixtures/(label+'.json')).write_text(json.dumps(row,indent=2)+'\n')
                row['default_probe']=run([str(a.native.resolve()),'run',str(root/binary),'--root',str(root),'--argv0','/'+binary,'--max-steps','2000000','--','--version'])
                row['max_steps']=a.max_steps
                row['dynamic_probe']=run([str(a.native.resolve()),'run',str(root/binary),'--root',str(root),'--argv0','/'+binary,'--dynamic',*(['--trace'] if a.trace else []),'--job-control','--scheduled-network',*(['--elf-limit-mib','64','--mmap-limit-mib','128'] if a.expanded_limits else []),'--max-steps',str(a.max_steps),'--','--version'])
                if a.startup:
                    row['startup_probe']=startup_probe([str(a.native.resolve()),'run',str(root/binary),'--root',str(root),'--argv0','/'+binary,'--dynamic',*(['--trace'] if a.trace else []),'--job-control','--scheduled-network',*(['--elf-limit-mib','64','--mmap-limit-mib','128'] if a.expanded_limits else []),'--max-steps',str(a.max_steps)],root,label)
                row['outcome']='version-only success; service untested' if row['dynamic_probe']['exit_code']==0 else 'version probe did not exit successfully; service untested'
                reference=name+'-reference'
                try:
                    row['linux_reference']=run(['docker','run','--name',reference,'--network=none','--read-only','--memory=256m','--pids-limit=32','--entrypoint','/'+binary,image,'--version'])
                finally:
                    subprocess.run(['docker','rm','--force','--volumes',reference],capture_output=True,timeout=30)
            finally:
                subprocess.run(['docker','rm','--force','--volumes',name],capture_output=True,timeout=30)
        row['version_matches_linux']=all(row['dynamic_probe'].get(k)==row['linux_reference'].get(k) for k in ('exit_code','stdout','stderr')) and row['dynamic_probe']['exit_code']==0
        results.append(row)
        a.report.parent.mkdir(parents=True,exist_ok=True)
        a.report.write_text(json.dumps(dict(runtime_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest(),scope='Direct ELF probes with optional temporary server startup; not stock image, Dockiva or persistence acceptance',expanded_limits=a.expanded_limits,results=results),indent=2)+'\n')
        print(label,json.dumps({k:row[k] for k in ('default_probe','dynamic_probe','linux_reference',*(['startup_probe'] if a.startup else []))}),flush=True)
if __name__=='__main__':main()
