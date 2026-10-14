#!/usr/bin/env python3
"""Benchmarks: Dockiva VM-free vs another engine. Usage: bench.py ENGINE OUT.json
ENGINE: vmfree (the test engine) or orbstack. Each case runs 3 times; the median counts."""
import json,os,statistics,subprocess,sys,time
HOSTS={'vmfree':'unix://'+os.path.expanduser('~/.vmfree-clitest/docker.sock'),'orbstack':'unix://'+os.path.expanduser('~/.orbstack/run/docker.sock')}
ENGINE,OUT=sys.argv[1],sys.argv[2];ENV=dict(os.environ,DOCKER_HOST=HOSTS[ENGINE])
BIND=os.path.join(os.path.dirname(os.path.abspath(__file__)),'bench','bind-'+ENGINE)
os.makedirs(BIND,exist_ok=True)
def sh(cmd,timeout=900):
    r=subprocess.run(cmd,shell=True,env=ENV,capture_output=True,text=True,timeout=timeout)
    if r.returncode: raise RuntimeError(f'{cmd}: {(r.stdout+r.stderr)[-600:]}')
    return r.stdout+r.stderr
def timed(cmd,timeout=900):
    t=time.perf_counter();out=sh(cmd,timeout);return time.perf_counter()-t,out
def median(f,n=3): return statistics.median(f() for _ in range(n))
res={}
def case(name,unit,f):
    try: v=f();res[name]={'value':round(v,3),'unit':unit}
    except Exception as e: res[name]={'error':str(e)[-400:]}
    print(name,res[name],flush=True);json.dump(res,open(OUT,'w'),indent=1)
for image in ('alpine:latest','postgres:16-alpine','node:22-alpine','golang:1.23-alpine'): sh(f'docker pull -q {image}',1800)
# 1. Container start to exit
case('run alpine true','s',lambda:median(lambda:timed('docker run --rm alpine:latest true')[0],5))
# 2. A syscall-heavy loop: 2 million 1-byte reads and writes
case('dd 1M x 1-byte','s',lambda:median(lambda:timed('docker run --rm alpine:latest dd if=/dev/zero of=/dev/null bs=1 count=1000000')[0]))
# 3. Postgres TPC-B (pgbench, scale 10, 4 clients, 20 s)
def pgbench():
    sh('docker rm -f bench-pg',60);sh('docker run -d --name bench-pg -e POSTGRES_PASSWORD=pw postgres:16-alpine',120)
    for _ in range(90):
        try: sh("docker exec bench-pg pg_isready -U postgres -h 127.0.0.1",30);break
        except Exception: time.sleep(1)
    time.sleep(3)
    sh('docker exec bench-pg pgbench -U postgres -i -s 10 -q postgres',600)
    tps=[]
    for _ in range(3):
        out=sh('docker exec bench-pg pgbench -U postgres -c 4 -j 4 -T 20 postgres',300)
        tps.append(float(next(l for l in out.splitlines() if l.startswith('tps')).split('=')[1].split()[0]))
    sh('docker rm -f bench-pg',60);return statistics.median(tps)
case('pgbench TPC-B tps','tps',pgbench)
# 4. npm install (network: registry downloads included, cache cleared each run)
case('npm install express+lodash','s',lambda:median(lambda:timed("docker run --rm node:22-alpine sh -c 'cd /tmp && npm init -y >/dev/null && npm install --no-audit --no-fund express@4 lodash >/dev/null'")[0]))
# 5. go build of a net/http program from a cold build cache
GO="cGFja2FnZSBtYWluCmltcG9ydCAoImVuY29kaW5nL2pzb24iOyJuZXQvaHR0cCI7ImNyeXB0by90bHMiKQpmdW5jIG1haW4oKSB7IF8gPSBqc29uLk1hcnNoYWw7IF8gPSB0bHMuQ29uZmlne307IGh0dHAuTGlzdGVuQW5kU2VydmUoIjo4MDgwIiwgbmlsKSB9Cg=="
case('go build net/http (cold cache)','s',lambda:median(lambda:timed(f"docker run --rm golang:1.23-alpine sh -c 'cd /tmp && echo {GO} | base64 -d > m.go && go mod init m >/dev/null 2>&1 && go build -o m .'")[0]))
# 6. Files on a bind mount: 2000 small files written then read; one 256 MiB sequential write
SMALL="cd /b && mkdir -p d && i=0; while [ $i -lt 2000 ]; do echo $i > d/f$i; i=$((i+1)); done; cat d/* > /dev/null"
def small():
    sub=os.path.join(BIND,'d');[os.unlink(os.path.join(sub,f)) for f in (os.listdir(sub) if os.path.isdir(sub) else [])]
    return timed(f"docker run --rm -v {BIND}:/b alpine:latest sh -c '{SMALL}'")[0]
case('bind mount 2000 small files','s',lambda:median(small))
case('bind mount 256 MiB write','s',lambda:median(lambda:timed(f"docker run --rm -v {BIND}:/b alpine:latest dd if=/dev/zero of=/b/big bs=1M count=256 conv=fsync")[0]))
case('container fs 256 MiB write','s',lambda:median(lambda:timed("docker run --rm alpine:latest dd if=/dev/zero of=/tmp/big bs=1M count=256 conv=fsync")[0]))
# 7. CPU-bound: sha256 of 512 MiB
case('sha256 512 MiB (CPU)','s',lambda:median(lambda:timed("docker run --rm alpine:latest sh -c 'head -c 536870912 /dev/zero | sha256sum'")[0]))
print(json.dumps(res,indent=1))
