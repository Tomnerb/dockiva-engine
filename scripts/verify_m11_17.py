#!/usr/bin/env python3
"""Combined release acceptance; optional real Docker/Compose and Linux references."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import hashlib
import http.client as http_client
import json
import os
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import platform
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
import oci,registry,compose

def receive(sock):
    data=b''
    while True:
        chunk=sock.recv(8192)
        if not chunk:return data
        data+=chunk

def http(port,path='/'):
    with socket.create_connection(('127.0.0.1',port),timeout=5) as s:
        s.sendall(('GET '+path+' HTTP/1.1\r\nHost: localhost\r\n\r\n').encode());return receive(s)

def ready(proc,prefix=b'PORT '):
    with selectors.DefaultSelector() as select:
        select.register(proc.stdout,selectors.EVENT_READ)
        if not select.select(10):raise RuntimeError('Readiness timeout')
        data=proc.stdout.readline()
        if not data.startswith(prefix):raise RuntimeError('Unexpected readiness: '+repr(data)+' '+proc.stderr.read().decode(errors='replace'))
        return data

def freeport():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path);p.add_argument('--docker');p.add_argument('--compose');p.add_argument('--qemu');a=p.parse_args()
    native=str(a.native.resolve());d=ROOT/'examples/m11_17';rows=[];processes=[]
    def check(name,passed,**detail):
        rows.append(dict(name=name,passed=bool(passed),**detail));print(('PASS ' if passed else 'FAIL ')+name,flush=True)
    def run(command,**kwargs):return subprocess.run(command,capture_output=True,timeout=40,**kwargs)
    report=dict(host=platform.platform(),external_gates=dict(mac_signed_install='not-run',independent_security_review='not-run',same_mac_vm_benchmark='not-run'),cases=rows)
    try:
        m=json.loads((d/'build-manifest.json').read_text());check('fixture_hashes',all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==h for n,h in m['sha256'].items()))
        r=run([native,'run',str(d/'net-check.elf')]);check('poll_fcntl_errors',r.returncode==0 and not r.stdout and not r.stderr)
        if a.qemu:
            r=run([a.qemu,str(d/'net-check.elf')]);check('linux_network_reference',r.returncode==0 and not r.stdout and not r.stderr)
        with tempfile.TemporaryDirectory(prefix='vmfree-m17-') as temp:
            base=Path(temp);tree=base/'root';tree.mkdir();oci.Image(d/'http-image').extract(tree)
            proc=subprocess.Popen([native,'run',str(tree/'app/server'),'--root',str(tree),'--writable','--max-steps','10000000','--','0'],stdout=subprocess.PIPE,stderr=subprocess.PIPE);processes.append(proc)
            port=int(ready(proc)[5:]);body=(d/'index.html').read_bytes()
            check('repeated_http',all(http(port).endswith(body) for _ in range(5)))
            with ThreadPoolExecutor(max_workers=6) as pool:responses=list(pool.map(lambda _:http(port),range(12)))
            check('concurrent_http',all(r.endswith(body) for r in responses))
            with socket.create_connection(('127.0.0.1',port),timeout=5) as slow:
                slow.sendall(b'GET / HTTP/1.1\r\nHost:')
                check('slow_client_does_not_block_other',http(port).endswith(body))
                slow.sendall(b' localhost\r\n\r\n');check('fragmented_http',receive(slow).endswith(body))
            with socket.create_connection(('127.0.0.1',port),timeout=5):pass
            check('disconnect_recovery',http(port).endswith(body))
            check('invalid_request',b'400 Bad Request' in http(port,'/missing'))
            check('server_state_file',(tree/'counter').read_bytes()==b'1')
            proc.terminate();proc.wait(timeout=5);check('persistent_server_stop',proc.returncode==-15 and not proc.stderr.read())
            # A bounded local Distribution API fixture with real HTTP requests.
            layout=d/'http-image';idx=json.loads((layout/'index.json').read_text());desc=idx['manifests'][0];manifest=(layout/'blobs/sha256'/desc['digest'][7:]).read_bytes()
            class Registry(BaseHTTPRequestHandler):
                def log_message(self,*args):pass
                def do_GET(self):
                    parts=self.path.split('/');raw=None
                    if '/manifests/' in self.path:
                        if parts[-1]=='multi':raw=json.dumps(dict(schemaVersion=2,mediaType=registry.INDEX,manifests=[dict(desc,platform={'os':'linux','architecture':'arm64'})])).encode()
                        else:raw=manifest
                    elif '/blobs/' in self.path:
                        digest=parts[-1]
                        if oci.DIGEST.fullmatch(digest):
                            path=layout/'blobs/sha256'/digest[7:]
                            if path.exists():raw=path.read_bytes()
                        if self.server.corrupt and raw:raw=raw[:-1]+bytes([raw[-1]^1])
                    if raw is None:self.send_error(404);return
                    self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.send_header('Docker-Content-Digest',oci.sha(raw));self.end_headers();self.wfile.write(raw)
            server=ThreadingHTTPServer(('127.0.0.1',0),Registry);server.corrupt=False
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            cache=base/'pull-cache';ref='127.0.0.1:'+str(server.server_address[1])+'/demo/server'
            try:
                path=registry.pull(ref+':latest',cache,True);check('registry_tag_pull',oci.Image(path).digest==desc['digest'])
                path=registry.pull(ref+'@'+desc['digest'],cache,True);check('registry_digest_pull',oci.Image(path).digest==desc['digest'])
                path=registry.pull(ref+':multi',cache,True);check('registry_arm64_selection',oci.Image(path).digest==desc['digest'])
                check('verified_cache_reuse',registry.pull(ref+':latest',cache,True)==registry.resolve(cache,ref+':latest'))
                server.corrupt=True
                try:registry.pull(ref+':bad',cache,True);rejected=False
                except ValueError:rejected=True
                check('registry_corrupt_blob_rejected',rejected and not (cache/registry.key(ref+':bad')).exists())
                try:registry.pull('example.com/demo:test',cache,True);rejected=False
                except ValueError:rejected=True
                check('remote_plain_http_rejected',rejected)
            finally:server.shutdown();server.server_close();thread.join()
            imported=base/'import';shutil.copytree(layout,imported);secret=base/'secret';secret.write_text('not image content');(imported/'extra').symlink_to(secret)
            local=registry.commit(cache,'test:import',imported);check('import_ignores_unreferenced_links',not (local/'extra').exists())
            limits=run([sys.executable,str(ROOT/'confined_exec.py'),sys.executable,'-c','import resource,json;print(json.dumps([resource.getrlimit(x) for x in (resource.RLIMIT_CORE,resource.RLIMIT_FSIZE,resource.RLIMIT_NOFILE,resource.RLIMIT_CPU)]))'])
            check('host_resource_limits',limits.returncode==0 and json.loads(limits.stdout)==[[0,0],[16777216,16777216],[128,128],[60,60]])
            home=base/'engine';home.mkdir(mode=0o700);registry.commit(home/'images','dockiva/server:demo',layout)
            api=subprocess.Popen([sys.executable,str(ROOT/'daemon.py'),'--home',str(home),'--native',native,'--tcp','0'],stdout=subprocess.PIPE,stderr=subprocess.PIPE);processes.append(api)
            ready(api,b'API ');client=compose.Client(home)
            info=json.loads((home/'connection.json').read_text());url=urllib.parse.urlsplit(info['host'])
            conn=http_client.HTTPConnection(url.hostname,url.port,timeout=5);conn.request('GET','/version');resp=conn.getresponse();resp.read();check('api_authentication',resp.status==401);conn.close()
            check('api_version',client.request('GET','/version')['ApiVersion']=='1.45')
            check('api_image_inspect',client.request('GET','/images/vmfree%2Fserver%3Ademo/json')['Architecture']=='arm64')
            def rejects(path,body):
                try:client.request('POST',path,body);return False
                except ValueError as e:return '501' in str(e)
            check('api_privileged_rejected',rejects('/containers/create?name=bad',dict(Image='dockiva/server:demo',HostConfig={'Privileged':True})))
            check('api_port_remapping_rejected',rejects('/containers/create?name=bad',dict(Image='dockiva/server:demo',HostConfig={'PortBindings':{'80/tcp':[{'HostPort':'8080'}]}})))
            check('api_wrong_platform_rejected',rejects('/containers/create?name=bad&platform=linux/amd64',dict(Image='dockiva/server:demo')))
            model=json.loads((d/'compose.json').read_text());back,front=freeport(),freeport()
            while front==back:front=freeport()
            model['services']['backend']['command']=[str(back)];model['services']['frontend']['command']=[str(front),str(back)]
            composefile=base/'compose.json';composefile.write_text(json.dumps(model))
            try:
                compose.execute(client,composefile,'up');check('compose_create_start',len(compose.execute(client,composefile,'ps'))==2)
                deadline=time.monotonic()+10
                while True:
                    try:response=http(front)
                    except OSError:response=b''
                    if response.endswith(body) or time.monotonic()>deadline:break
                    time.sleep(.05)
                check('compose_frontend_backend',response.endswith(body))
                compose.execute(client,composefile,'stop');check('compose_stop',all(r['State']=='exited' for r in compose.execute(client,composefile,'ps')))
                compose.execute(client,composefile,'up');time.sleep(.3)
                check('compose_restart_persistence',(home/'containers/vmfree-demo-backend/rootfs/counter').read_bytes()==b'2')
                compose.execute(client,composefile,'down');check('compose_remove',not compose.execute(client,composefile,'ps'))
                bad=dict(model,services={'bad':dict(model['services']['backend'],volumes=['x:/data'])});f=base/'invalid.json';f.write_text(json.dumps(bad))
                try:compose.load(f);rejected=False
                except ValueError:rejected=True
                check('compose_unsupported_volume_rejected',rejected)
                if a.docker:
                    env={**os.environ,'DOCKER_API_VERSION':'1.45'};env.pop('DOCKER_CONTEXT',None)
                    for name,args in [('version',['version']),('images',['images']),('create',['create','--network','host','--name','cli','dockiva/server:demo','0']),('start',['start','cli']),('ps',['ps','-a']),('inspect',['inspect','cli']),('logs',['logs','cli']),('stop',['stop','cli']),('rm',['rm','cli'])]:
                        r=run([a.docker,'--config',info['config'],'--host',info['host'],*args],env=env);check('docker_'+name,r.returncode==0,stderr=r.stderr.decode(errors='replace'))
                if a.compose:
                    env={**os.environ,'DOCKER_API_VERSION':'1.45','DOCKER_HOST':info['host'],'DOCKER_CONFIG':info['config']};env.pop('DOCKER_CONTEXT',None)
                    for name,args in [('up',['up','-d']),('ps',['ps','-a'])]:
                        r=run([a.compose,'-f',str(composefile),*args],env=env);check('docker_compose_'+name,r.returncode==0,stderr=r.stderr.decode(errors='replace'))
                    check('docker_compose_service_traffic',http(front).endswith(body))
                    for name,args in [('stop',['stop']),('restart',['up','-d']),('down',['down'])]:
                        r=run([a.compose,'-f',str(composefile),*args],env=env);check('docker_compose_'+name,r.returncode==0,stderr=r.stderr.decode(errors='replace'))
            finally:
                for item in client.request('GET','/containers/json?all=1'):
                    try:client.request('DELETE','/containers/'+item['Id']+'?force=1')
                    except Exception:pass
                api.terminate();api.wait(timeout=5)
        report['passed']=all(r['passed'] for r in rows)
    except Exception as e:
        check('verification_error',False,error=str(e));report['passed']=False
    finally:
        for proc in processes:
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
            proc.stdout.close();proc.stderr.close()
        report['optional_tools']=dict(docker=a.docker or 'not-run',compose=a.compose or 'not-run',qemu=a.qemu or 'not-run')
        if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1
if __name__=='__main__':sys.exit(main())
