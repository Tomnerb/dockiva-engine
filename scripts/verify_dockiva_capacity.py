#!/usr/bin/env python3
"""Isolated bundled-engine concurrency/recovery tests and steady-process accounting."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import re
import socket
import statistics
import subprocess
import sys
import tempfile
import time

class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost', timeout=25)
        self.path = str(path)
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)

def cpu_seconds(value):
    days, sep, rest = value.partition('-')
    total = int(days)*86400 if sep else 0
    parts = (rest if sep else value).split(':')
    for index, part in enumerate(reversed(parts)):
        total += float(part)*60**index
    return total

def process_snapshot(home):
    raw = subprocess.check_output(['ps','-axo','pid=,ppid=,rss=,time=,command='], text=True)
    rows = []
    for line in raw.splitlines():
        fields = line.split(None, 4)
        if len(fields) != 5:
            continue
        pid, parent, rss, cpu, command = fields
        rows.append(dict(pid=int(pid), parent=int(parent), rss_bytes=int(rss)*1024,
                         cpu_seconds=cpu_seconds(cpu), command=command))
    owned = {row['pid'] for row in rows if str(home) in row['command']}
    while True:
        expanded = owned | {row['pid'] for row in rows if row['parent'] in owned}
        if expanded == owned:
            break
        owned = expanded
    # Command lines are used only for attribution, not recorded in the report.
    return [{k:v for k,v in row.items() if k != 'command'} for row in rows if row['pid'] in owned]

def measure(home, seconds):
    samples = []
    for _ in range(seconds+1):
        samples.append(dict(at=time.monotonic(), processes=process_snapshot(home)))
        if len(samples) <= seconds:
            time.sleep(1)
    first={p['pid']:p for p in samples[0]['processes']}
    last={p['pid']:p for p in samples[-1]['processes']}
    stable=set(first)==set(last) and all({p['pid'] for p in sample['processes']}==set(first) for sample in samples)
    elapsed=samples[-1]['at']-samples[0]['at']
    cpu=sum(last[pid]['cpu_seconds']-first[pid]['cpu_seconds'] for pid in first) if stable else None
    rss=[sum(p['rss_bytes'] for p in sample['processes']) for sample in samples]
    return dict(samples=samples, stable_process_set=stable, process_count=len(last),
                summed_rss_mib_median=statistics.median(rss)/1024**2,
                summed_rss_mib_max=max(rss)/1024**2,
                cpu_percent_one_core=100*cpu/elapsed if cpu is not None else None)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--sample-seconds', type=int, default=10)
    args=parser.parse_args()
    if not 3 <= args.sample_seconds <= 60:
        parser.error('sample duration must be 3..60 seconds')
    helper=args.helper.resolve(strict=True)
    report=dict(host=platform.platform(), machine=platform.machine(),
                host_ram_bytes=int(subprocess.check_output(['sysctl','-n','hw.memsize'])),
                helper_sha256=hashlib.sha256(helper.read_bytes()).hexdigest(), checks=[], measurements={},
                scope='Bundled API, all supervisors and native interpreters in an isolated store. Excludes Dockiva UI and WebView processes.',
                memory_method='Sum of process RSS; shared pages may be counted more than once. Not physical footprint.',
                cpu_method='Delta of ps cumulative CPU time during stable process sets; 100% means one full core.',
                dockiva_total_ram_mib=None, eight_gb_hardware_qualified=False, docker_savings=None)
    engine=None
    with tempfile.TemporaryDirectory(prefix='dv-cap-',dir='/tmp') as directory:
        home=Path(directory)
        def api(method,path,body=None):
            token=json.loads((home/'docker-client/config.json').read_text())['HttpHeaders']['X-VMFree-Token']
            conn=UnixHTTP(home/'engine.sock')
            try:
                conn.request(method,'/v1.45'+path,body=json.dumps(body) if body is not None else None,
                             headers={'X-VMFree-Token':token,'Content-Type':'application/json'})
                response=conn.getresponse();return response.status,response.read()
            finally:
                conn.close()
        def check(name, success):
            if not success:
                raise AssertionError(name)
            report['checks'].append(name)
            print('PASS',name,flush=True)
        def launch():
            log=(home/'engine.log').open('ab')
            proc=subprocess.Popen([str(helper),'--home',str(home)],stdin=subprocess.DEVNULL,stdout=log,stderr=log)
            log.close()
            try:
                for _ in range(200):
                    if proc.poll() is not None:
                        raise RuntimeError((home/'engine.log').read_text())
                    try:
                        if api('GET','/_ping')[0]==200:
                            return proc
                    except (OSError,ValueError):
                        pass
                    time.sleep(.1)
                raise TimeoutError('engine startup')
            except BaseException:
                proc.kill();proc.wait();raise
        def stop_engine():
            engine.terminate()
            try:
                engine.wait(5)
            except subprocess.TimeoutExpired:
                engine.kill();engine.wait()
        def port(name):
            status,raw=api('GET',f'/containers/{name}/logs?stdout=1&tail=1')
            found=re.search(rb'PORT (\d+)',raw)
            if status!=200 or not found:
                raise RuntimeError((status,raw))
            return int(found[1])
        def fetch(p):
            start=time.perf_counter();conn=http.client.HTTPConnection('127.0.0.1',p,timeout=5)
            try:
                conn.request('GET','/');response=conn.getresponse();body=response.read()
                if response.status!=200 or b'Persistent VM-free HTTP server OK' not in body:
                    raise AssertionError('HTTP content')
                return (time.perf_counter()-start)*1000
            finally:
                conn.close()
        try:
            engine=launch()
            report['measurements']['engine_idle']=measure(home,args.sample_seconds)
            for i in range(5):
                name=f'server-{i}'
                status,_=api('POST',f'/containers/create?name={name}',{'Image':'dockiva/server:demo','Cmd':['0'],'HostConfig':{'NetworkMode':'host','ReadonlyRootfs':True}})
                check(f'create {name}',status==201)
                if i<4:
                    check(f'start {name}',api('POST',f'/containers/{name}/start')[0]==204)
            ports=[port(f'server-{i}') for i in range(4)]
            with ThreadPoolExecutor(max_workers=4) as pool:
                timings=list(pool.map(fetch,ports*25))
            report['http_latency_ms']=dict(requests=len(timings),median=statistics.median(timings),maximum=max(timings),p95=sorted(timings)[94])
            check('100 concurrent-dispatch HTTP requests across four containers',len(timings)==100)
            check('fifth active runtime rejected',api('POST','/containers/server-4/start')[0]==409)
            report['measurements']['four_servers_idle']=measure(home,args.sample_seconds)
            previous_token=json.loads((home/'docker-client/config.json').read_text())['HttpHeaders']['X-VMFree-Token']
            stop_engine()
            for p in ports:
                fetch(p)
            check('all four workloads survive API shutdown',True)
            engine=launch()
            token=json.loads((home/'docker-client/config.json').read_text())['HttpHeaders']['X-VMFree-Token']
            check('new API token after restart',token!=previous_token)
            status,raw=api('GET','/containers/json?all=1')
            check('four running containers rediscovered',status==200 and sum(r['State']=='running' for r in json.loads(raw))==4)
            check('recovered supervisor accepts stop',api('POST','/containers/server-0/stop')[0]==204)
            check('freed slot admits fifth container',api('POST','/containers/server-4/start')[0]==204)
            fetch(port('server-4'))
            check('fifth container serves HTTP',True)
            check('recovered supervisor accepts restart',api('POST','/containers/server-1/restart')[0]==204)
            fetch(port('server-1'))
            check('restarted container serves HTTP',True)
        finally:
            # Development-only legacy control client also cleans up if the API has failed.
            for i in range(5):
                for action in ('stop','rm'):
                    subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'containers.py'),'--state-dir',str(home/'store'/'containers'),action,f'server-{i}'],capture_output=True,timeout=30)
            if engine is not None and engine.poll() is None:
                stop_engine()
        check('no remaining store-owned processes',not process_snapshot(home))
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:{metric:value for metric,value in v.items() if metric!='samples'} for k,v in report['measurements'].items()},indent=2))

if __name__=='__main__':
    main()
