#!/usr/bin/env python3
"""Guest interval timers, inheritance, invalid inputs and host cancellation."""
import argparse,json,select,signal,subprocess,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime');p.add_argument('--report',type=Path,required=True);p.add_argument('--linux-reference',action='store_true');a=p.parse_args();rows=[]
for mode,code,wanted in [('validation',0,b'timer validation and error side effects match Linux\n'),('inherit',0,b'fork clears child timer and preserves parent timer\n'),('periodic',0,b'periodic alarm interrupts blocked guest reads\n'),('default',142,b''),('cancel',143,b'')]:
    child=subprocess.Popen([str(a.native.resolve()),'run',str(ROOT/'examples/m29/timer.elf'),'--job-control','--max-steps','1000000','--',mode],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    start=time.monotonic()
    try:
        if mode=='cancel':
            if not select.select([child.stdout],[],[],2)[0] or child.stdout.readline()!=b'ready\n':raise RuntimeError('Timer cancellation probe not ready')
            child.send_signal(signal.SIGTERM)
        out,err=child.communicate(timeout=2);elapsed=time.monotonic()-start
        good=child.returncode==code and out==wanted and not err and elapsed<2
        if mode in ['periodic','default']:good=good and elapsed>=.03
        rows.append(dict(name=mode,passed=good,exit_code=child.returncode,elapsed=elapsed,stdout=out.decode(),stderr=err.decode()));print(rows[-1],flush=True)
    finally:
        if child.poll() is None:child.kill();child.wait()
if a.linux_reference:
    image=json.loads((ROOT/'examples/m28/build-manifest.json').read_text())['image_id']
    for row in list(rows):
        if row['name']=='cancel':continue
        name='vmfree-m29-timer-'+uuid.uuid4().hex
        try:
            r=subprocess.run(['docker','run','--rm','--init','--name',name,'--network=none','--read-only','--cap-drop=ALL','--pids-limit=16','--memory=64m','--cpus=1','-v',str(ROOT/'examples/m29/timer.elf')+':/timer:ro','--entrypoint=/timer',image,row['name']],capture_output=True,timeout=10)
        finally:subprocess.run(['docker','rm','--force',name],capture_output=True,timeout=10)
        good=r.returncode==row['exit_code'] and r.stdout.decode()==row['stdout'] and not r.stderr
        rows.append(dict(name='linux_'+row['name'],passed=good,exit_code=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()));print(rows[-1],flush=True)
a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(dict(passed=all(x['passed'] for x in rows),checks=rows),indent=2)+'\n')
raise SystemExit(0 if all(x['passed'] for x in rows) else 1)
