#!/usr/bin/env python3
"""Second compatibility sweep: ~23 more images and Compose stacks, one image set at a
time (pull, test, remove) to keep disk use low. Usage: sweep3.py OUT.json [cases]"""
import base64,json,os,shutil,subprocess,sys,time
HERE=os.path.dirname(os.path.abspath(__file__));D=os.path.join(HERE,'sweep3')
src=open(os.path.join(HERE,'sweep1.py')).read();exec(src[:src.index('CASES={')])   # sh, http, logs_until, db, run
MIN_FREE=8<<30
KEEP=set(json.load(open(os.path.join(HERE,'sweep3-keep.json'))))                 # images present before this sweep
def b64(script): return "sh -c 'echo "+base64.b64encode(script.encode()).decode()+" | base64 -d | sh'"
def server(name,image,args,port,url,want,extra='',timeout=120):
    sh(f'docker rm -f {name}',30)
    c,o=sh(f'docker run -d --name {name} -p {port} {extra} {image} {args}',120)
    if c: return False,o
    ok,b=http(url,timeout,want)
    if not ok: _,l=sh(f'docker logs {name}',20);b=b+' || logs: '+l[-500:]
    return ok,b
def compose(project,want_url,want,timeout=240):
    d=os.path.join(D,project);sh(f'docker compose -p {project} down -v',120,)
    c,o=sh(f'cd {d} && docker compose -p {project} up -d',300)
    if c: return False,o
    ok,b=http(want_url,timeout,want)
    if not ok: _,l=sh(f'cd {d} && docker compose -p {project} logs --tail 30',30);b=b+' || logs: '+l[-800:]
    return ok,b
def mqtt():
    sh('docker rm -f s3-mqtt',30);c,o=sh('docker run -d --name s3-mqtt eclipse-mosquitto:2',120)
    if c: return False,o
    ok,why=logs_until('s3-mqtt','running',60)
    if not ok: return False,why
    c,o=sh("docker exec s3-mqtt sh -c 'mosquitto_sub -t t -C 1 -W 10 & sleep 1; mosquitto_pub -t t -m mqtt-ok; wait'",40)
    return 'mqtt-ok' in o,o[-300:]
DJANGO='''set -e
pip install -q --root-user-action=ignore django==5.1.* 2>&1 | tail -2
cd /tmp && django-admin startproject p && cd p && python manage.py migrate -v0
(python manage.py runserver 0.0.0.0:8000 --noreload >/tmp/log 2>&1 &)
for i in $(seq 60); do python -c "import urllib.request as u;print(u.urlopen('http://localhost:8000/').read().decode()[:2000])" 2>/dev/null | grep -o 'The install worked successfully' && exit 0; sleep 1; done
cat /tmp/log; exit 1'''
RUBY='''ruby -rjson -ropenssl -rsocket -e 's=TCPServer.new("::",9000);t=Thread.new{c=s.accept;c.puts(c.gets.upcase);c.close};c=TCPSocket.new("localhost",9000);c.puts("ruby-ok");puts c.gets, OpenSSL::OPENSSL_LIBRARY_VERSION, JSON.dump([1]);t.join'
gem list 2>/dev/null | head -3'''
DOTNET='''set -e
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
cd /tmp && dotnet new console -o a && dotnet --version'''
PHP='''php -r 'echo json_encode([PHP_VERSION, extension_loaded("pdo_sqlite"), extension_loaded("openssl")]), "\\n";' && php-fpm -t 2>&1'''
CLICK='''for i in $(seq 60); do clickhouse-client -q "select 'ch-' || toString(sum(number)) from numbers(1000)" 2>/dev/null && exit 0; sleep 1; done; exit 1'''
CASES=[ # name, images, test
 ('postgres15',['postgres:15-alpine'],lambda:db('s3-pg15','postgres:15-alpine','-e POSTGRES_PASSWORD=pw','ready to accept connections',"psql -U postgres -tAc 'select version()'",240)),
 ('postgres17',['postgres:17-alpine'],lambda:db('s3-pg17','postgres:17-alpine','-e POSTGRES_PASSWORD=pw','ready to accept connections',"psql -U postgres -tAc 'select version()'",240)),
 ('php-fpm',['php:8.3-fpm-alpine'],lambda:run('php:8.3-fpm-alpine',b64(PHP),'test is successful')),
 ('ruby',['ruby:3.3-alpine'],lambda:run('ruby:3.3-alpine',b64(RUBY),'RUBY-OK')),
 ('django',['python:3.12'],lambda:run('python:3.12',b64(DJANGO),'The install worked',420)),
 ('dotnet-sdk',['mcr.microsoft.com/dotnet/sdk:8.0'],lambda:run('mcr.microsoft.com/dotnet/sdk:8.0',b64(DOTNET),'was created successfully',600,extra='-m 4g')),
 ('keycloak',['quay.io/keycloak/keycloak:26.0'],lambda:server('s3-keycloak','quay.io/keycloak/keycloak:26.0','start-dev','28101:8080','http://127.0.0.1:28101/realms/master','"realm":"master"','-m 2g -e KC_BOOTSTRAP_ADMIN_USERNAME=admin -e KC_BOOTSTRAP_ADMIN_PASSWORD=admin',300)),
 ('minio',['quay.io/minio/minio:latest'],lambda:server('s3-minio','quay.io/minio/minio:latest','server /data','28102:9000','http://127.0.0.1:28102/minio/health/live','')),
 ('n8n',['n8nio/n8n:latest'],lambda:server('s3-n8n','n8nio/n8n:latest','','28103:5678','http://127.0.0.1:28103/healthz','ok','',240)),
 ('ghost',['ghost:5-alpine'],lambda:server('s3-ghost','ghost:5-alpine','','28104:2368','http://127.0.0.1:28104/','Ghost','-e NODE_ENV=development -e url=http://127.0.0.1:28104',240)),
 ('sonarqube',['sonarqube:community'],lambda:server('s3-sonar','sonarqube:community','','28105:9000','http://127.0.0.1:28105/api/system/status','"status":"UP"','-m 4g -e SONAR_ES_BOOTSTRAP_CHECKS_DISABLE=true',600)),
 ('gitea',['gitea/gitea:1.22'],lambda:server('s3-gitea','gitea/gitea:1.22','','28106:3000','http://127.0.0.1:28106/','Gitea')),
 ('haproxy',['haproxy:alpine'],lambda:server('s3-haproxy','haproxy:alpine','','28107:8080','http://127.0.0.1:28107/','haproxy-ok',f'-v {D}/haproxy.cfg:/usr/local/etc/haproxy/haproxy.cfg:ro')),
 ('vault',['hashicorp/vault:1.17'],lambda:server('s3-vault','hashicorp/vault:1.17','server -dev -dev-listen-address=0.0.0.0:8200 -dev-root-token-id=root','28108:8200','http://127.0.0.1:28108/v1/sys/health','"initialized":true','-e SKIP_SETCAP=1')),
 ('influxdb',['influxdb:2'],lambda:server('s3-influx','influxdb:2','','28109:8086','http://127.0.0.1:28109/health','"status":"pass"')),
 ('clickhouse',['clickhouse/clickhouse-server:latest'],lambda:(lambda r:r if not r[0] else (lambda c:(c[0]==0 and 'ch-499500' in c[1],c[1][-200:]))(sh('docker exec s3-click '+b64(CLICK),120)))(server('s3-click','clickhouse/clickhouse-server:latest','','28110:8123','http://127.0.0.1:28110/ping','Ok.','-m 2g',180))),
 ('jenkins',['jenkins/jenkins:lts-jdk21'],lambda:server('s3-jenkins','jenkins/jenkins:lts-jdk21','','28111:8080','http://127.0.0.1:28111/login','Jenkins','-m 2g',300)),
 ('mosquitto',['eclipse-mosquitto:2'],mqtt),
 ('nats',['nats:alpine'],lambda:server('s3-nats','nats:alpine','-m 8222','28112:8222','http://127.0.0.1:28112/varz','server_id')),
 ('compose-wordpress',['wordpress:php8.3-apache','mysql:8.4'],lambda:compose('wp','http://127.0.0.1:28120/wp-admin/install.php','WordPress',300)),
 ('compose-lemp',['php:8.3-fpm-alpine','nginx:alpine'],lambda:compose('lemp','http://127.0.0.1:28121/index.php','PHP OK 8.3 fpm-fcgi')),
 ('compose-python-redis',['python:3.12-alpine','redis:7-alpine'],lambda:compose('pyredis','http://127.0.0.1:28122/','hits ')),
]
OUT=sys.argv[1];only=set(sys.argv[2:])
res=json.load(open(OUT)) if os.path.exists(OUT) else {}
for name,images,test in CASES:
    if only and name not in only: continue
    free=shutil.disk_usage(os.path.expanduser('~')).free
    if free<MIN_FREE: print(f'STOP disk free {free>>30} GiB < {MIN_FREE>>30} GiB',flush=True);break
    t=time.time();ok,detail=True,''
    for image in images:
        c,o=sh(f'docker pull {image}',1800)
        if c and sh(f'docker image inspect {image}',30)[0]: ok,detail=False,'pull: '+o[-400:];break   # a flaky pull of a local image is fine
    pulled=round(time.time()-t);t=time.time()
    if ok:
        try: ok,detail=test()
        except Exception as e: ok,detail=False,repr(e)
    res[name]={'ok':bool(ok),'pull_seconds':pulled,'seconds':round(time.time()-t),'detail':str(detail)[-900:]}
    print(('PASS ' if ok else 'FAIL ')+name,res[name]['seconds'],'s',('' if ok else res[name]['detail'][-400:].replace('\n',' | ')),flush=True)
    json.dump(res,open(OUT,'w'),indent=1)
    if not ok: continue                                    # keep a failing case's images for debugging
    for project in ('wp','lemp','pyredis'):
        if name.endswith(project.replace('wp','wordpress').replace('pyredis','python-redis')): sh(f'cd {D}/{project} && docker compose -p {project} down -v',180)
    _,names=sh("docker ps -a --format '{{.Names}}'",30)
    for n in names.split():
        if n.startswith('s3-'): sh(f'docker rm -f {n}',60)
    sh('docker volume prune -af',120)
    for image in images:
        if image not in KEEP: sh(f'docker rmi {image}',120)
