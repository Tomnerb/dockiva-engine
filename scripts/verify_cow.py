#!/usr/bin/env python3
"""Test private fork COW, macOS host-page allocation bounds and backend isolation."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, default=ROOT/'evidence/cow/cow-tests.json')
    args = parser.parse_args()
    folder = ROOT / 'build/cow'
    folder.mkdir(exist_ok=True)
    source = ROOT / 'tests/native/cow_host.c'
    rows = []
    for name in ('interpreter-sanitize', 'page-sanitize'):
        source = ROOT / 'tests/native' / ('page_cow_host.c' if name == 'page-sanitize' else 'cow_host.c')
        binary = folder / name
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
        libraries = []
        flags += ['-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        subprocess.run(['cc', *flags, str(source), *libraries, '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        rows.append(dict(test=name, passed=result.returncode == 0 and not result.stderr, exit=result.returncode,
                         stdout=result.stdout, stderr=result.stderr,
                         binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
        print(name, 'PASS' if rows[-1]['passed'] else rows[-1], flush=True)
        if name == 'page-sanitize':
            continue
        result = subprocess.run([str(binary), '--oom-store'], capture_output=True, text=True, timeout=15)
        # A store past the memory budget is an OOM kill of the process, as Docker's cgroup does.
        passed = result.returncode == 137 and result.stderr == 'dockiva: out of memory: killed process 1\n'
        rows.append(dict(test=name+'-oom-store', passed=passed, exit=result.returncode,
                         stdout=result.stdout, stderr=result.stderr))
        print(name+'-oom-store', 'PASS' if passed else rows[-1], flush=True)
    report = args.report
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(dict(results=rows, source_hashes={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [ROOT/'tests/native/cow_host.c', ROOT/'tests/native/page_cow_host.c', *sorted((ROOT/'native').glob('*.h')), ROOT/'native/runtime.c']}), indent=2)+'\n')
    return 0 if all(row['passed'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
