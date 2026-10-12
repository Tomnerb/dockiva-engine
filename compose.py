#!/usr/bin/env python3
"""Execute a validated JSON Compose subset through the VM-free Engine API."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import re
import socket
import urllib.parse

class UnixConnection(http.client.HTTPConnection):
    def __init__(self,path):super().__init__('localhost',timeout=35);self.path=path
    def connect(self):self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.sock.settimeout(self.timeout);self.sock.connect(self.path)

class Client:
    def __init__(self,home):
        data=json.loads((Path(home)/'connection.json').read_text());self.host=data['host'];self.headers=json.loads((Path(data['config'])/'config.json').read_text())['HttpHeaders']
    def request(self,method,path,body=None):
        if self.host.startswith('unix://'):conn=UnixConnection(self.host[7:])
        else:
            url=urllib.parse.urlsplit(self.host);conn=http.client.HTTPConnection(url.hostname,url.port,timeout=35)
        try:
            data=json.dumps(body).encode() if body is not None else None
            conn.request(method,'/v1.45'+path,data,{**self.headers,'Content-Type':'application/json'})
            response=conn.getresponse();raw=response.read()
            if response.status>=400:raise ValueError(f'API {response.status}: {raw.decode(errors="replace")}')
            if not raw:return None
            return json.loads(raw)
        finally:conn.close()

def load(path):
    # JSON is a YAML subset. Avoid an ad-hoc YAML parser or silent interpolation.
    model=json.loads(Path(path).read_text())
    if not isinstance(model,dict) or set(model)-{'name','services','version'}:raise ValueError('Unsupported top-level Compose fields')
    name=model.get('name','vmfree-demo')
    if not isinstance(name,str) or not re.fullmatch('[a-z][a-z0-9_-]{0,15}',name):raise ValueError('Invalid project name')
    services=model.get('services')
    if not isinstance(services,dict) or not 1<=len(services)<=8:raise ValueError('Expected 1..8 services')
    for service,c in services.items():
        if not re.fullmatch('[a-z][a-z0-9_-]{0,15}',service) or not isinstance(c,dict):raise ValueError('Invalid service')
        if set(c)-{'image','command','entrypoint','environment','depends_on','network_mode','read_only'}:raise ValueError('Unsupported options in service '+service)
        if c.get('network_mode')!='host':raise ValueError('Each service requires explicit network_mode: host')
        if not isinstance(c.get('image'),str):raise ValueError('Image name required')
        for field in ('command','entrypoint'):
            if field in c and (not isinstance(c[field],list) or not c[field] or not all(isinstance(x,str) for x in c[field])):raise ValueError(field+' must be a nonempty exec-form array')
        env=c.get('environment',{})
        if not isinstance(env,dict) or not all(isinstance(k,str) and isinstance(v,str) for k,v in env.items()):raise ValueError('Environment must be a string map')
        deps=c.get('depends_on',[])
        if not isinstance(deps,list) or any(d not in services for d in deps):raise ValueError('Invalid depends_on list')
        if 'read_only' in c and not isinstance(c['read_only'],bool):raise ValueError('read_only must be boolean')
    ordered=[];active=set()
    def visit(name):
        if name in active:raise ValueError('Dependency cycle')
        if name in ordered:return
        active.add(name)
        for d in services[name].get('depends_on',[]):visit(d)
        active.remove(name);ordered.append(name)
    for s in services:visit(s)
    return name,services,ordered

def execute(client,path,action):
    name,services,order=load(path)
    filters=urllib.parse.quote(json.dumps({'label':['com.docker.compose.project='+name]}))
    existing=client.request('GET','/containers/json?all=1&filters='+filters)
    by_service={c['Labels'].get('com.docker.compose.service'):c for c in existing}
    if action=='ps':return existing
    if action in ('stop','down'):
        for service in reversed(order):
            if service in by_service:
                ident=by_service[service]['Id'];client.request('POST','/containers/'+ident+'/stop')
                if action=='down':client.request('DELETE','/containers/'+ident)
        return {'project':name,'action':action}
    # Preflight all cached images/configs before changing any container.
    for service in order:client.request('GET','/images/'+urllib.parse.quote(services[service]['image'],safe='')+'/json')
    for service,item in by_service.items():
        if service not in services:raise ValueError('Project contains orphan services; resolve before up')
        expected=hashlib.sha256(json.dumps(services[service],sort_keys=True).encode()).hexdigest()
        if item['Labels'].get('com.docker.compose.config-hash')!=expected:raise ValueError('Service config changed; down before creating a replacement')
    created=[];started=[]
    try:
        for service in order:
            config=services[service];item=by_service.get(service)
            if item is None:
                body=dict(Image=config['image'],Labels={'com.docker.compose.project':name,'com.docker.compose.service':service,'com.docker.compose.config-hash':hashlib.sha256(json.dumps(config,sort_keys=True).encode()).hexdigest()},HostConfig=dict(NetworkMode='host',ReadonlyRootfs=config.get('read_only',False)),Env=[k+'='+v for k,v in config.get('environment',{}).items()])
                for field,wire in [('command','Cmd'),('entrypoint','Entrypoint')]:
                    if field in config:body[wire]=config[field]
                result=client.request('POST','/containers/create?name='+name+'-'+service,body);ident=result['Id'];created.append(ident)
            else:ident=item['Id']
            if item is None or item['State']!='running':client.request('POST','/containers/'+ident+'/start');started.append(ident)
    except BaseException:
        for ident in reversed(started):
            try:client.request('POST','/containers/'+ident+'/stop')
            except Exception:pass
        for ident in reversed(created):
            try:client.request('DELETE','/containers/'+ident)
            except Exception:pass
        raise
    return {'project':name,'services':order,'status':'started (application readiness not implied)'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--home',type=Path,default=Path.home()/'.vmfree');p.add_argument('-f','--file',type=Path,required=True);p.add_argument('action',choices=['up','stop','down','ps','config']);a=p.parse_args()
    if a.action=='config':load(a.file);print(a.file.read_text())
    else:print(json.dumps(execute(Client(a.home),a.file,a.action),indent=2))
if __name__=='__main__':
    try:main()
    except (OSError,ValueError) as e:raise SystemExit('compose error: '+str(e))
