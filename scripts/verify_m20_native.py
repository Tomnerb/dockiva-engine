#!/usr/bin/env python3
"""Compile and run M20 direct ABI rejection checks."""
import argparse,shutil,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
with tempfile.TemporaryDirectory() as temp:
    binary=str(Path(temp)/'m20-io-check')
    flags=['-fsanitize=address,undefined','-g'] if a.sanitize else []
    subprocess.run([shutil.which('cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic',*flags,str(root/'tests/native/m20_io.c'),'-o',binary],check=True)
    subprocess.run([binary],check=True)
