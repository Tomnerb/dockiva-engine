#!/usr/bin/env python3
"""Bounded Linux reference smoke tests; never a VM-free backend or fallback."""
import argparse,json,subprocess,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
IMAGES={'postgresql':'sha256:d394728dee24b7791f1969c683f8b8410fe57450713a1db2aff5a500f0f98ab0','redis':'sha256:3b0f54bf27a77c98d319dc3a6706ed500a8985309f0b6c6407599b869f4a2bd9','valkey':'sha256:d33b8c582a23d02e46689d5d98a19853e1ae9a1215595c68b64da4476911d67e'}
def docker(*args,check=True,timeout=40):
    p=subprocess.run(['docker',*args],capture_output=True,text=True,timeout=timeout)
    if check and p.returncode:raise RuntimeError(p.stderr[-2000:])
    return p
parser=argparse.ArgumentParser();parser.add_argument('--target',choices=list(IMAGES));args=parser.parse_args()
report=ROOT/'evidence/feasibility/linux-services.json'
rows=json.loads(report.read_text())['results'] if args.target and report.exists() else []
rows=[r for r in rows if r['database']!=args.target]
for label,image in IMAGES.items():
    if args.target and label!=args.target:continue
    name='vmfree-feasibility-'+uuid.uuid4().hex;volume=name+'-data';row={'database':label,'image_id':image,'checks':[]}
    def client(*args):
        cmd=['psql','-h','127.0.0.1','-U','postgres','-d','postgres','-At','-v','ON_ERROR_STOP=1','-c',*args] if label=='postgresql' else [label+'-cli','--raw',*args]
        return docker('exec','-e','PGPASSWORD=local-feasibility-only',name,*cmd,check=False,timeout=6)
    def ready():
        until=time.monotonic()+30
        while time.monotonic()<until:
            q=client('SELECT 1' if label=='postgresql' else 'PING')
            if q.returncode==0 and q.stdout.strip() in ('1','PONG'):return
            time.sleep(.25)
        raise RuntimeError('readiness timeout: '+docker('logs','--tail','30',name,check=False).stdout)
    def expect(expected,*args):
        q=client(*args);ok=q.returncode==0 and q.stdout.strip()==expected
        row['checks'].append({'request':list(args),'expected':expected,'stdout':q.stdout,'stderr':q.stderr,'exit_code':q.returncode,'passed':ok})
        if not ok:raise RuntimeError('Client mismatch')
    try:
        docker('volume','create',volume)
        data_path='/var/lib/postgresql/data' if label=='postgresql' else '/data'
        flags=['run','-d','--pull=never','--name',name,'--network=none','--read-only','--memory=384m','--cpus=1','--pids-limit=128','--mount','type=volume,src='+volume+',dst='+data_path,'--tmpfs','/tmp:rw,nosuid,nodev,size=64m']
        if label=='postgresql':flags+=['--tmpfs','/var/run/postgresql:rw,nosuid,nodev,mode=777,size=4m','-e','POSTGRES_PASSWORD=local-feasibility-only',image]
        else:flags += [image,label+'-server','--appendonly','yes','--appendfsync','always','--save','']
        docker(*flags);ready()
        row['trace_tool_available']=docker('exec',name,'sh','-c','command -v strace',check=False).returncode==0
        row['process_snapshot']=docker('top',name,'-eo','pid,ppid,comm').stdout
        row['main_process_status']=docker('exec',name,'sh','-c','cat /proc/1/status; ls /proc/1/fd').stdout
        if label=='postgresql':
            expect('CREATE TABLE\nINSERT 0 1','CREATE TABLE feasibility(k integer PRIMARY KEY,v text); INSERT INTO feasibility VALUES(1,\'persisted\');')
            request=('SELECT v FROM feasibility WHERE k=1',)
        else:expect('OK','SET','feasibility','persisted');request=('GET','feasibility')
        expect('persisted',*request)
        docker('stop','--time','10',name);docker('start',name);ready();expect('persisted',*request)
        docker('kill','--signal','KILL',name);docker('start',name);ready();expect('persisted',*request)
        row['result']='pass query, graceful restart and SIGKILL recovery for one acknowledged write'
    except Exception as error:row['error']=str(error);row['logs']=docker('logs','--tail','50',name,check=False).stdout
    finally:
        row['container_cleanup']=docker('rm','--force','--volumes',name,check=False).returncode
        row['volume_cleanup']=docker('volume','rm',volume,check=False).returncode
    rows.append(row);print(label,row.get('result',row.get('error')),flush=True)
    (ROOT/'evidence/feasibility/linux-services.json').write_text(json.dumps({'scope':'Stock pinned images on OrbStack Linux ARM64; no VM-free execution or Docker Desktop performance claim; no load qualification','results':rows},indent=2)+'\n')
