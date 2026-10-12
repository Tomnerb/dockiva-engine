#!/usr/bin/env python3
"""Verify access, readlink and sync syscalls without claiming crash durability."""
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
    for source in ["m44_sync.c","m44_permissions.c"]:
        binary = str(Path(temp) / 'sync-check')
        flags = ['-fsanitize=address,undefined', '-g'] if args.sanitize else []
        subprocess.run([shutil.which('cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        *flags, str(ROOT / 'tests/native' / source), '-o', binary], check=True)
        subprocess.run([binary], check=True, timeout=10)
