from pathlib import Path
import tempfile,shutil,subprocess,socket,selectors,json
import argparse
p=argparse.ArgumentParser()
p.add_argument('--qemu',required=True)
a=p.parse_args()
project=Path(__file__).resolve().parents[1]
results=[]
with tempfile.TemporaryDirectory() as temp:
    root=Path(temp)
    (root/'www').mkdir()
    shutil.copy(project/'examples/m11_17/index.html',root/'www/index.html')
    shutil.copy(project/'examples/m11_17/server.elf',root/'server')
    for wildcard in (False,True):
        args=[a.qemu,'-L',str(root),str(root/'server'),'0','0']+(['--bind-any'] if wildcard else [])
        proc=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            with selectors.DefaultSelector() as sel:
                sel.register(proc.stdout,selectors.EVENT_READ)
                assert sel.select(5), 'No readiness line'
            line=proc.stdout.readline(); assert line.startswith(b'PORT '), (line,proc.wait(),proc.stderr.read())
            port=int(line[5:])
            host='127.0.0.2' if wildcard else '127.0.0.1'
            with socket.create_connection((host,port),timeout=2) as s:
                s.sendall(b'GET / HTTP/1.1\r\nHost: benchmark\r\n\r\n')
                data=b''
                while True:
                    chunk=s.recv(8192)
                    if not chunk:break
                    data+=chunk
                assert b'200 OK' in data and b'Persistent VM-free HTTP server OK' in data,data
            if not wildcard:
                try:
                    socket.create_connection(('127.0.0.2',port),timeout=1).close()
                except ConnectionRefusedError:pass
                else:raise AssertionError('Default unexpectedly reachable on another address')
            results.append({'name':'linux_wildcard_http' if wildcard else 'linux_loopback_only','passed':True})
        finally:
            proc.terminate();proc.wait(timeout=5)
    proc=subprocess.run([str(project/'build/vmfree-c'),'run',str(root/'server'),'--root',str(root),'--max-steps','10000000','--','0','0','--bind-any'],capture_output=True,timeout=5)
    assert proc.returncode==74,(proc.returncode,proc.stderr)
    results.append({'name':'native_wildcard_still_rejected','passed':True})
print(json.dumps(results,indent=2))
