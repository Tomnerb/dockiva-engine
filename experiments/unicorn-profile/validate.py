"""Compare prebuilt, otherwise identical source builds; local Apple Silicon only."""
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT/'build/unicorn-profile'
OUT = ROOT/'evidence/unicorn-profile'


def run(args, label, timeout=30):
    result = subprocess.run([str(a) for a in args], cwd=ROOT, text=True,
                            capture_output=True, timeout=timeout)
    OUT.joinpath(label+'.stdout').write_text(result.stdout)
    OUT.joinpath(label+'.stderr').write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{label}: exit {result.returncode}; see evidence')
    return result.stdout


def main():
    report = {'runs': [], 'hashes': {}, 'signing': 'ad-hoc hardened runtime; allow-jit only'}
    for flavor in ('baseline', 'patched'):
        for program, source in [('probe', ROOT/'experiments/unicorn-adapter/probe.c'),
                                ('regressions', ROOT/'experiments/unicorn-profile/regressions.c')]:
            binary = BUILD/f'{program}-{flavor}-hardened'
            run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                 '-D_DARWIN_C_SOURCE', '-I', ROOT/'build/unicorn-adapter', '-I',
                 BUILD/'unicorn-2.1.4/include', source, ROOT/'build/unicorn-adapter/native.S',
                 BUILD/flavor/'libunicorn.a', '-lpthread', '-lm', '-o', binary], f'compile-{program}-{flavor}')
            run(['codesign', '--force', '--sign', '-', '--options', 'runtime', '--entitlements',
                 ROOT/'experiments/unicorn-adapter/jit.entitlements.plist', binary], f'sign-{program}-{flavor}')
            run(['codesign', '--verify', '--strict', '--verbose=2', binary], f'verify-{program}-{flavor}')
            run(['codesign', '--display', '--verbose=4', '--entitlements', '-', binary], f'signature-{program}-{flavor}')
            report['hashes'][str(binary.relative_to(ROOT))] = hashlib.sha256(binary.read_bytes()).hexdigest()
        text = run([BUILD/f'regressions-{flavor}-hardened'], f'hardened-guards-{flavor}')
        rows = [json.loads(line) for line in text.splitlines()]
        assert len(rows) == 2 and all(r['passed'] for r in rows)
        run([BUILD/flavor/'test_arm64'], f'arm64-{flavor}')
    # ABBA order reduces drift between baseline and candidate binaries.
    for index, flavor in enumerate(('baseline', 'patched', 'patched', 'baseline')):
        text = run([BUILD/f'probe-{flavor}-hardened'], f'measure-{index}-{flavor}')
        rows = [json.loads(line) for line in text.splitlines()]
        assert len(rows) == 60 and all(r.get('passed', True) for r in rows)
        report['runs'].append({'index': index, 'flavor': flavor, 'rows': rows})
    report['medians_seconds'] = {}
    for flavor in ('baseline', 'patched'):
        rows = [r for run_ in report['runs'] if run_['flavor'] == flavor for r in run_['rows']]
        report['medians_seconds'][flavor] = {
            workload: {str(mode): statistics.median(r['seconds'] for r in rows
                         if r.get('workload') == workload and r.get('mode') == mode)
                       for mode in range(5)}
            for workload in ('compute', 'memory64k')}
    for p in [ROOT/'experiments/unicorn-profile/nonexec-dirty.patch',
              ROOT/'experiments/unicorn-profile/regressions.c',
              ROOT/'experiments/unicorn-profile/profile.c', Path(__file__),
              ROOT/'experiments/unicorn-adapter/probe.c',
              BUILD/'baseline/libunicorn.a', BUILD/'patched/libunicorn.a']:
        report['hashes'][str(p.relative_to(ROOT))] = hashlib.sha256(p.read_bytes()).hexdigest()
    report['passed'] = True
    OUT.joinpath('comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report['medians_seconds'], indent=2))


if __name__ == '__main__':
    main()
