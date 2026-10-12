#!/usr/bin/env python3
"""Measure the same persistent HTTP guest under explicit runtime/VM commands."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import platform
import selectors
import socket
import statistics
import subprocess
import time

def request(port,timeout=10):
    start=time.perf_counter();data=b'';deadline=time.monotonic()+timeout
    with socket.create_connection(('127.0.0.1',port),timeout=timeout) as s:
        s.sendall(b'GET / HTTP/1.1\r\nHost: benchmark\r\n\r\n')
        while True:
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('HTTP response deadline exceeded')
            s.settimeout(remaining)
            chunk=s.recv(8192)
            if not chunk:break
            data+=chunk
    if b'200 OK' not in data.split(b'\r\n',1)[0] or b'Persistent VM-free HTTP server OK' not in data:raise ValueError(f'Unexpected HTTP workload response on port {port}: {data[:512]!r}')
    return (time.perf_counter()-start)*1000

def wait_http_ready(proc,port,timeout=30):
    """A guest PORT line does not guarantee host port forwarding is ready yet."""
    deadline=time.monotonic()+timeout
    last_error=None
    attempts=0
    while True:
        if proc.poll() is not None:
            raise RuntimeError(f'Backend exited with status {proc.returncode} before host HTTP readiness on port {port}')
        remaining=deadline-time.monotonic()
        if remaining<=0:
            raise RuntimeError(f'Host HTTP port 127.0.0.1:{port} did not become ready within {timeout}s; '
                               f'check the configured host port and Docker port mapping. Last error: {last_error}')
        attempts+=1
        try:
            request(port,timeout=min(1.0,remaining))
            return attempts
        except OSError as exc:
            # Only startup probes retry. Invalid HTTP content and measured requests fail.
            last_error=str(exc)
            time.sleep(min(.05,max(0,deadline-time.monotonic())))

def sample(spec,requests,concurrency):
    started=time.perf_counter()
    proc=subprocess.Popen(spec['command'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        ready=b''
        with selectors.DefaultSelector() as selector:
            selector.register(proc.stdout,selectors.EVENT_READ);deadline=time.monotonic()+30
            while b'\n' not in ready:
                if time.monotonic()>deadline:raise RuntimeError('Readiness timeout')
                if not selector.select(.1):continue
                chunk=os.read(proc.stdout.fileno(),512)
                if not chunk:raise RuntimeError('Exited before ready: '+proc.stderr.read().decode(errors='replace'))
                ready+=chunk
        lines=[line for line in ready.splitlines() if line.startswith(b'PORT ')]
        if not lines:raise RuntimeError('Expected PORT readiness line')
        port=spec.get('port') or int(lines[0][5:])
        attempts=wait_http_ready(proc,port)
        startup=(time.perf_counter()-started)*1000
        begin=time.perf_counter()
        with ThreadPoolExecutor(max_workers=concurrency) as pool:latencies=list(pool.map(lambda _:request(port),range(requests)))
        elapsed=time.perf_counter()-begin
        return dict(readiness_attempts=attempts,startup_to_first_response_ms=startup,requests_per_second=requests/elapsed,latency_median_ms=statistics.median(latencies),latency_p95_ms=sorted(latencies)[max(0,int(len(latencies)*.95)-1)],requests=requests,concurrency=concurrency)
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        if spec.get('cleanup'):subprocess.run(spec['cleanup'],capture_output=True,timeout=15)
        proc.stdout.close();proc.stderr.close()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--config',type=Path,required=True);p.add_argument('--report',type=Path,required=True);p.add_argument('--samples',type=int,default=5);p.add_argument('--requests',type=int,default=100);p.add_argument('--concurrency',type=int,default=4);a=p.parse_args()
    if not 1<=a.samples<=20 or not 1<=a.requests<=10000 or not 1<=a.concurrency<=8:p.error('Samples 1..20; requests 1..10000; concurrency 1..8')
    specs=json.loads(a.config.read_text())
    report=dict(host=platform.platform(),machine=platform.machine(),timing='wall clock, startup includes launcher and host HTTP readiness retries',cpu_memory_comparison='not measured; do not infer resource savings',results=[],completed=False)
    try:
        for spec in specs:
            if spec.get('kind') not in ('vmfree','linux-vm'):raise ValueError('Set backend kind explicitly')
            if not isinstance(spec.get('command'),list) or not spec['command'] or not all(isinstance(v,str) for v in spec['command']):raise ValueError('Command must be an argument array')
            row=dict(name=spec['name'],kind=spec['kind'],command=spec['command'],samples=[]);report['results'].append(row)
            for i in range(a.samples):row['samples'].append(sample(spec,a.requests,a.concurrency));print(spec['name'],i+1,flush=True)
        report['completed']=True
    except Exception as exc:
        report['error']=str(exc)
        raise
    finally:a.report.write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
