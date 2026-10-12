#!/usr/bin/env python3
"""Record standalone interpreter launch CPU/RSS/time; never claim whole-product savings."""
import argparse,hashlib,json,os,platform,resource,shutil,socket,statistics,subprocess,tempfile,threading,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def measured(command,limit):
    with tempfile.TemporaryFile() as out,tempfile.TemporaryFile() as err:
        start=time.perf_counter();child=subprocess.Popen(command,stdout=out,stderr=err,stdin=subprocess.DEVNULL)
        while True:
            pid,status,usage=os.wait4(child.pid,os.WNOHANG)
            if pid:break
            if time.perf_counter()-start>limit:
                child.kill();pid,status,usage=os.wait4(child.pid,0);child.returncode=os.waitstatus_to_exitcode(status);raise TimeoutError('Workload deadline exceeded')
            time.sleep(.002)
        child.returncode=os.waitstatus_to_exitcode(status);elapsed=time.perf_counter()-start
        out.seek(0);err.seek(0);stdout=out.read();stderr=err.read()
        if child.returncode or stderr:raise RuntimeError(dict(exit_code=child.returncode,stderr=stderr.decode(errors='replace')))
        return dict(wall_seconds=elapsed,cpu_user_seconds=usage.ru_utime,cpu_system_seconds=usage.ru_stime,
                    peak_rss_bytes=usage.ru_maxrss*(1 if platform.system()=='Darwin' else 1024)),stdout

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path,required=True);p.add_argument('--repetitions',type=int,default=8);a=p.parse_args()
    if not 2<=a.repetitions<=100:p.error('Use 2..100 repetitions')
    rows=[];payload=b'0123456789abcdef'*(1024*1024//16)
    for workload in ['shell-startup','fetch-1mib']:
        for iteration in range(a.repetitions):
            with tempfile.TemporaryDirectory() as temp:
                root=Path(temp)/'root';shutil.copytree(ROOT/'examples/m28/root',root)
                cmd=[str(a.native.resolve()),'run',str(root/'bin/busybox'),'--dynamic','--job-control','--scheduled-network','--root',str(root),'--writable','--max-steps','200000000','--']
                listener=None;thread=None;errors=[]
                if workload=='shell-startup':cmd+=['sh','-c','echo ready'];wanted=b'ready\n'
                else:
                    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(15)
                    def serve():
                        try:
                            with listener.accept()[0] as peer:
                                peer.settimeout(15);request=b''
                                while b'\r\n\r\n' not in request and len(request)<8192:
                                    chunk=peer.recv(4096)
                                    if not chunk:return
                                    request+=chunk
                                peer.sendall(b'HTTP/1.0 200 OK\r\nContent-Length: '+str(len(payload)).encode()+b'\r\n\r\n'+payload)
                        except OSError as e:errors.append(str(e))
                    thread=threading.Thread(target=serve,daemon=True);thread.start()
                    cmd+=['wget','-q','-O','/download','http://127.0.0.1:'+str(listener.getsockname()[1])+'/'];wanted=b''
                try:
                    measurement,out=measured(cmd,20)
                    if out!=wanted:raise RuntimeError('Unexpected output')
                    if workload=='fetch-1mib' and (root/'download').read_bytes()!=payload:raise RuntimeError('Payload differs')
                    rows.append(dict(workload=workload,iteration=iteration,**measurement))
                finally:
                    if listener:listener.close()
                    if thread:thread.join(16)
                if errors:raise RuntimeError(errors)
    summaries={}
    for workload in ['shell-startup','fetch-1mib']:
        samples=[r for r in rows if r['workload']==workload]
        summaries[workload]={metric:dict(median=statistics.median(r[metric] for r in samples),minimum=min(r[metric] for r in samples),maximum=max(r[metric] for r in samples)) for metric in ['wall_seconds','cpu_user_seconds','cpu_system_seconds','peak_rss_bytes']}
    report=dict(schema_version=1,host=platform.platform(),machine=platform.machine(),runtime_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest(),scope='standalone native process, including guest tasks; excludes server fixture, managed supervisors, Docker VM and battery',baseline=None,savings=None,production_feasibility_accepted=False,samples=rows,summary=summaries)
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n');print('Recorded',len(rows),'samples; no Docker or whole-product savings claim')
if __name__=='__main__':main()
