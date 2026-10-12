#!/usr/bin/env python3
"""Verify bounded ELF startup metadata; not glibc application qualification."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory() as temp:
    binary = str(Path(temp) / 'startup-check')
    flags = ['-fsanitize=address,undefined', '-g'] if args.sanitize else []
    subprocess.run([shutil.which('cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    *flags, str(ROOT / 'tests/native/m39_startup.c'), '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=10)
