#!/usr/bin/env python3
import argparse,subprocess,tempfile,shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');a=p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    binary=str(Path(tmp)/'signals')
    flags=['-fsanitize=address,undefined','-g'] if a.sanitize else []
    subprocess.run([shutil.which('cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic',*flags,str(root/'tests/native/m22_signals.c'),'-o',binary],check=True)
    for case in ['cancel-read','cancel-write','cancel-spin','cancel-wait','self-term']:
        subprocess.run([binary,'run',str(root/'examples/m22/probe.elf'),'--schedule','--max-steps','10000000','--',case],check=True,timeout=20)
