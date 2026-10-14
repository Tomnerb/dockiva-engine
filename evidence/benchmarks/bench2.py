import json,os,statistics,subprocess,sys,time
HOSTS={'vmfree':'unix://'+os.path.expanduser('~/.vmfree-clitest/docker.sock'),'orbstack':'unix://'+os.path.expanduser('~/.orbstack/run/docker.sock')}
ENGINE,OUT=sys.argv[1],sys.argv[2];ENV=dict(os.environ,DOCKER_HOST=HOSTS[ENGINE])
def timed(cmd):
    t=time.perf_counter();r=subprocess.run(cmd,shell=True,env=ENV,capture_output=True,text=True,timeout=900)
    if r.returncode:raise RuntimeError(r.stdout[-300:]+r.stderr[-300:])
    return time.perf_counter()-t
res=json.load(open(OUT)) if os.path.exists(OUT) else {}
GO="cGFja2FnZSBtYWluCmltcG9ydCAoImVuY29kaW5nL2pzb24iOyJuZXQvaHR0cCI7ImNyeXB0by90bHMiKQpmdW5jIG1haW4oKSB7IF8gPSBqc29uLk1hcnNoYWw7IF8gPSB0bHMuQ29uZmlne307IGh0dHAuTGlzdGVuQW5kU2VydmUoIjo4MDgwIiwgbmlsKSB9Cg=="
for name,cmd in [('go build net/http (cold cache)',f"docker run --rm golang:1.23-alpine sh -c 'mkdir /src && cd /src && echo {GO} | base64 -d > m.go && go mod init m >/dev/null 2>&1 && go build -o m .'"),
                 ('awk loop 30M (CPU)',"docker run --rm alpine:latest awk 'BEGIN{for(i=0;i<3e7;i++)s+=i;print s}'")]:
    try: res[name]={'value':round(statistics.median(timed(cmd) for _ in range(3)),3),'unit':'s'}
    except Exception as e: res[name]={'error':str(e)[-300:]}
    print(name,res[name],flush=True)
json.dump(res,open(OUT,'w'),indent=1)
