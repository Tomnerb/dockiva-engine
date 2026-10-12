#!/usr/bin/env python3
"""Compile and run direct M18 filesystem ABI and malformed-pointer checks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
cc=shutil.which('cc')
if not cc:raise SystemExit('C compiler required')
with tempfile.TemporaryDirectory() as temp:
    binary=str(Path(temp)/'m18-fs-check')
    subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic',str(root/'tests/native/m18_fs.c'),'-o',binary],check=True)
    subprocess.run([binary],check=True)
