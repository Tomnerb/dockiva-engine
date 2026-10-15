#!/usr/bin/env python3
"""Prepared pinned Redis: actual background snapshot/rewrite plus restart reads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from verify_redis_preset import request
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime',type=Path,default=ROOT/'build/dockiva-hvf')
    parser.add_argument('--fixtures',type=Path,default=ROOT/'build/database-fixtures-current')
    parser.add_argument('--report',type=Path,default=ROOT/'evidence/redis-native/redis-background.json')
    parser.add_argument('--trace',action='store_true')
    a=parser.parse_args();pin=json.loads((a.fixtures/'redis.json').read_text());source=a.fixtures/'redis'
    for name,info in pin['files'].items():
        path=source/name.lstrip('/')
        if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=info['sha256']:raise RuntimeError('Fixture mismatch: '+name)
    report=dict(scope='Prepared pinned ARM64 Redis ELF; bounded VM-free backend; not stock-image or PostgreSQL qualification',image_id=pin['image_id'],runtime_sha256=hashlib.sha256(a.runtime.read_bytes()).hexdigest(),results=[])
    for mode in ('rdb','aof'):
        with tempfile.TemporaryDirectory(prefix='vmfree-redis-fork-') as tmp:
            root=Path(tmp)/'root';shutil.copytree(source,root);(root/'data').mkdir(exist_ok=True)
            with socket.socket() as reservation:reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
            command=[str(a.runtime.resolve()),'run',str(root/pin['binary'].lstrip('/')),'--cpu-backend','hvf','--dynamic','--process-tree','--scheduled-network','--root',str(root),'--writable','--elf-limit-mib','64','--mmap-limit-mib','2048','--max-steps','1000000000','--argv0',pin['binary'],*(['--trace'] if a.trace else []),'--','--bind','127.0.0.1','--port',str(port),'--dir','/data','--save','','--appendonly','yes' if mode=='aof' else 'no','--appendfsync','always','--daemonize','no']
            expected={f'key-{i}':str(i)+'x'*4096 for i in range(64)}
            for phase in ('background','restart'):
                row=dict(mode=mode,phase=phase,passed=False);report['results'].append(row);started=time.monotonic()
                with tempfile.TemporaryFile() as output,tempfile.TemporaryFile() as errors:
                    proc=subprocess.Popen(command,stdout=output,stderr=errors,stdin=subprocess.DEVNULL)
                    try:
                        deadline=time.monotonic()+15
                        while proc.poll() is None and time.monotonic()<deadline:
                            output.seek(0)
                            if b'ready to accept connections' in output.read().lower():break
                            time.sleep(.05)
                        else:raise RuntimeError(f'Readiness failed; exit={proc.poll()}')
                        with socket.create_connection(('127.0.0.1',port),timeout=3) as client:
                            client.settimeout(20)
                            with client.makefile('rb') as stream:
                                def query(*args):return request(client,stream,*args)
                                def info():return dict(line.split(':',1) for line in query('INFO','persistence').splitlines() if ':' in line)
                                if phase=='background':
                                    for key,value in expected.items():
                                        if query('SET',key,value)!='OK':raise RuntimeError('SET failed')
                                    before=info();command_name='BGSAVE' if mode=='rdb' else 'BGREWRITEAOF'
                                    fork_started=time.monotonic();row['reply']=query(command_name);row['reply_seconds']=time.monotonic()-fork_started;row['before']=before
                                    live_expected=expected.copy();overlap=0
                                    # Parent must continue accepting mutations after the background fork.
                                    for i in range(16):
                                        key=f'post-{i}';value='after-fork-'+str(i)
                                        if query('SET',key,value)!='OK':raise RuntimeError('Post-fork SET failed')
                                        changed=f'key-{i}';changed_value='updated-after-fork-'+str(i)
                                        if query('SET',changed,changed_value)!='OK':raise RuntimeError('Post-fork update failed')
                                        live_expected.update({key:value,changed:changed_value})
                                        if mode=='aof':expected.update({key:value,changed:changed_value})
                                        active='rdb_bgsave_in_progress' if mode=='rdb' else 'aof_rewrite_in_progress'
                                        overlap+=info()[active]=='1'
                                    row['writes_while_child_active']=overlap*2
                                    if not overlap:raise RuntimeError('No acknowledged writes while child was active')
                                    deadline=time.monotonic()+20
                                    while time.monotonic()<deadline:
                                        state=info()
                                        count='rdb_saves' if mode=='rdb' else 'aof_rewrites'
                                        pending='rdb_bgsave_in_progress' if mode=='rdb' else 'aof_rewrite_in_progress'
                                        status='rdb_last_bgsave_status' if mode=='rdb' else 'aof_last_bgrewrite_status'
                                        if int(state[count])>int(before[count]) and state[pending]=='0':
                                            if state[status]!='ok':raise RuntimeError(command_name+' failed')
                                            row['after']=state;row['background_seconds']=time.monotonic()-fork_started;break
                                        time.sleep(.02)
                                    else:raise RuntimeError(command_name+' completion deadline exceeded')
                                verify=live_expected if phase=='background' else expected
                                for key,value in verify.items():
                                    if query('GET',key)!=value:raise RuntimeError('Value mismatch: '+key)
                                if mode=='rdb' and phase=='restart':
                                    for i in range(16):
                                        if query('GET',f'post-{i}') is not None:raise RuntimeError('RDB included a post-fork key')
                                row['verified_keys']=len(verify)
                                client.sendall(b'*2\r\n$8\r\nSHUTDOWN\r\n$6\r\nNOSAVE\r\n')
                                if stream.read(1)!=b'':raise RuntimeError('Shutdown refused')
                        if proc.wait(timeout=5)!=0:raise RuntimeError('Nonzero exit')
                        row['passed']=True
                    except (OSError,RuntimeError,ValueError,KeyError,subprocess.TimeoutExpired) as exc:row['error']=str(exc)
                    finally:
                        if proc.poll() is None:proc.terminate()
                        try:proc.wait(timeout=2)
                        except subprocess.TimeoutExpired:proc.kill();proc.wait()
                        output.seek(0);errors.seek(0,2);errors.seek(max(0,errors.tell()-65536))
                        row.update(seconds=time.monotonic()-started,exit=proc.returncode,stdout=output.read(65536).decode(errors='replace'),stderr=errors.read(65536).decode(errors='replace'))
                print(mode,phase,'PASS' if row['passed'] else row.get('error'),flush=True)
                if not row['passed']:break
    report['passed']=len(report['results'])==4 and all(row['passed'] for row in report['results'])
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
