#!/usr/bin/env python3
"""Pinned prepared-ELF Redis acceptance: real RESP, AOF, graceful restart.

No Docker/host-service fallback and no ignored Redis safety warnings. A version
string or readiness log alone never passes. This does not qualify a stock image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]


def request(client, stream, *args):
    parts=[str(x).encode() for x in args]
    client.sendall(b'*'+str(len(parts)).encode()+b'\r\n'+b''.join(b'$'+str(len(p)).encode()+b'\r\n'+p+b'\r\n' for p in parts))
    line=stream.readline(65536)
    if not line.endswith(b'\r\n'):raise RuntimeError('Truncated Redis response')
    if line.startswith(b'$'):
        size=int(line[1:-2])
        if size==-1:return None
        if not 0<=size<=65536:raise RuntimeError('Oversized Redis response')
        value=stream.read(size+2)
        if len(value)!=size+2 or not value.endswith(b'\r\n'):raise RuntimeError('Truncated Redis value')
        return value[:-2].decode()
    if line.startswith(b':'):
        return int(line[1:-2])
    if not line.startswith(b'+'):raise RuntimeError('Unexpected Redis reply: '+repr(line))
    return line[1:-2].decode()


def cycle(command, port, key, first):
    row={'phase':'write' if first else 'restart_read','passed':False,'queries':[]}
    with tempfile.TemporaryFile() as output,tempfile.TemporaryFile() as errors:
        proc=subprocess.Popen(command,stdout=output,stderr=errors,stdin=subprocess.DEVNULL)
        try:
            deadline=time.monotonic()+15
            while proc.poll() is None and time.monotonic()<deadline:
                output.seek(0);logs=output.read(65536).decode(errors='replace')
                if 'ready to accept connections' in logs.lower():break
                time.sleep(.05)
            else:
                if proc.poll() is not None:raise RuntimeError(f'Redis exited before readiness ({proc.returncode})')
                raise RuntimeError('Redis did not become ready within the deadline')
            with socket.create_connection(('127.0.0.1',port),timeout=2) as client:
                client.settimeout(2)
                with client.makefile('rb') as stream:
                    checks=[(('PING',),'PONG')]
                    if first:checks.append((('SET',key,'persisted-value'),'OK'))
                    checks.append((('GET',key),'persisted-value'))
                    for args,expected in checks:
                        reply=request(client,stream,*args);passed=reply==expected
                        row['queries'].append({'command':list(args),'reply':reply,'passed':passed})
                        if not passed:raise RuntimeError('Unexpected query result')
                    client.sendall(b'*1\r\n$8\r\nSHUTDOWN\r\n')
                    if stream.read(1)!=b'':raise RuntimeError('Redis refused shutdown')
            if proc.wait(timeout=5)!=0:raise RuntimeError('Redis did not exit successfully')
            row['passed']=True
        except (OSError,RuntimeError,ValueError,subprocess.TimeoutExpired) as exc:
            row['error']=str(exc)
        finally:
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=2)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            output.seek(0);errors.seek(0)
            row.update(exit=proc.returncode,stdout=output.read(65536).decode(errors='replace'),stderr=errors.read(65536).decode(errors='replace'))
    return row


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime',type=Path,default=ROOT/'build/dockiva-hvf')
    parser.add_argument('--fixtures',type=Path,default=ROOT/'build/database-fixtures-current')
    parser.add_argument('--report',type=Path,default=ROOT/'evidence/redis-native/acceptance.json')
    parser.add_argument('--trace',action='store_true')
    a=parser.parse_args();pin=json.loads((a.fixtures/'redis.json').read_text());source=a.fixtures/'redis'
    for name,expected in pin['files'].items():
        p=source/name.lstrip('/')
        if p.is_symlink() or hashlib.sha256(p.read_bytes()).hexdigest()!=expected['sha256']:
            raise RuntimeError('Pinned fixture mismatch: '+str(p))
    report={'image_id':pin['image_id'],'scope':'Prepared pinned Linux ELF; native Hypervisor.framework backend; no Docker or native Redis fallback',
            'runtime_sha256':hashlib.sha256(a.runtime.read_bytes()).hexdigest(),'service_qualified':False,'cycles':[]}
    with tempfile.TemporaryDirectory(prefix='vmfree-redis-accept-') as tmp:
        root=Path(tmp)/'root';shutil.copytree(source,root);(root/'data').mkdir(exist_ok=True)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
        command=[str(a.runtime.resolve()),'run',str(root/pin['binary'].lstrip('/')),
                 '--cpu-backend','hvf','--dynamic','--process-tree','--scheduled-network','--root',str(root),'--writable',
                 '--elf-limit-mib','64','--mmap-limit-mib','2048','--max-steps','200000000',
                 '--argv0',pin['binary'],*(['--trace'] if a.trace else []),'--','--bind','127.0.0.1','--port',str(port),
                 '--dir','/data','--save','','--appendonly','yes','--appendfsync','always','--daemonize','no']
        key='vmfree-accept-'+uuid.uuid4().hex
        for first in (True,False):
            result=cycle(command,port,key,first);report['cycles'].append(result)
            if not result['passed']:break
        report['service_qualified']=len(report['cycles'])==2 and all(r['passed'] for r in report['cycles'])
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return 0 if report['service_qualified'] else 1


if __name__=='__main__':raise SystemExit(main())
