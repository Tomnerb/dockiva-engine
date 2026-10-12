#!/usr/bin/env python3
"""Pinned unmodified wget: HTTP bodies, errors, timeout, cancellation and OCI."""
import argparse,hashlib,json,shutil,socket,subprocess,tempfile,threading,time,signal,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path,required=True);p.add_argument('--host-sandbox',action='store_true');a=p.parse_args();rows=[]
class Server:
    def __init__(self,mode):
        self.mode=mode;self.socket=socket.socket();self.socket.bind(('127.0.0.1',0));self.socket.listen(4);self.socket.settimeout(6)
        self.port=self.socket.getsockname()[1];self.connected=threading.Event();self.stop=threading.Event();self.error=None
        self.thread=threading.Thread(target=self.serve,daemon=True);self.thread.start()
    def serve(self):
        try:
            for i in range(2 if self.mode in ['redirect','twice'] else 1):
                with self.socket.accept()[0] as c:
                    c.settimeout(3);data=b''
                    while b'\r\n\r\n' not in data and len(data)<8192:
                        chunk=c.recv(4096)
                        if not chunk:return
                        data+=chunk
                    self.connected.set()
                    if self.mode in ['timeout','cancel','crash']:self.stop.wait(4);return
                    if self.mode=='redirect' and i==0:c.sendall(b'HTTP/1.0 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n');continue
                    if self.mode=='not-found':c.sendall(b'HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n');return
                    if self.mode=='chunked':c.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n');return
                    if self.mode=='truncated':c.sendall(b'HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nabc');return
                    c.sendall(b'HTTP/1.0 200 OK\r\nContent-Length: 6\r\n\r\n')
                    for b in [b'abc',b'def']:
                        if self.mode=='slow':time.sleep(.08)
                        c.sendall(b)
        except (OSError,TimeoutError) as e:
            if not self.stop.is_set():self.error=str(e)
    def close(self):self.stop.set();self.socket.close();self.thread.join(7)
for mode in ['body','slow','chunked','redirect','not-found','truncated','timeout','cancel','crash']:
    server=Server(mode);child=None
    try:
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)/'root';shutil.copytree(ROOT/'examples/m28/root',root)
            cmd=[str(a.native.resolve()),'run',str(root/'bin/busybox'),'--dynamic','--job-control','--scheduled-network','--root',str(root),'--writable','--max-steps','20000000','--','wget','-q','-T','1','-O','/download','http://127.0.0.1:'+str(server.port)+'/']
            if a.host_sandbox:cmd=[sys.executable,str(ROOT/'confined_exec.py'),'--seatbelt',*cmd]
            start=time.monotonic();child=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if mode in ['cancel','crash']:
                if not server.connected.wait(5):raise RuntimeError('No HTTP connection for cancellation')
                child.send_signal(signal.SIGTERM if mode=='cancel' else signal.SIGKILL)
            out,err=child.communicate(timeout=8);elapsed=time.monotonic()-start
            data=(root/'download').read_bytes() if (root/'download').exists() else None
            if mode in ['body','slow','chunked','redirect']:good=child.returncode==0 and data==b'abcdef' and not err
            elif mode=='timeout':good=child.returncode==1 and .8<=elapsed<3 and b'timed out' in err
            elif mode=='cancel':good=child.returncode==143
            elif mode=='crash':good=child.returncode==-signal.SIGKILL
            else:good=child.returncode==1 and bool(err)
            rows.append(dict(name=mode,passed=good and not out,exit_code=child.returncode,elapsed=elapsed,data_hex=None if data is None else data.hex(),stderr=err.decode()))
            print(('PASS ' if rows[-1]['passed'] else 'FAIL ')+mode,flush=True)
            if not rows[-1]['passed']:print(rows[-1],flush=True)
    finally:
        if child is not None and child.poll() is None:child.kill();child.wait()
        server.close()
server=Server('body')
try:
    r=subprocess.run([sys.executable,str(ROOT/'oci.py'),'run',str(ROOT/'examples/m29/fetch-image'),'--native',str(a.native.resolve()),*(['--host-sandbox'] if a.host_sandbox else []),'--dynamic','--job-control','--scheduled-network','--max-steps','20000000','--','-q','-O','-','http://127.0.0.1:'+str(server.port)+'/'],capture_output=True,timeout=10)
    rows.append(dict(name='oci_fetch_stdout',passed=r.returncode==0 and r.stdout==b'abcdef' and not r.stderr,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
    print(('PASS ' if rows[-1]['passed'] else 'FAIL ')+rows[-1]['name'],flush=True)
finally:server.close()
server=Server('twice')
try:
    with tempfile.TemporaryDirectory() as temp:
        state=Path(temp)/'state'
        def container(*args):
            r=subprocess.run([sys.executable,str(ROOT/'containers.py'),'--state-dir',str(state),'--native',str(a.native.resolve()),*args],capture_output=True,timeout=15)
            if r.returncode:raise RuntimeError(r.stderr.decode())
            return r
        container('create','fetch',str(ROOT/'examples/m29/fetch-image'),*(['--host-sandbox'] if a.host_sandbox else []),'--dynamic','--job-control','--scheduled-network','--max-steps','20000000','--arg=-q','--arg=-O','--arg=/download','--arg=http://127.0.0.1:'+str(server.port)+'/')
        for iteration in range(2):
            container('start','fetch');deadline=time.monotonic()+10
            while True:
                info=json.loads(container('inspect','fetch').stdout)
                if info['status']['state'] in ['exited','failed']:break
                if time.monotonic()>deadline:raise RuntimeError('Managed fetch did not exit')
                time.sleep(.03)
            good=info['status']['exit_code']==0 and (state/'fetch/rootfs/download').read_bytes()==b'abcdef'
            rows.append(dict(name='managed_fetch_'+str(iteration),passed=good))
            print(('PASS ' if good else 'FAIL ')+rows[-1]['name'],flush=True)
        container('rm','fetch');rows.append(dict(name='managed_removed',passed=not (state/'fetch').exists()))
finally:server.close()
a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(dict(passed=all(x['passed'] for x in rows),checks=rows,runtime_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest()),indent=2)+'\n')
raise SystemExit(0 if all(x['passed'] for x in rows) else 1)
