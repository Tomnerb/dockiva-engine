#!/usr/bin/env python3
"""Verify actual scheduler suspension, streaming integrity and resource release."""
import argparse,shutil,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    temp=Path(tmp);(temp/'bin').mkdir();binary=str(temp/'check')
    for src,name in [('examples/m21/shell.elf','sh'),('examples/m21/probe.elf','probe'),('examples/m18/busybox.elf','busybox')]:shutil.copy(root/src,temp/'bin'/name)
    flags=['-fsanitize=address,undefined','-g'] if a.sanitize else []
    subprocess.run([shutil.which('cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic',*flags,str(root/'tests/native/m21_scheduler.c'),'-o',binary],check=True)
    r=subprocess.run([binary,'run',str(temp/'bin/sh'),'--root',str(temp),'--schedule','--max-steps','100000000','--','-c','probe large | probe slowcat | busybox cat'],capture_output=True,timeout=30)
    assert r.returncode==0,(r.returncode,r.stderr)
    assert r.stdout==b'Z'*(4*1024*1024)
    print(r.stderr.decode(),end='')
