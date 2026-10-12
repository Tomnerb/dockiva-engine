#!/usr/bin/env python3
"""Validate scheduled guest cancellation and signal-status propagation."""
import argparse,hashlib,json,platform,subprocess,tempfile,shutil,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path);a=p.parse_args()
    binary=str(a.native.resolve());d=ROOT/'examples/m22';rows=[]
    def check(name,r,code,out=b'',err=b''):
        good=r.returncode==code and r.stdout==out and r.stderr==err
        rows.append(dict(name=name,passed=good,exit_code=r.returncode,stdout=r.stdout.decode(errors='replace'),stderr=r.stderr.decode(errors='replace')))
        print(('PASS ' if good else 'FAIL ')+name,flush=True)
        if not good:raise RuntimeError(rows[-1])
    try:
        m=json.loads((d/'build-manifest.json').read_text());assert all(hashlib.sha256((ROOT/k).read_bytes()).hexdigest()==v for k,v in m['sha256'].items())
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'bin').mkdir()
            for source,name in [(d/'shell.elf','sh'),(d/'probe.elf','probe'),(ROOT/'examples/m18/busybox.elf','busybox')]:shutil.copy(source,root/'bin'/name)
            for flags in [[],['--no-decode-cache'],['--no-lookup-cache']]:
                for case,code in [('cancel-spin',0),('cancel-read',0),('cancel-write',0),('cancel-wait',130),('self-term',143),('self-pipe',141),('signal-zombie',0),('signal-errors',0)]:
                    r=subprocess.run([binary,'run',root/'bin/probe','--root',root,'--schedule','--max-steps','10000000',*flags,'--',case],capture_output=True,timeout=20)
                    check(case+''.join(flags),r,code,b'cancelled child; sibling and parent survived\n' if case in ['cancel-spin','cancel-read','cancel-write'] else b'')
            r=subprocess.run([binary,'run',root/'bin/probe','--','signals-disabled'],capture_output=True,timeout=20)
            check('signals_require_scheduler',r,0)
            r=subprocess.run([binary,'run',root/'bin/probe','--schedule','--','portable-signal-status'],capture_output=True,timeout=20)
            check('portable_signal_status',r,0,b'signal statuses: 2 9 13 15; reaped PIDs absent\n')
            for command,code,out in [('probe self-term | busybox cat',143,b''),('probe self-term | busybox cat; status',143,b'143\n'),('probe self-term || busybox echo recovered',0,b'recovered\n')]:
                r=subprocess.run([binary,'run',root/'bin/sh','--root',root,'--schedule','--max-steps','10000000','--','-c',command],capture_output=True,timeout=20);check(command,r,code,out)
            r=subprocess.run([sys.executable,ROOT/'oci.py','run',d/'signal-image','--native',binary,'--schedule','--max-steps','10000000','--','cancel-read'],capture_output=True,timeout=20)
            check('oci_cancellation',r,0,b'cancelled child; sibling and parent survived\n')
    except Exception as exc:
        if not rows or rows[-1]['passed']:rows.append(dict(name='verification_error',passed=False,error=str(exc)));print('FAIL',exc)
    report=dict(milestone=22,host=platform.platform(),passed=bool(rows) and all(r['passed'] for r in rows),results=rows,runtime_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),source_hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted((ROOT/'native').glob('*')) if x.suffix in ('.c','.h')},linux_reference='not-run')
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
