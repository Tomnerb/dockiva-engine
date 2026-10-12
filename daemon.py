#!/usr/bin/env python3
"""Authenticated, local Docker Engine API subset for supported VM-free images."""
import argparse
from datetime import datetime, timezone
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import signal
import socketserver
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import containers
import oci
import registry
ROOT=Path(__file__).resolve().parent
def iso(timestamp):return datetime.fromtimestamp(timestamp,timezone.utc).isoformat().replace('+00:00','Z')

class APIError(Exception):
    def __init__(self,status,message):self.status=status;self.message=message

class Backend:
    def __init__(self,home,native):
        self.home=registry.private(home);self.state=self.home/'containers';self.cache=self.home/'images';self.native=str(Path(native).resolve(strict=True));self.mutex=threading.RLock()
    def cli(self,*args):
        p=subprocess.run([sys.executable,str(ROOT/'containers.py'),'--state-dir',str(self.state),'--native',self.native,*args],capture_output=True,timeout=20)
        if p.returncode:raise APIError(409,p.stderr.decode(errors='replace').strip())
        return p.stdout
    def all(self):return json.loads(self.cli('list'))
    def name(self,value):
        names=[r['name'] for r in self.all()]
        if value in names:return value
        matches=[n for n in names if len(value)>=12 and hashlib.sha256(n.encode()).hexdigest().startswith(value)]
        if len(matches)!=1:raise APIError(404,'No such container')
        return matches[0]
    def inspect(self,name):
        raw=json.loads(self.cli('inspect',name));c=raw['config'];s=raw['status'];ident=hashlib.sha256(name.encode()).hexdigest()
        status=s['state'];running=status in ('starting','running');status='dead' if status=='failed' else status
        image=c.get('image_name',c['image'])
        return dict(Id=ident,Name='/'+name,Created=iso(c['created_at']),Path=c['command'][0],Args=c['command'][1:],Image=c['image'],
            State=dict(Status=status,Running=running,Paused=False,Restarting=False,OOMKilled=False,Dead=status=='dead',Pid=s.get('pid',0) if running else 0,ExitCode=s.get('exit_code',0),Error=s.get('error',''),StartedAt=iso(s['started_at']) if 'started_at' in s else '0001-01-01T00:00:00Z',FinishedAt=iso(s['finished_at']) if 'finished_at' in s else '0001-01-01T00:00:00Z'),
            Config=dict(Image=image,Entrypoint=c['command'][:1],Cmd=c['command'][1:],Env=c['env'],Labels=c.get('labels',{}),Tty=False,WorkingDir='/',Hostname=name),
            HostConfig=dict(NetworkMode='host',ReadonlyRootfs=not c['writable'],RestartPolicy=dict(Name='no',MaximumRetryCount=0),LogConfig=dict(Type='json-file',Config={})),
            NetworkSettings=dict(Ports={},Networks={}),Mounts=[],Driver='vmfree-private-copy',Platform='linux',RestartCount=0)
    def image(self,name):
        try:image=oci.Image(registry.resolve(self.cache,name))
        except ValueError:raise APIError(404,'Image is not cached: '+name)
        return dict(Id=image.digest,RepoTags=[name],RepoDigests=[name.split(':')[0]+'@'+image.digest],Created=iso((registry.resolve(self.cache,name).parent/'image.json').stat().st_mtime),Size=sum(len(e[1]) for e in image.tree.values()),Architecture='arm64',Os='linux',Config=dict(Entrypoint=image.entry,Cmd=image.args,Env=image.env,WorkingDir='/'),RootFS=dict(Type='layers',Layers=[]))
    def create(self,name,body):
        if not isinstance(body,dict):raise APIError(400,'Expected config object')
        allowed={'Image','Cmd','Entrypoint','Env','Labels','HostConfig','WorkingDir','AttachStdout','AttachStderr','NetworkingConfig'}
        for k,v in body.items():
            if k not in allowed and v not in (None,False,'',[],{},0):raise APIError(501,'Unsupported container option: '+k)
        network=body.get('NetworkingConfig') or {}
        if not isinstance(network,dict) or set(network)-{'EndpointsConfig'}:raise APIError(501,'Unsupported NetworkingConfig')
        endpoints=network.get('EndpointsConfig') or {}
        if not isinstance(endpoints,dict) or set(endpoints)-{'host'}:raise APIError(501,'Only host network supported')
        settings=endpoints.get('host') or {}
        if not isinstance(settings,dict) or any(v not in (None,False,'',[],{},0) for v in settings.values()):raise APIError(501,'Host network options unsupported')
        host=body.get('HostConfig') or {}
        if not isinstance(host,dict):raise APIError(400,'Invalid HostConfig')
        for k,v in host.items():
            if k=='MemorySwappiness' and v==-1:continue
            if k=='ConsoleSize' and v==[0,0]:continue
            if k=='NetworkMode' and v in ('host',''):continue
            if k=='ReadonlyRootfs' and isinstance(v,bool):continue
            if k=='LogConfig' and isinstance(v,dict) and set(v)<= {'Type','Config'} and v.get('Type','') in ('','json-file') and v.get('Config') in (None,{}):continue
            if k=='RestartPolicy' and isinstance(v,dict) and v.get('Name','') in ('','no') and not v.get('MaximumRetryCount'):continue
            if v not in (None,False,'',[],{},0):raise APIError(501,'Unsupported HostConfig: '+k)
        if body.get('WorkingDir','') not in ('','/'):raise APIError(501,'Only guest working directory / supported')
        image=body.get('Image')
        if not isinstance(image,str) or not 1<=len(image)<=512:raise APIError(400,'Invalid image name')
        image_info=self.image(image)
        args=['create',name,str(registry.resolve(self.cache,image))]
        if image=='vmfree/redis:preview':
            profile=json.loads((ROOT/'examples/redis-preview/profile.json').read_text())
            if image_info['Id']!=profile['digest']:raise APIError(400,'Redis preview image does not match bundled profile')
            if host.get('ReadonlyRootfs'):raise APIError(400,'Redis preview requires its private writable root')
            args+=['--cpu-backend','hvf','--dynamic','--process-tree','--scheduled-network','--max-steps','1000000000']
        if host.get('ReadonlyRootfs'):args+=['--read-only']
        for key,flag in [('Cmd','--arg'),('Entrypoint','--entrypoint'),('Env','--env')]:
            values=body.get(key)
            if values is not None:
                if not isinstance(values,list) or len(values)>64 or not all(isinstance(v,str) and '\0' not in v for v in values):raise APIError(400,'Invalid '+key)
                if values==[] and key in ('Cmd','Entrypoint'):raise APIError(501,'Explicit empty '+key+' unsupported')
                for v in values:args+=[flag+'='+v]
        args+=['--labels-json',json.dumps(body.get('Labels') or {})]
        self.cli(*args)
        p=self.state/name/'config.json';c=containers.read(p);c['image_name']=image;containers.save(p,c)
        return dict(Id=hashlib.sha256(name.encode()).hexdigest(),Warnings=['VM-free prototype: host loopback networking; limited Linux compatibility'])

class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def setup(self):super().setup();self.connection.settimeout(10)
    def send(self,status,data=None,ctype='application/json'):
        raw=b'' if data is None else data if isinstance(data,bytes) else json.dumps(data).encode()
        self.send_response(status);self.send_header('Content-Type',ctype);self.send_header('Content-Length',str(len(raw)));self.send_header('API-Version','1.45');self.send_header('Docker-Experimental','true');self.end_headers()
        if self.command!='HEAD' and raw:self.wfile.write(raw)
    def do_HEAD(self):self.handle_api()
    def do_GET(self):self.handle_api()
    def do_POST(self):self.handle_api()
    def do_DELETE(self):self.handle_api()
    def handle_api(self):
        try:
            if not hmac.compare_digest(self.headers.get('X-VMFree-Token',''),self.server.token):raise APIError(401,'Authentication required')
            if self.headers.get('Transfer-Encoding'):raise APIError(400,'Chunked requests unsupported')
            length=int(self.headers.get('Content-Length','0'))
            if not 0<=length<=1024*1024:raise APIError(413,'Request exceeds 1 MiB')
            raw=self.rfile.read(length)
            if len(raw)!=length:raise APIError(400,'Truncated request')
            parsed=urllib.parse.urlsplit(self.path);path=urllib.parse.unquote(parsed.path);query=urllib.parse.parse_qs(parsed.query,keep_blank_values=True)
            version=re.match(r'^/v(\d+)\.(\d+)(/.*)$',path)
            if version:
                major,minor=int(version[1]),int(version[2])
                if major!=1 or not 24<=minor<=45:raise APIError(400,'Supported API range is 1.24–1.45')
                path=version[3]
            b=self.server.backend;method=self.command
            if path=='/_ping' and method in ('HEAD','GET'):return self.send(200,b'OK','text/plain')
            if path=='/version' and method=='GET':return self.send(200,dict(Version='0.17.0',ApiVersion='1.45',MinAPIVersion='1.24',Os='linux',Arch='arm64',GitCommit='prototype',GoVersion='not-go',KernelVersion='vmfree-user-mode',Experimental=True,Components=[]))
            if path=='/info' and method=='GET':return self.send(200,dict(ID='vmfree',Name='vmfree',ServerVersion='0.17.0',OSType='linux',Architecture='aarch64',OperatingSystem='VM-free prototype on '+sys.platform,Driver='private-copy',Containers=len(b.all()),NCPU=1,MemTotal=0,SecurityOptions=[],Warnings=['Not a production isolation boundary']))
            if path=='/containers/create' and method=='POST':
                if query.get('platform',['linux/arm64'])[0] not in ('','linux/arm64'):raise APIError(501,'Only linux/arm64 platform supported')
                name=query.get('name',[secrets.token_hex(12)])[0]
                with b.mutex:return self.send(201,b.create(name,json.loads(raw or b'{}')))
            if path=='/containers/json' and method=='GET':
                filters=json.loads(query.get('filters',['{}'])[0]);rows=[]
                if not isinstance(filters,dict) or set(filters)-{'label','name','status','id'}:raise APIError(501,'Unsupported container filter')
                for row in b.all():
                    item=b.inspect(row['name']);state=item['State'];labels=item['Config']['Labels'];ok=True
                    if query.get('all',['0'])[0] not in ('1','true') and not state['Running']:continue
                    for k,values in filters.items():
                        if not isinstance(values,(list,dict)):raise APIError(400,'Invalid filter values')
                        if k=='label':
                            for v in values:
                                key,sep,value=v.partition('=');ok=ok and key in labels and (not sep or labels[key]==value)
                        elif k=='name':ok=ok and any(v in row['name'] for v in values)
                        elif k=='status':ok=ok and state['Status'] in values
                        else:ok=ok and any(item['Id'].startswith(v) for v in values)
                    if ok:rows.append(dict(Id=item['Id'],Names=[item['Name']],Image=item['Config']['Image'],ImageID=item['Image'],Command=' '.join([item['Path'],*item['Args']]),Created=int(datetime.fromisoformat(item['Created'].replace('Z','+00:00')).timestamp()),State=state['Status'],Status=state['Status'],Labels=labels,Ports=[],HostConfig={'NetworkMode':'host'},NetworkSettings={'Networks':{}},Mounts=[]))
                return self.send(200,rows)
            if path=='/images/json' and method=='GET':
                cache=registry.private(b.cache);return self.send(200,[dict(b.image(json.loads(p.read_text())['name']),Created=int(p.stat().st_mtime)) for p in cache.glob('*/image.json')])
            if path.startswith('/images/') and path.endswith('/json') and method=='GET':return self.send(200,b.image(path[len('/images/'):-5]))
            if path=='/images/create' and method=='POST':
                image=query.get('fromImage',[''])[0];tag=query.get('tag',[''])[0]
                if tag and '@' not in image:image+=':'+tag
                registry.pull(image,b.cache)
                return self.send(200,(json.dumps({'status':'Downloaded verified image '+image})+'\n').encode(),'application/json')
            match=re.fullmatch(r'/containers/([^/]+)(?:/(json|start|stop|restart|logs|wait))?',path)
            if match:
                name=b.name(match[1]);action=match[2]
                if action=='json' and method=='GET':return self.send(200,b.inspect(name))
                if action in ('start','stop','restart') and method=='POST':
                    with b.mutex:
                        if action=='restart':b.cli('stop',name);b.cli('start',name)
                        else:b.cli(action,name)
                    return self.send(204)
                if action=='logs' and method=='GET':
                    if query.get('follow',['0'])[0] in ('true','1'):raise APIError(501,'Streaming follow unsupported')
                    if any(k in query for k in ('since','until')) and any(query[k][0] not in ('','0') for k in ('since','until') if k in query):raise APIError(501,'Time-filtered logs unsupported')
                    data=b''
                    for stream,flag in [(1,'stdout'),(2,'stderr')]:
                        if query.get(flag,['0'])[0] in ('1','true'):
                            log=b.cli('logs',name,*(['--stderr'] if stream==2 else []))
                            tail=query.get('tail',['all'])[0]
                            if tail!='all':
                                n=int(tail)
                                if n<0:raise APIError(400,'Invalid tail')
                                log=b''.join(log.splitlines(keepends=True)[-n:]) if n else b''
                            for at in range(0,len(log),16384):
                                chunk=log[at:at+16384];data+=struct.pack('>BxxxI',stream,len(chunk))+chunk
                    return self.send(200,data,'application/vnd.docker.raw-stream')
                if action=='wait' and method=='POST':
                    if query.get('condition',['not-running'])[0]!='not-running':raise APIError(501,'Only not-running wait supported')
                    deadline=time.monotonic()+30
                    while time.monotonic()<deadline:
                        item=b.inspect(name)
                        if not item['State']['Running']:return self.send(200,dict(StatusCode=item['State']['ExitCode'],Error=None))
                        time.sleep(.1)
                    raise APIError(408,'Wait exceeded prototype 30-second limit')
                if action is None and method=='DELETE':
                    if query.get('force',['0'])[0] in ('1','true'):b.cli('stop',name)
                    b.cli('rm',name);return self.send(204)
            raise APIError(501,'Unsupported endpoint: '+method+' '+path)
        except APIError as e:
            self.close_connection=True;self.send(e.status,dict(message=e.message))
        except (ValueError,TypeError,KeyError) as e:self.send(400,dict(message='Invalid request: '+str(e)))
        except (OSError,subprocess.TimeoutExpired):self.send(500,dict(message='Host operation failed; inspect local state'))

class UnixServer(socketserver.ThreadingMixIn,socketserver.UnixStreamServer):
    daemon_threads=True
    block_on_close=False

class TCPServer(ThreadingHTTPServer):
    daemon_threads=True
    request_queue_size=16

# Bound concurrent request processing, including authenticated wait requests.
class Bounded:
    def process_request(self,request,client_address):
        if not self.slots.acquire(blocking=False):request.close();return
        try:super().process_request(request,client_address)
        except BaseException:self.slots.release();raise
    def process_request_thread(self,*args):
        try:super().process_request_thread(*args)
        finally:self.slots.release()
class LocalTCP(Bounded,TCPServer):pass
class LocalUnix(Bounded,UnixServer):pass

def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--home',type=Path,default=Path.home()/'.vmfree');p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--tcp',type=int,help='Loopback-only port; 0 selects an ephemeral port');a=p.parse_args()
    backend=Backend(a.home,a.native)
    lock=os.open(backend.home/'daemon.lock',os.O_CREAT|os.O_RDWR,0o600)
    import fcntl
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    token=secrets.token_hex(32)
    endpoint=backend.home/'engine.sock'
    if a.tcp is None:
        endpoint.unlink(missing_ok=True);server=LocalUnix(str(endpoint),Handler);host='unix://'+str(endpoint)
    else:server=LocalTCP(('127.0.0.1',a.tcp),Handler);host='tcp://127.0.0.1:'+str(server.server_address[1])
    server.backend=backend;server.token=token;server.slots=threading.BoundedSemaphore(16)
    configdir=backend.home/'docker-client';configdir.mkdir(mode=0o700,exist_ok=True)
    containers.save(configdir/'config.json',dict(HttpHeaders={'X-VMFree-Token':token}))
    containers.save(backend.home/'connection.json',dict(host=host,config=str(configdir),api_version='1.45'))
    print('API '+host,flush=True)
    def stop(signum,frame):threading.Thread(target=server.shutdown,daemon=True).start()
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    try:server.serve_forever(poll_interval=.1)
    finally:
        server.server_close();endpoint.unlink(missing_ok=True);os.close(lock)
if __name__=='__main__':main()
