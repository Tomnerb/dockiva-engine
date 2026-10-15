#!/usr/bin/env python3
"""Bounded prepared-ELF Redis concurrency, clean restarts and SIGKILL recovery.

Tests process interruption, not host power loss. Every observed SET acknowledgement
must survive restart; unanswered writes may or may not survive. No service fallback.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import uuid
from verify_redis_preset import request

ROOT=Path(__file__).resolve().parents[1]


def verify_values(port, expected):
    with socket.create_connection(('127.0.0.1',port),timeout=3) as client:
        client.settimeout(3)
        with client.makefile('rb') as stream:
            if request(client,stream,'PING')!='PONG':raise RuntimeError('PING failed')
            for key,value in expected.items():
                actual=request(client,stream,'GET',key)
                if actual!=value:raise RuntimeError(f'Lost or corrupted acknowledged key {key}: {actual!r} != {value!r}')
    return len(expected)


def write_clients(port, expected, prefix, clients, writes, crash_after, proc):
    barrier=threading.Barrier(clients);lock=threading.Lock();threshold=threading.Event();stop=threading.Event()
    counts={'acknowledged_sets':0,'verified_gets':0,'verified_increments':0,'connected_clients':0,'interrupted_clients':0}
    errors=[];overlap=0;crashed=False
    increments=set();counter=prefix+'-counter';work_deadline=time.monotonic()+20
    if not crash_after:
        with socket.create_connection(('127.0.0.1',port),timeout=3) as client:
            client.settimeout(3)
            with client.makefile('rb') as stream:
                if request(client,stream,'SET',counter,'0')!='OK':raise RuntimeError('Counter initialization failed')
        expected[counter]='0'
    def worker(index):
        nonlocal overlap
        try:
            with socket.create_connection(('127.0.0.1',port),timeout=3) as client:
                client.settimeout(3)
                with client.makefile('rb') as stream:
                    with lock:counts['connected_clients']+=1;overlap=max(overlap,counts['connected_clients'])
                    barrier.wait(timeout=5)
                    for n in range(writes):
                        if stop.is_set():break
                        if time.monotonic()>=work_deadline:raise RuntimeError('Concurrent workload deadline exceeded')
                        key=f'{prefix}-{index}-{n}';value=f'value-{index}-{n}-'+('x'*128)
                        if request(client,stream,'SET',key,value)!='OK':raise RuntimeError('SET did not acknowledge OK')
                        with lock:
                            expected[key]=value;counts['acknowledged_sets']+=1
                            if crash_after and counts['acknowledged_sets']>=crash_after:threshold.set()
                        if request(client,stream,'GET',key)!=value:raise RuntimeError('Concurrent GET mismatch')
                        with lock:counts['verified_gets']+=1
                        if not crash_after:
                            number=request(client,stream,'INCR',counter)
                            with lock:
                                if type(number) is not int or not 1<=number<=clients*writes or number in increments:
                                    raise RuntimeError('Invalid or duplicate shared-counter increment')
                                increments.add(number);counts['verified_increments']+=1
                                expected[counter]=str(max(increments))
        except (OSError,ValueError,RuntimeError,threading.BrokenBarrierError) as exc:
            with lock:
                # Only transport failure after deliberate termination is expected.
                if stop.is_set() and isinstance(exc,(OSError,RuntimeError)) and (
                    isinstance(exc,OSError) or str(exc)=='Truncated Redis response'):
                    counts['interrupted_clients']+=1
                else:errors.append(f'client {index}: {type(exc).__name__}: {exc}')
            barrier.abort()
    with ThreadPoolExecutor(max_workers=clients) as pool:
        futures=[pool.submit(worker,i) for i in range(clients)]
        if crash_after:
            deadline=time.monotonic()+20
            while not threshold.wait(.02):
                if all(f.done() for f in futures) or proc.poll() is not None or time.monotonic()>=deadline:break
            if threshold.is_set() and proc.poll() is None:
                stop.set();proc.kill();crashed=True
                if proc.wait(timeout=5)!=-9:errors.append('Runtime did not exit from SIGKILL')
            else:
                stop.set();barrier.abort();errors.append('Crash acknowledgement threshold not reached')
        for future in futures:future.result(timeout=30)
    if errors:raise RuntimeError('; '.join(errors))
    if overlap!=clients:raise RuntimeError('Not all clients connected before concurrent writes')
    if not crash_after and (counts['acknowledged_sets']!=clients*writes or counts['verified_gets']!=clients*writes or counts['verified_increments']!=clients*writes):
        raise RuntimeError('Incomplete concurrent workload')
    if crash_after and not crashed:raise RuntimeError('No deliberate process crash occurred')
    return dict(counts,clients=clients,all_clients_connected_before_writes=True,deliberate_sigkill=crashed)


def run_cycle(command,port,expected,phase,index,clients,writes,crash_after):
    row={'phase':phase,'index':index,'passed':False,'expected_before':len(expected)}
    with tempfile.TemporaryFile() as output,tempfile.TemporaryFile() as errors:
        proc=subprocess.Popen(command,stdout=output,stderr=errors,stdin=subprocess.DEVNULL)
        try:
            deadline=time.monotonic()+15
            while proc.poll() is None and time.monotonic()<deadline:
                output.seek(0)
                if b'Ready to accept connections' in output.read(65536):break
                time.sleep(.025)
            else:raise RuntimeError(f'Redis readiness failed (exit={proc.poll()})')
            row['recovered_keys_verified']=verify_values(port,expected)
            if phase!='final_recovery':
                row['workload']=write_clients(port,expected,f'{index}-{uuid.uuid4().hex}',clients,writes,crash_after,proc)
            if not crash_after:
                row['post_write_keys_verified']=verify_values(port,expected)
                with socket.create_connection(('127.0.0.1',port),timeout=3) as client:
                    client.settimeout(3);client.sendall(b'*1\r\n$8\r\nSHUTDOWN\r\n')
                    if client.recv(1)!=b'':raise RuntimeError('Shutdown returned unexpected data')
                if proc.wait(timeout=5)!=0:raise RuntimeError('Shutdown did not exit zero')
            row['passed']=True
        except (OSError,RuntimeError,ValueError,subprocess.TimeoutExpired) as exc:row['error']=str(exc)
        finally:
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=2)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=2)
            row.update(exit=proc.returncode,expected_after=len(expected))
            for name,handle in [('stdout',output),('stderr',errors)]:
                handle.seek(0,2);length=handle.tell();handle.seek(max(0,length-16384))
                row[name]=handle.read(16384).decode(errors='replace');row[name+'_bytes']=length
    return row


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime',type=Path,default=ROOT/'build/dockiva-hvf')
    parser.add_argument('--fixtures',type=Path,default=ROOT/'build/database-fixtures-current')
    parser.add_argument('--report',type=Path,default=ROOT/'evidence/redis-recovery/acceptance.json')
    parser.add_argument('--clients',type=int,default=4,choices=range(2,9))
    parser.add_argument('--writes',type=int,default=25,choices=range(1,101))
    parser.add_argument('--clean-cycles',type=int,default=3,choices=range(1,6))
    parser.add_argument('--crash-cycles',type=int,default=2,choices=range(1,6))
    a=parser.parse_args();pin=json.loads((a.fixtures/'redis.json').read_text());source=a.fixtures/'redis'
    for name,info in pin['files'].items():
        p=source/name.lstrip('/')
        if p.is_symlink() or hashlib.sha256(p.read_bytes()).hexdigest()!=info['sha256']:raise RuntimeError('Pinned fixture mismatch: '+name)
    report=dict(scope='Prepared pinned Linux ARM64 ELF, VM-free native execution; concurrent clients, clean restarts, process SIGKILL/AOF recovery; not power-loss or stock-image qualification',image_id=pin['image_id'],runtime_sha256=hashlib.sha256(a.runtime.read_bytes()).hexdigest(),passed=False,cycles=[])
    expected={}
    try:
        with tempfile.TemporaryDirectory(prefix='vmfree-redis-recovery-') as tmp:
            root=Path(tmp)/'root';shutil.copytree(source,root);(root/'data').mkdir(exist_ok=True)
            with socket.socket() as reservation:reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
            command=[str(a.runtime.resolve()),'run',str(root/pin['binary'].lstrip('/')),'--cpu-backend','hvf','--dynamic','--process-tree','--scheduled-network','--root',str(root),'--writable','--elf-limit-mib','64','--mmap-limit-mib','2048','--max-steps','1000000000','--argv0',pin['binary'],'--','--bind','127.0.0.1','--port',str(port),'--dir','/data','--save','','--appendonly','yes','--appendfsync','always','--daemonize','no']
            phases=['clean']*a.clean_cycles+['crash']*a.crash_cycles+['final_recovery']
            for index,phase in enumerate(phases):
                threshold=min(24,a.clients*a.writes) if phase=='crash' else 0
                row=run_cycle(command,port,expected,phase,index,a.clients,a.writes,threshold)
                report['cycles'].append(row)
                print(phase,index,'PASS' if row['passed'] else row.get('error'),f'acknowledged keys={len(expected)}',flush=True)
                if not row['passed']:break
            report['passed']=len(report['cycles'])==len(phases) and all(c['passed'] for c in report['cycles'])
            report['acknowledged_keys']=len(expected)
            report['acknowledged_data_sha256']=hashlib.sha256(json.dumps(expected,sort_keys=True).encode()).hexdigest()
            report['aof_files']=[dict(path=str(p.relative_to(root/'data')),bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted((root/'data').rglob('*')) if p.is_file()]
    finally:
        a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
