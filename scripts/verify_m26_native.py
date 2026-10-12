#!/usr/bin/env python3
"""Assert task and network-resource cleanup, optionally with sanitizers."""
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
    binary = str(Path(temp) / 'network-check')
    flags = ['-fsanitize=address,undefined', '-g'] if args.sanitize else []
    subprocess.run([shutil.which('cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    *flags, str(ROOT / 'tests/native/m26_network.c'), '-o', binary], check=True)
    for mode in ['roundtrip', 'cancel-accept', 'read-timeout', 'poll-timeout', 'accept-timeout']:
        subprocess.run([binary, 'run', str(ROOT / 'examples/m26/probe.elf'), '--scheduled-network',
                        '--max-steps', '10000000', '--', mode], check=True, timeout=10)
