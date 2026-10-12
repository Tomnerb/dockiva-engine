#!/usr/bin/env python3
"""Use a native host-syscall probe to test Seatbelt independently of guest adapters."""
import argparse,json,platform,shutil,subprocess,sys,tempfile,socket,threading
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
if platform.system()!='Darwin':raise SystemExit('Seatbelt qualification requires macOS')
rows=[]
with tempfile.TemporaryDirectory() as temp:
    folder=Path(temp);binary=folder/'host-probe'
    subprocess.run([shutil.which('cc'),'-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/native/m31_host_sandbox.c'),'-o',str(binary)],check=True)
    outside=folder/'outside';outside.write_text('secret')
    for writable in [False,True]:
        root=folder/('rw' if writable else 'ro');root.mkdir();(root/'guest').write_bytes(b'fixture');(root/'link').symlink_to(outside)
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(5)
        def serve():
            try:
                with listener.accept()[0] as peer:peer.sendall(b'k')
            except OSError:pass
        thread=threading.Thread(target=serve,daemon=True);thread.start()
        r=subprocess.run([sys.executable,str(ROOT/'confined_exec.py'),'--seatbelt',str(binary),'run',str(root/'guest'),'--root',str(root),*(['--writable'] if writable else []),'--',str(outside),str(listener.getsockname()[1])],capture_output=True,timeout=10)
        listener.close();thread.join(6)
        good=r.returncode==0 and r.stdout==b'PASS kernel filesystem, link, client-only network, fork and exec restrictions\n' and not r.stderr and outside.read_text()=='secret'
        rows.append(dict(name='writable' if writable else 'readonly',passed=good,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
        print(rows[-1],flush=True)
a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(dict(passed=all(x['passed'] for x in rows),host=platform.platform(),checks=rows),indent=2)+'\n')
raise SystemExit(0 if all(x['passed'] for x in rows) else 1)
