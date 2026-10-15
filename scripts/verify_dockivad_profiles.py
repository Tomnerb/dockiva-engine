#!/usr/bin/env python3
"""Packaged profile negotiation and backend replacement regression; development only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from verify_dockivad import UnixHTTP, until

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args();helper=args.helper.resolve(strict=True)
    report={'helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),'checks':[]}
    def check(name,condition=True):
        assert condition,name
        report['checks'].append(name);print('PASS',name,flush=True)
    with tempfile.TemporaryDirectory(prefix='dv-prof-',dir='/tmp') as tmp:
        home=Path(tmp);native=home/'native';native.mkdir()
        for name in ('dockiva-runtime','dockiva-hvf'):
            shutil.copy2(helper.parent/'_internal/build'/name,native/name)
        subprocess.run([str(helper),'--home',str(home),'--import-name','alias/prepared:renamed',
                        '--import-layout',str(helper.parent/'_internal/examples/redis-preview/image')],check=True,capture_output=True)
        def api(method,path,body=None):
            token=json.loads((home/'docker-client/config.json').read_text())['HttpHeaders']['X-VMFree-Token']
            conn=UnixHTTP(home/'engine.sock')
            try:
                conn.request(method,'/v1.45'+path,json.dumps(body) if body is not None else None,
                             {'X-VMFree-Token':token,'Content-Type':'application/json'})
                response=conn.getresponse();return response.status,response.read()
            finally:conn.close()
        def expect(method,path,code,body=None):
            status,raw=api(method,path,body);assert status==code,(method,path,status,raw)
            return raw
        def launch():
            with (home/'engine.log').open('ab') as log:
                p=subprocess.Popen([str(helper),'--home',str(home),'--native',str(native/'dockiva-runtime')],
                                   stdout=log,stderr=log,env=dict(os.environ,PATH='/usr/bin:/bin'))
            def ready():
                if p.poll() is not None:raise AssertionError((home/'engine.log').read_text())
                return api('GET','/_ping')[0]==200
            try:until(ready);return p
            except BaseException:p.kill();p.wait();raise
        engine=launch()
        try:
            for binary,backend in [('dockiva-runtime','interpreter'),('dockiva-hvf','hvf')]:
                caps=json.loads(subprocess.check_output([str(native/binary),'capabilities']))
                check('packaged native handshake '+backend,caps['schema_version']==1 and backend in caps['backends'])
            expect('POST','/containers/create?name=readonly',400,
                   {'Image':'alias/prepared:renamed','Cmd':['--version'],'HostConfig':{'ReadonlyRootfs':True}})
            check('alias requires writable root before state publication',not (home/'containers/readonly').exists())
            expect('POST','/containers/create?name=alias',201,{'Image':'alias/prepared:renamed','Cmd':['--version']})
            info=json.loads(expect('GET','/containers/alias/json',200))['DockivaCompatibility']
            check('alias inspection exposes digest profile and experimental scope',
                  info['Backend']=='hvf' and info['Status']=='EXPERIMENTAL' and
                  info['ProfileID']=='prepared-redis-7.4.2-native' and info['RuntimeABI']=='dockiva-native-v1')
            before=(home/'containers/alias/config.json').read_bytes()
            # Simulate an incorrectly packaged/replaced backend without modifying the real bundle.
            replacement=native/'replacement';shutil.copy2(native/'dockiva-runtime',replacement)
            replacement.replace(native/'dockiva-hvf')
            failure=expect('POST','/containers/alias/start',409)
            check('missing required backend fails without interpreter fallback',b'required backend: hvf' in failure)
            state=json.loads((home/'containers/alias/status.json').read_text())
            check('failed negotiation launches no guest worker',state['state']=='created' and not (home/'containers/alias/control.json').exists())
            shutil.copy2(helper.parent/'_internal/build/dockiva-hvf',replacement)
            replacement.replace(native/'dockiva-hvf')
            expect('POST','/containers/alias/start',204)
            result=json.loads(expect('POST','/containers/alias/wait',200))
            logs=expect('GET','/containers/alias/logs?stdout=1&stderr=1',200)
            check('restored backend runs renamed prepared Redis',result['StatusCode']==0 and b'Redis server v=7.4.2' in logs)
            engine.terminate();engine.wait(5);engine=launch()
            after=json.loads(expect('GET','/containers/alias/json',200))['DockivaCompatibility']
            check('daemon reconnect preserves exact selected profile',after==info and (home/'containers/alias/config.json').read_bytes()==before)
            config=json.loads(before);config['max_steps']=42
            (home/'containers/alias/config.json').write_text(json.dumps(config))
            failure=expect('POST','/containers/alias/start',400)
            check('saved launch-policy drift rejected before restart',b'disagrees with profile: max_steps' in failure)
            (home/'containers/alias/config.json').write_bytes(before)
            expect('DELETE','/containers/alias?force=1',204)
            check('restored policy remains removable')
        finally:
            for path in (home/'containers').glob('*'):
                if path.is_dir() and (path/'status.json').exists():
                    try:api('DELETE',f'/containers/{path.name}?force=1')
                    except (OSError,ValueError):pass
            if engine.poll() is None:
                engine.terminate()
                try:engine.wait(5)
                except subprocess.TimeoutExpired:engine.kill();engine.wait()
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
