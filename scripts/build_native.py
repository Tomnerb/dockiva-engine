#!/usr/bin/env python3
"""Build the host C interpreter using the local C compiler."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=shutil.which('cc'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--with-hvf', action='store_true', help='Build the experimental native Hypervisor.framework backend (signed with com.apple.security.hypervisor)')
    args = parser.parse_args()
    if not args.cc:
        parser.error('A C compiler is required. On macOS, install Xcode Command Line Tools.')
    out = ROOT / 'build'
    out.mkdir(exist_ok=True)
    name = 'vmfree-c-sanitize' if args.sanitize else 'vmfree-c'
    if args.with_hvf:
        name += '-hvf'
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        flags += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    source = ROOT / 'native/runtime.c'
    libraries = []
    if args.with_hvf:
        flags += ['-DVMFREE_HVF']
        libraries += ['-framework', 'Hypervisor']
    command = [args.cc, *flags, str(source), *libraries, '-o', str(out / name)]
    subprocess.run(command, check=True)
    if args.with_hvf:
        # Hypervisor.framework refuses unsigned callers; ad-hoc signing suffices locally.
        subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements', str(ROOT/'native/hvf.entitlements.plist'), str(out / name)], check=True)
    metadata = dict(host=platform.platform(), compiler=subprocess.check_output([args.cc, '--version'], text=True).splitlines()[0], flags=flags, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
    metadata['source_hashes']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((ROOT/'native').glob('*')) if p.suffix in ('.c','.h')}
    (out / (name + '.json')).write_text(json.dumps(metadata, indent=2)+'\n')
    print(out / name)

if __name__ == '__main__':
    main()
