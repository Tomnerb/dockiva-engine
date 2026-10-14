#!/usr/bin/env python3
"""Compatibility sweep: run each image's typical workload on the VM-free engine."""
import json,os,subprocess,sys,time,urllib.request
H=os.path.expanduser('~/.vmfree-clitest');ENV=dict(os.environ,DOCKER_HOST='unix://'+H+'/docker.sock')
OUT=sys.argv[1];only=set(sys.argv[2:])
def sh(cmd,timeout=120,input=None):
    try:
        r=subprocess.run(cmd,shell=True,env=ENV,capture_output=True,text=True,timeout=timeout,input=input)
        return r.returncode,(r.stdout+r.stderr)
    except subprocess.TimeoutExpired as e: return 124,'timeout'
def http(url,timeout=60,want=''):
    end=time.time()+timeout;last=''
    while time.time()<end:
        try:
            body=urllib.request.urlopen(url,timeout=3).read().decode(errors='replace')
            if want in body: return True,body[:120]
            last=body[:120]
        except Exception as e: last=str(e)
        time.sleep(1)
    return False,last
def logs_until(name,text,timeout=120):
    end=time.time()+timeout
    while time.time()<end:
        _,o=sh(f'docker logs {name}',20)
        if text in o: return True,''
        _,st=sh(f"docker inspect -f '{{{{.State.Status}}}}' {name}",10)
        if st.strip() in ('exited','dead'): return False,'exited: '+o[-600:]
        time.sleep(2)
    _,o=sh(f'docker logs {name}',20);return False,'timeout: '+o[-600:]
def server(name,image,args,port,url,want,extra=''):
    sh(f'docker rm -f {name}',30)
    c,o=sh(f'docker run -d --name {name} -p {port} {extra} {image} {args}',120)
    if c: return False,o
    ok,b=http(url,90,want)
    return ok,b
def db(name,image,env,ready,check,timeout=180,extra=''):
    sh(f'docker rm -f {name}',30)
    c,o=sh(f'docker run -d --name {name} {env} {extra} {image}',120)
    if c: return False,o
    ok,why=logs_until(name,ready,timeout)
    if not ok: return False,why
    for _ in range(20):
        c,o=sh(f'docker exec {name} {check}',60)
        if c==0: return True,o.strip()[-200:]
        time.sleep(3)
    return False,o[-400:]
def run(image,cmd,want,timeout=180,extra=''):
    c,o=sh(f"docker run --rm {extra} {image} {cmd}",timeout)
    return c==0 and want in o,o.strip()[-400:]
CASES={
 'nginx':lambda:server('sw-nginx','nginx:alpine','','28081:80','http://127.0.0.1:28081/','Welcome to nginx'),
 'httpd':lambda:server('sw-httpd','httpd:alpine','','28082:80','http://127.0.0.1:28082/','It works'),
 'caddy':lambda:server('sw-caddy','caddy:alpine','','28083:80','http://127.0.0.1:28083/','Caddy'),
 'traefik':lambda:server('sw-traefik','traefik:v3.2','--api.insecure=true','28084:8080','http://127.0.0.1:28084/api/version','Version'),
 'memcached':lambda:(lambda r:(r[0],r[1]))(server('sw-memcached','memcached:alpine','','28085:11211','http://127.0.0.1:1/','x') if False else (sh('docker rm -f sw-mc',30),sh('docker run -d --name sw-mc memcached:alpine',60),time.sleep(2),sh("docker exec sw-mc sh -c 'printf \"set k 0 0 2\\r\\nhi\\r\\nget k\\r\\nquit\\r\\n\" | nc 127.0.0.1 11211'",30))[-1] and ((lambda o:( 'VALUE k' in o[1], o[1][-200:]))(sh("docker exec sw-mc sh -c 'printf \"set k 0 0 2\\r\\nhi\\r\\nget k\\r\\nquit\\r\\n\" | nc 127.0.0.1 11211'",30)))),
 'node-alpine':lambda:run('node:22-alpine',"node -e \"const c=require('crypto');console.log('ok',c.createHash('sha256').update('x').digest('hex').slice(0,8),require('os').totalmem())\"",'ok 2d711642'),
 'npm':lambda:run('node:22-alpine',"sh -c 'cd /tmp && npm init -y >/dev/null && npm --version'",'.'),
 'python-alpine':lambda:run('python:3.12-alpine',"python -c \"import ssl,sqlite3,json,hashlib,multiprocessing as m;print('ok',ssl.OPENSSL_VERSION.split()[0],m.Pool(2).map(abs,[-1,-2]))\"",'ok OpenSSL [1, 2]'),
 'golang':lambda:run('golang:1.23-alpine',"sh -c 'cd /tmp && echo cGFja2FnZSBtYWluCmltcG9ydCAiZm10IgpmdW5jIG1haW4oKXtmbXQuUHJpbnRsbigiZ28tb2siKX0K | base64 -d > m.go && go run m.go'",'go-ok',300),
 'mariadb':lambda:db('sw-mariadb','mariadb:11','-e MARIADB_ROOT_PASSWORD=pw','ready for connections',"mariadb -uroot -ppw -e 'select version()'",300),
 'mysql':lambda:db('sw-mysql','mysql:8.4','-e MYSQL_ROOT_PASSWORD=pw','ready for connections. Version',"mysql -uroot -ppw -e 'select version()'",300),
 'mongo':lambda:db('sw-mongo','mongo:7','','Waiting for connections',"mongosh --quiet --eval 'db.runCommand({ping:1}).ok'",240),
 'rabbitmq':lambda:db('sw-rabbit','rabbitmq:3-management-alpine','','Server startup complete','rabbitmq-diagnostics -q ping',300),
 'kafka':lambda:db('sw-kafka','apache/kafka:3.8.0','','Kafka Server started','/opt/kafka/bin/kafka-topics.sh --bootstrap-server localhost:9092 --list',300),
 'java':lambda:run('eclipse-temurin:21-jre',"java -XX:+PrintFlagsFinal -version",'MaxHeapSize'),
 'java-2g-heap':lambda:run('eclipse-temurin:21-jre',"java -Xmx2g -version",'Runtime Environment',extra='-m 3g'),
 'prometheus':lambda:server('sw-prom','prom/prometheus:latest','','28090:9090','http://127.0.0.1:28090/-/ready','Ready'),
 'grafana':lambda:server('sw-grafana','grafana/grafana:latest','','28091:3000','http://127.0.0.1:28091/api/health','database'),
 'elasticsearch':lambda:server('sw-es','elasticsearch:8.15.3','','28092:9200','http://127.0.0.1:28092/','cluster_name',extra='-m 2g -e discovery.type=single-node -e xpack.security.enabled=false -e ES_JAVA_OPTS=-Xmx512m'),
 'node-full':lambda:run('node:22',"node -e \"console.log('ok')\"",'ok'),
 'python-full':lambda:run('python:3.12',"python -c \"print('ok')\"",'ok'),
}
res={}
if os.path.exists(OUT): res=json.load(open(OUT))
for k,f in CASES.items():
    if only and k not in only: continue
    t=time.time()
    try: ok,detail=f()
    except Exception as e: ok,detail=False,repr(e)
    res[k]={'ok':bool(ok),'seconds':round(time.time()-t),'detail':str(detail)[-600:]}
    print(('PASS ' if ok else 'FAIL ')+k,res[k]['seconds'],'s',('' if ok else res[k]['detail'][-300:].replace('\n',' | ')),flush=True)
    json.dump(res,open(OUT,'w'),indent=1)
