#!/usr/bin/env python3
"""Test owned CPU snapshots with ASan/UBSan."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, default=ROOT/'evidence/state-ownership/cpu-state.json')
    args = parser.parse_args()
    folder = ROOT / 'build/cpu-state'
    folder.mkdir(exist_ok=True)
    source = ROOT / 'tests/native/cpu_state_host.c'
    rows = []
    for name in ('interpreter-sanitize',):
        binary = folder / name
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic']
        libraries = []
        flags += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(['cc', *flags, str(source), *libraries, '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        rows.append(dict(test=name, passed=result.returncode == 0, exit=result.returncode,
                         stdout=result.stdout, stderr=result.stderr,
                         binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
        print(name, 'PASS' if result.returncode == 0 else rows[-1], flush=True)
    report = args.report
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(dict(results=rows, source_hashes={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [source, *sorted((ROOT/'native').glob('*.h')), ROOT/'native/runtime.c']}), indent=2)+'\n')
    return 0 if all(row['passed'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
