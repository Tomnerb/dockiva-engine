#!/usr/bin/env python3
"""Compile and run direct decode equivalence and instruction-fetch checks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
cc=shutil.which('cc')
if not cc:raise SystemExit('C compiler required')
with tempfile.TemporaryDirectory() as temp:
    binary=str(Path(temp)/'decode-check')
    subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic',str(root/'tests/native/decode_cache.c'),'-o',binary],check=True)
    subprocess.run([binary],check=True)
