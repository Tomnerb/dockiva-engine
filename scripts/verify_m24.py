#!/usr/bin/env python3
"""Pinned upstream ash, static PIE, SIMD, nested processes and signal masks."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
CASES = [
    ('echo', 'echo hello', 0, b'hello\n', b''),
    ('variables', 'x=world; echo "hello $x"', 0, b'hello world\n', b''),
    ('arithmetic', 'echo $((7*6))', 0, b'42\n', b''),
    ('for', 'for x in a b c; do echo "$x"; done', 0, b'a\nb\nc\n', b''),
    ('conditional', 'false && echo no || echo yes', 0, b'yes\n', b''),
    ('pipeline', 'echo hello | cat', 0, b'hello\n', b''),
    ('three_stages', 'echo a | cat | cat', 0, b'a\n', b''),
    ('substitution', 'echo "$(echo substituted)"', 0, b'substituted\n', b''),
    ('nested_substitution', 'echo "$(echo $(echo deep))"', 0, b'deep\n', b''),
    ('subshell', '(echo nested); echo outer', 0, b'nested\nouter\n', b''),
    ('external', '/bin/echo external', 0, b'external\n', b''),
    ('redirection', 'echo alpha >/tmp/out; echo beta >>/tmp/out; cat </tmp/out', 0, b'alpha\nbeta\n', b''),
    ('nested_pipeline', 'sh -c "echo nested | cat"; echo done', 0, b'nested\ndone\n', b''),
    ('nested_status', 'sh -c "exit 7"; echo $?', 0, b'7\n', b''),
    ('printf', 'printf "%s:%d\\n" value 7', 0, b'value:7\n', b''),
    ('while', 'i=0; while [ "$i" -lt 3 ]; do echo "$i"; i=$((i+1)); done', 0, b'0\n1\n2\n', b''),
    ('case', 'case abc in a*) echo matched;; *) echo bad;; esac', 0, b'matched\n', b''),
    ('function', 'f() { echo "function:$1"; }; f yes', 0, b'function:yes\n', b''),
    ('default_expansion', 'echo "${missing:-fallback}"', 0, b'fallback\n', b''),
    ('substitution_pipeline', 'echo "$(echo a | cat)"', 0, b'a\n', b''),
    ('stderr_status', 'echo test >&2; exit 7', 7, b'', b'test\n'),
]
PROBES = {
    'mask': b'masked signal delivered after unblock\n',
    'ignored': b'ignored pending signal cleared and disposition inherited\n',
    'signal-errors': b'signal errors preserved state\n',
    'nested': b'process tree reaped correctly\n',
    'orphan': b'process tree reaped correctly\n',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=ROOT / 'build/dockiva-runtime')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--linux-reference', action='store_true')
    args = parser.parse_args()
    native = str(args.native.resolve())
    folder = ROOT / 'examples/m24'
    manifest = json.loads((folder / 'build-manifest.json').read_text())
    rows = []

    def check(name, good, **details):
        rows.append(dict(name=name, passed=bool(good), **details))
        print(('PASS ' if good else 'FAIL ') + name, flush=True)
        if not good:
            raise RuntimeError(str(details))

    def expect(name, result, code, out, err=b''):
        check(name, (result.returncode, result.stdout, result.stderr) == (code, out, err),
              exit_code=result.returncode, stdout=result.stdout[:500].decode(errors='replace'),
              stderr=result.stderr[:1000].decode(errors='replace'))

    def linux(program, command, mount=None):
        name = 'vmfree-m24-reference-' + uuid.uuid4().hex
        options = ['docker', 'run', '--rm', '--pull=never', '--name', name, '--network=none',
                   '--cap-drop=ALL', '--pids-limit=64', '--memory=128m', '--cpus=1']
        if mount:
            options += ['--mount', f'type=bind,source={mount},target={program},readonly']
        try:
            return subprocess.run([*options, '--entrypoint', program, manifest['image_id'], *command],
                                  capture_output=True, timeout=30)
        finally:
            subprocess.run(['docker', 'rm', '--force', name], capture_output=True, timeout=15)

    try:
        pins = {**manifest['sha256'], **json.loads((folder / 'conformance-manifest.json').read_text())}
        check('fixture_hashes', all(hashlib.sha256((ROOT / k).read_bytes()).hexdigest() == v for k, v in pins.items()))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'bin').mkdir()
            (root / 'tmp').mkdir()
            for name in ['sh', 'busybox', 'cat', 'echo']:
                shutil.copy(folder / 'busybox.elf', root / 'bin' / name)
            base = [native, 'run', str(root / 'bin/sh'), '--argv0', '/bin/sh', '--env', 'PATH=/bin',
                    '--root', str(root), '--writable', '--process-tree', '--max-steps', '10000000']
            for flags in [[], ['--no-decode-cache'], ['--no-lookup-cache']]:
                suffix = ''.join(flags)
                for name, command, code, out, err in CASES:
                    result = subprocess.run([*base, *flags, '--', '-c', command], capture_output=True, timeout=20)
                    expect(name + suffix, result, code, out, err)
                for mode, out in PROBES.items():
                    result = subprocess.run([native, 'run', str(folder / 'probe.elf'), '--process-tree',
                                             '--max-steps', '10000000', *flags, '--', mode], capture_output=True, timeout=20)
                    expect(mode + suffix, result, 0, out)
            isa = subprocess.run([native, 'run', str(folder / 'conformance.elf')], capture_output=True, timeout=20)
            check('isa_output', isa.returncode == 0 and not isa.stderr and len(isa.stdout) == 264,
                  sha256=hashlib.sha256(isa.stdout).hexdigest())
            for flag in ['--no-decode-cache', '--no-lookup-cache']:
                result = subprocess.run([native, 'run', str(folder / 'conformance.elf'), flag], capture_output=True, timeout=20)
                expect('isa' + flag, result, 0, isa.stdout)
            # Reject malformed bias and dynamic metadata before entering guest code.
            original = (folder / 'busybox.elf').read_bytes()
            phoff = struct.unpack_from('<Q', original, 32)[0]
            count = struct.unpack_from('<H', original, 56)[0]
            dynamic = next(phoff + i * 56 for i in range(count)
                           if struct.unpack_from('<I', original, phoff + i * 56)[0] == 2)
            load = next(phoff + i * 56 for i in range(count)
                        if struct.unpack_from('<I', original, phoff + i * 56)[0] == 1)
            mutations = [('entry_overflow', 24, '<Q', 2**64-1),
                         ('bias_overflow', load+16, '<Q', 2**64-4096),
                         ('dynamic_oob', dynamic+8, '<Q', 2**64-1),
                         ('dynamic_partial', dynamic+32, '<Q', 1),
                         ('interpreter_rejected', dynamic, '<I', 3)]
            dynoff = struct.unpack_from('<Q', original, dynamic+8)[0]
            mutations.append(('needed_rejected', dynoff, '<Q', 1))
            for name, offset, fmt, value in mutations:
                malformed = bytearray(original)
                struct.pack_into(fmt, malformed, offset, value)
                path = root / 'malformed.elf'
                path.write_bytes(malformed)
                result = subprocess.run([native, 'run', str(path)], capture_output=True, timeout=10)
                check(name, result.returncode == 125 and not result.stdout and b'vmfree:' in result.stderr)
            result = subprocess.run([sys.executable, str(ROOT / 'oci.py'), 'run', str(folder / 'shell-image'),
                                     '--native', native, '--process-tree', '--max-steps', '10000000', '--', '-c',
                                     'echo "$(echo upstream | cat)"'], capture_output=True, timeout=20)
            expect('oci_upstream_shell', result, 0, b'upstream\n')
            if args.linux_reference:
                for name, command, code, out, err in CASES:
                    expect('linux_' + name, linux('/bin/sh', ['-c', command]), code, out, err)
                expect('linux_isa', linux('/probe', [], folder / 'conformance.elf'), 0, isa.stdout)
                for mode, out in PROBES.items():
                    if mode != 'signal-errors':
                        expect('linux_' + mode, linux('/probe', [mode], folder / 'probe.elf'), 0, out)
    except Exception as exc:
        if not rows or rows[-1]['passed']:
            rows.append(dict(name='verification_error', passed=False, error=str(exc)))
            print('FAIL', exc)
    report = dict(milestone=24, host=platform.platform(), passed=bool(rows) and all(r['passed'] for r in rows),
                  results=rows, linux_reference=manifest['image_id'] if args.linux_reference else 'not-run',
                  runtime_sha256=hashlib.sha256(Path(native).read_bytes()).hexdigest(),
                  source_hashes={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted((ROOT / 'native').glob('*')) if p.suffix in ('.h', '.c')},
                  excluded=['caught signal handlers and traps', 'job control', 'unbounded process trees',
                            'chdir', 'general filesystem and full applet compatibility'])
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
