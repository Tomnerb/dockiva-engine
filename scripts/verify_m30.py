#!/usr/bin/env python3
"""Replay a pinned differential corpus; fail on every unclassified discrepancy."""
import argparse,hashlib,json,shutil,subprocess,tempfile,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def outcome(r):return dict(exit_code=r.returncode,stdout=r.stdout.decode(errors='replace'),stderr=r.stderr.decode(errors='replace'))
def classify(actual,expected,reference=None,reference_expected=None,gap=None):
    if actual!=expected:return 'regression'
    if reference is None:return 'local-only'
    if reference!=(reference_expected or expected):return 'reference-regression'
    if actual==reference:return 'matched'
    return 'declared-policy-gap' if gap else 'unclassified-discrepancy'
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path,required=True);p.add_argument('--linux-reference',action='store_true');p.add_argument('--corpus',type=Path,default=ROOT/'tests/corpus/m30/compatibility.json');a=p.parse_args()
    corpus=json.loads(a.corpus.read_text());rows=[];image=json.loads((ROOT/'examples/m28/build-manifest.json').read_text())['image_id']
    pins={}
    for name in ['build-manifest.json','probe-manifest.json']:
        value=json.loads((ROOT/'examples/m28'/name).read_text());pins.update(value.get('sha256',value))
    if any(not (ROOT/k).is_file() or digest(ROOT/k)!=v for k,v in pins.items()):raise RuntimeError('Fixture hash mismatch')
    for case in corpus['cases']:
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)/'root';shutil.copytree(ROOT/'examples/m28/root',root)
            (root/'data').write_text('fixture\n');(root/'link').symlink_to('data')
            executable=case.get('executable','bin/busybox')
            command=[str(a.native.resolve()),'run',str(root/executable),'--dynamic','--job-control','--scheduled-network','--writable','--root',str(root),'--env','PATH=/bin','--max-steps','20000000','--',*case['args']]
            actual=outcome(subprocess.run(command,stdin=subprocess.DEVNULL,capture_output=True,timeout=15));reference=None
            if a.linux_reference:
                # Recreate the writable fixture so runtime side effects cannot feed the reference.
                shutil.rmtree(root);shutil.copytree(ROOT/'examples/m28/root',root)
                (root/'data').write_text('fixture\n');(root/'link').symlink_to('data')
                name='vmfree-m30-'+uuid.uuid4().hex
                try:
                    r=subprocess.run(['docker','run','--rm','--pull=never','--name',name,'--network=none','--read-only','--cap-drop=ALL','--cap-add=SYS_CHROOT','--pids-limit=32','--memory=128m','--cpus=1','-v',str(root)+':/fixture','--entrypoint=/bin/busybox',image,'chroot','/fixture','/'+executable,*case['args']],capture_output=True,timeout=20)
                    reference=outcome(r)
                finally:subprocess.run(['docker','rm','--force',name],capture_output=True,timeout=10)
            classification=classify(actual,case['expected'],reference,case.get('reference_expected'),case.get('policy_gap'))
            rows.append(dict(id=case['id'],classification=classification,passed=classification in ['matched','declared-policy-gap','local-only'],actual=actual,reference=reference,policy_gap=case.get('policy_gap')))
            print(classification,case['id'],flush=True)
    malformed=json.loads((ROOT/'tests/corpus/m30/malformed.json').read_text())
    for case in malformed['cases']:
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'input.elf'
            if digest(ROOT/case['source'])!=case['source_sha256']:raise RuntimeError('Malformed corpus seed hash mismatch')
            data=bytearray((ROOT/case['source']).read_bytes())
            if 'truncate' in case:data=data[:case['truncate']]
            for offset,value in case.get('patches',[]):data[offset:offset+len(bytes.fromhex(value))]=bytes.fromhex(value)
            path.write_bytes(data)
            command=[str(a.native.resolve()),'run',str(path),'--max-steps',str(case.get('max_steps',1000))]
            r=subprocess.run(command,capture_output=True,timeout=5);good=r.returncode==125 and not r.stdout and case['diagnostic'] in r.stderr.decode()
            rows.append(dict(id=case['id'],classification='bounded-rejection' if good else 'regression',passed=good,input_sha256=digest(path),actual=outcome(r)))
            print(rows[-1]['classification'],case['id'],flush=True)
    report=dict(schema_version=1,passed=all(x['passed'] for x in rows),linux_reference=a.linux_reference,reference_image=image if a.linux_reference else None,corpus_sha256=digest(a.corpus),runtime_sha256=digest(a.native),source_hashes={str(x.relative_to(ROOT)):digest(x) for x in (ROOT/'native').glob('*') if x.suffix in ['.c','.h']},checks=rows)
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n');return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
