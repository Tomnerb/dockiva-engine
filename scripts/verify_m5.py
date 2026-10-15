#!/usr/bin/env python3
"""Exercise a real guest HTTP server over localhost and network error probes."""
import argparse,hashlib,json,os,platform,selectors,socket,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def serve(command,parts,expected_status=0):
    proc=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    ready=b'';response=b''
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(proc.stdout,selectors.EVENT_READ);deadline=time.monotonic()+12
            while b'\n' not in ready:
                if time.monotonic()>deadline:raise RuntimeError('Server did not announce a port')
                if not selector.select(0.2):continue
                chunk=os.read(proc.stdout.fileno(),128)
                if not chunk:raise RuntimeError('Server exited before readiness: '+proc.stderr.read().decode(errors='replace'))
                ready+=chunk
        if not ready.startswith(b'PORT '):raise RuntimeError('Unexpected readiness: '+repr(ready))
        port=int(ready.splitlines()[0][5:])
        with socket.create_connection(('127.0.0.1',port),timeout=8) as client:
            client.settimeout(8)
            for part in parts:
                client.sendall(part)
                if len(parts)>1:time.sleep(0.02)
            if not parts:client.shutdown(socket.SHUT_WR)
            while True:
                chunk=client.recv(4096)
                if not chunk:break
                response+=chunk
        out,err=proc.communicate(timeout=8)
        return dict(response_hex=response.hex(),exit_code=proc.returncode,stderr_hex=err.hex(),passed=proc.returncode==expected_status and not err and not out)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=2)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
        proc.stdout.close();proc.stderr.close()

def verify(native,reference):
    d=ROOT/'examples/m5';m=json.loads((d/'build-manifest.json').read_text())
    for name,digest in m['sources'].items():
        if hashlib.sha256((d/name).read_bytes()).hexdigest()!=digest:raise ValueError('Source hash mismatch: '+name)
    for f in m['files']:
        if hashlib.sha256((d/f['name']).read_bytes()).hexdigest()!=f['sha256']:raise ValueError('ELF hash mismatch')
    responses=json.loads((d/'responses.json').read_text());rows=[]
    requests=[('http_root',[b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n'],'ok',0),
              ('http_missing',[b'GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n'],'missing',0),
              ('http_method',[b'POST / HTTP/1.1\r\nHost: localhost\r\n\r\n'],'method',0),
              ('http_fragmented',[b'GET / HT',b'TP/1.1\r\nHost: local',b'host\r\n\r\n'],'ok',0),
              ('http_large_headers',[b'A'*2048],'too_large',0),
              ('client_disconnect',[],None,4)]
    for name,parts,key,status in requests:
        elf=str(d/'server.elf');actual=serve([str(native),'run',elf,'--','auto'],parts,status)
        expected=responses[key].encode().hex() if key else ''
        row=dict(name=name,actual=actual,passed=actual['passed'] and actual['response_hex']==expected)
        if reference is not None:
            ref=serve([*reference,elf,'auto'],parts,status);row['linux_reference']=ref
            row['passed']=row['passed'] and ref['passed'] and ref['response_hex']==expected
        rows.append(row)
    for f in m['files'][1:]:
        name=Path(f['name']).stem;path=str(d/f['name'])
        actual=subprocess.run([str(native),'run',path],capture_output=True,timeout=10)
        row=dict(name=name,passed=actual.returncode==0 and not actual.stdout and not actual.stderr,exit_code=actual.returncode,stderr_hex=actual.stderr.hex())
        # Bad read pointers on unconnected sockets have host-specific error ordering.
        comparable=name in ('invalid_fd','close_twice','bind_bad_pointer')
        row['scope']='linux-comparison' if comparable else 'runtime-policy'
        if reference is not None and comparable:
            ref=subprocess.run([*reference,path],capture_output=True,timeout=10)
            row['linux_reference']=dict(exit_code=ref.returncode,stdout_hex=ref.stdout.hex(),stderr_hex=ref.stderr.hex())
            row['passed']=row['passed'] and ref.returncode==0 and not ref.stdout and not ref.stderr
        rows.append(row)
    return dict(host=platform.platform(),passed=all(r['passed'] for r in rows),cases=rows)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime');p.add_argument('--report',type=Path)
    g=p.add_mutually_exclusive_group();g.add_argument('--qemu');g.add_argument('--native-linux',action='store_true');a=p.parse_args()
    if a.qemu and platform.system()!='Linux':p.error('QEMU reference requires Linux')
    if a.native_linux and (platform.system()!='Linux' or platform.machine() not in ('aarch64','arm64')):p.error('Native reference requires ARM64 Linux')
    try:
        r=verify(a.native.resolve(),[] if a.native_linux else [a.qemu] if a.qemu else None)
        r['reference_kind']='native-arm64-linux' if a.native_linux else 'qemu-user-linux' if a.qemu else 'not-run'
        for row in r['cases']:print(('PASS ' if row['passed'] else 'FAIL ')+row['name'])
        print('Linux reference: '+r['reference_kind'])
        if a.report:a.report.write_text(json.dumps(r,indent=2)+'\n')
        return 0 if r['passed'] else 1
    except (OSError,ValueError,RuntimeError,subprocess.TimeoutExpired) as exc:print('Verification failed: '+str(exc),file=sys.stderr);return 1
if __name__=='__main__':sys.exit(main())
