#!/usr/bin/env python3
"""Check allocation semantics; compare supported cases with a Linux reference."""
import argparse,hashlib,json,platform,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
FAULTS={'readonly_fault','after_unmap_fault','none_fault','after_shrink_fault'}
POLICY={'map_limit','brk_limit','unsupported_flags'}
def capture(cmd,env=None):
    p=subprocess.run(cmd,capture_output=True,timeout=15,env=env)
    return dict(stdout_hex=p.stdout.hex(),stderr_hex=p.stderr.hex(),exit_code=p.returncode)
def verify(native,reference):
    d=ROOT/'examples/m4';m=json.loads((d/'build-manifest.json').read_text());rows=[]
    for name,digest in m['sources'].items():
        if hashlib.sha256((d/name).read_bytes()).hexdigest()!=digest:raise ValueError('Source hash mismatch: '+name)
    for item in m['files']:
        path=d/item['name'];name=path.stem
        if hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:raise ValueError('Binary hash mismatch: '+name)
        actual=capture([str(native),'run',str(path)])
        expected=dict(stdout_hex=(b'heap allocation OK\n' if name=='demo' else b'').hex(),stderr_hex='',exit_code=0)
        fault=name in FAULTS
        passed=(actual['exit_code']==125 and not actual['stdout_hex'] and b'vmfree:' in bytes.fromhex(actual['stderr_hex'])) if fault else actual==expected
        row=dict(name=name,actual=actual,passed=passed,sha256=item['sha256'],scope='runtime-rejection' if fault else 'bounded-policy' if name in POLICY else 'linux-comparison')
        if not fault:row['expected']=expected
        if reference is not None and not fault and name not in POLICY:
            baseline=capture([*reference,str(path)],env={});row['linux_reference']=baseline;row['passed']=passed and baseline==expected
        rows.append(row)
    return dict(host=platform.platform(),passed=all(r['passed'] for r in rows),cases=rows)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime');p.add_argument('--report',type=Path)
    g=p.add_mutually_exclusive_group();g.add_argument('--qemu');g.add_argument('--native-linux',action='store_true');a=p.parse_args()
    if a.native_linux and (platform.system()!='Linux' or platform.machine() not in ('aarch64','arm64')):p.error('Native reference requires ARM64 Linux')
    if a.qemu and platform.system()!='Linux':p.error('QEMU reference requires Linux')
    try:
        r=verify(a.native.resolve(),[] if a.native_linux else [a.qemu] if a.qemu else None)
        r['reference_kind']='native-arm64-linux' if a.native_linux else 'qemu-user-linux' if a.qemu else 'not-run'
        for row in r['cases']:print(('PASS ' if row['passed'] else 'FAIL ')+row['name'])
        print('Linux reference: '+r['reference_kind'])
        if a.report:a.report.write_text(json.dumps(r,indent=2)+'\n')
        return 0 if r['passed'] else 1
    except (OSError,ValueError,KeyError,subprocess.TimeoutExpired) as exc:print('Verification failed: '+str(exc),file=sys.stderr);return 1
if __name__=='__main__':sys.exit(main())
