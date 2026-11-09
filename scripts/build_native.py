#!/usr/bin/env python3
"""Build the host C interpreter using the local C compiler."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess

import build_el1

ROOT = Path(__file__).resolve().parents[1]
# The oldest macOS the engine runs on: Dockiva's (its LSMinimumSystemVersion). Without a
# deployment target, clang stamps the SDK's version as the programs' minimum and compiles
# __builtin_available checks for any older version as always true.
MACOS_MIN = '14.0'

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
    name = {(False, False): 'dockiva-runtime', (True, False): 'dockiva-runtime-sanitize',
            (False, True): 'dockiva-hvf', (True, True): 'dockiva-hvf-sanitize'}[(args.sanitize, args.with_hvf)]
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if platform.system() == 'Darwin':
        flags += ['-mmacosx-version-min='+MACOS_MIN]
    else:
        # The Linux CI build (GCC) checks what the Mac build (clang) checks. The runtime is one
        # translation unit of headers, and clang does not report unused statics in headers
        # (helpers only Darwin code calls are unused here). GCC's misleading-indentation check
        # (this code's one-line `if(a)x;if(b)y;`) and its flow-based truncation, bounds and
        # overlap guesses have no clang counterpart.
        flags += ['-Wno-unused-function', '-Wno-unused-variable', '-Wno-misleading-indentation',
                  '-Wno-format-truncation', '-Wno-array-bounds', '-Wno-restrict']
    if args.sanitize:
        flags += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    source = ROOT / 'native/runtime.c'
    libraries = ['-lm']                                                     # exp() (fs.h); part of libSystem on macOS
    if args.with_hvf:
        build_el1.build(args.cc)                                              # native/hvf_el1_code.h, from hvf_el1_guest.c
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
