#!/usr/bin/env python3
"""Engine-level benchmarks: cold start, memory footprint, host -> container network.
Usage: bench3.py vmfree|orbstack OUT.json. Needs nginx:alpine and redis:7-alpine."""
import json,os,re,statistics,subprocess,sys,time,urllib.request
ENGINE,OUT=sys.argv[1],sys.argv[2]
HOSTS={'vmfree':'unix://'+os.path.expanduser('~/.vmfree-clitest/docker.sock'),'orbstack':'unix://'+os.path.expanduser('~/.orbstack/run/docker.sock')}
ENV=dict(os.environ,DOCKER_HOST=HOSTS[ENGINE])
S=os.path.dirname(os.path.abspath(__file__))
def sh(c,timeout=600,check=True):
    r=subprocess.run(c,shell=True,env=ENV,capture_output=True,text=True,timeout=timeout)
    if check and r.returncode: raise RuntimeError(c+': '+(r.stdout+r.stderr)[-300:])
    return r.stdout
res=json.load(open(OUT)) if os.path.exists(OUT) else {}
def case(name,unit,f):
    try: res[name]={'value':round(f(),3),'unit':unit}
    except Exception as e: res[name]={'error':str(e)[-300:]}
    print(name,res[name],flush=True);json.dump(res,open(OUT,'w'),indent=1)
def rss_mib(pattern):
    """Resident memory of the engine's host processes, in MiB (ps RSS)."""
    out=subprocess.run(['ps','-axo','rss=,command='],capture_output=True,text=True).stdout
    return sum(int(l.split(None,1)[0]) for l in out.splitlines() if re.search(pattern,l))/1024
PATTERN={'vmfree':r'/(dockiva-engine|dockivad|dockiva-worker|dockiva-hvf|dockiva-runtime)( |$)','orbstack':r'OrbStack|orbstack|/orb |vmgr|com\.apple\.Virtualization'}[ENGINE]
def stop_engine():
    if ENGINE=='orbstack': subprocess.run('orb stop',shell=True,capture_output=True)
    else: subprocess.run(['pkill','-f','^'+S+'/eb/dockiva-engine/'],capture_output=True)
    time.sleep(3)
def start_engine():
    t=time.time()
    if ENGINE=='orbstack': subprocess.Popen('orb start',shell=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    else: subprocess.Popen([S+'/eb/dockiva-engine/dockiva-engine','--home',os.path.expanduser('~/.vmfree-clitest')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
    while subprocess.run('docker info',shell=True,env=ENV,capture_output=True).returncode:
        if time.time()-t>120: raise RuntimeError('engine did not start')
        time.sleep(0.05)
    t1=time.time()-t
    sh('docker run --rm alpine:latest true')
    return t1,time.time()-t
# 1. Cold start: engine stopped -> docker info answers; and -> first container ran
stop_engine();a,b=start_engine()
res['engine cold start to ready']={'value':round(a,3),'unit':'s'};res['engine cold start to first container']={'value':round(b,3),'unit':'s'}
print(res['engine cold start to ready'],res['engine cold start to first container'],flush=True)
time.sleep(5)
# 2. Memory: idle engine, then with 5 idle containers (alpine sleep)
case('memory idle engine','MiB',lambda:rss_mib(PATTERN))
for i in range(5): sh(f'docker rm -f membench{i}',check=False);sh(f'docker run -d --name membench{i} alpine:latest sleep 600')
time.sleep(5)
case('memory with 5 idle containers','MiB',lambda:rss_mib(PATTERN))
for i in range(5): sh(f'docker rm -f membench{i}',check=False)
# 3. Host -> published port: HTTP requests/s to nginx, and redis round trips
sh('docker rm -f netbench-nginx netbench-redis',check=False)
sh('docker run -d --name netbench-nginx -p 28480:80 nginx:alpine');sh('docker run -d --name netbench-redis -p 28479:6379 redis:7-alpine')
for _ in range(100):
    try: urllib.request.urlopen('http://127.0.0.1:28480/',timeout=1).read();break
    except Exception: time.sleep(0.2)
def http_rps():
    import http.client
    n=3000;c=http.client.HTTPConnection('127.0.0.1',28480,timeout=5);t=time.time()
    for _ in range(n): c.request('GET','/');c.getresponse().read()
    return n/(time.time()-t)
case('host->nginx keep-alive HTTP req/s','req/s',lambda:statistics.median(http_rps() for _ in range(3)))
def redis_rtt():
    import socket
    for _ in range(50):
        try: s=socket.create_connection(('127.0.0.1',28479),timeout=2);break
        except OSError: time.sleep(0.2)
    n=20000;t=time.time()
    for _ in range(n):
        s.sendall(b'*1\r\n$4\r\nPING\r\n');s.recv(64)
    s.close();return n/(time.time()-t)
case('host->redis PING round trips/s','ops/s',lambda:statistics.median(redis_rtt() for _ in range(3)))
sh('docker rm -f netbench-nginx netbench-redis',check=False)
print(json.dumps(res,indent=1))
