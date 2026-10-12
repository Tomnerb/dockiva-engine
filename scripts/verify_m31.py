#!/usr/bin/env python3
"""Aggregate guest allocation limits and clone rollback."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c');p.add_argument('--report',type=Path,required=True);a=p.parse_args();rows=[]
for budget in [48,96]:
    for cache in [[],['--no-decode-cache'],['--no-lookup-cache']]:
        r=subprocess.run([str(a.native.resolve()),'run',str(ROOT/'examples/m31/probe.elf'),'--schedule','--memory-limit-mib',str(budget),'--profile','--max-steps','1000000',*cache,'--',*(['sufficient'] if budget==96 else [])],capture_output=True,timeout=10)
        lines=r.stderr.decode().splitlines();profile=json.loads(next(x[len('VMFREE_PROFILE '):] for x in lines if x.startswith('VMFREE_PROFILE ')))
        good=r.returncode==0 and len(lines)==1 and profile['allocated_bytes']==0 and profile['peak_allocated_bytes']<=budget*1024*1024 and profile['allocation_denials']==(4 if budget==48 else 0)
        rows.append(dict(name=str(budget)+''.join(cache),passed=good,profile=profile,stdout=r.stdout.decode()))
        print(('PASS ' if good else 'FAIL ')+rows[-1]['name'],flush=True)
for value in ['0','-1','1025','x','1']:
    r=subprocess.run([str(a.native.resolve()),'run',str(ROOT/'examples/m31/probe.elf'),'--memory-limit-mib',value],capture_output=True,timeout=5)
    rows.append(dict(name='budget_'+value,passed=r.returncode==125 and not r.stdout,stderr=r.stderr.decode()))
a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(dict(passed=all(x['passed'] for x in rows),checks=rows,runtime_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest()),indent=2)+'\n')
raise SystemExit(0 if all(x['passed'] for x in rows) else 1)
