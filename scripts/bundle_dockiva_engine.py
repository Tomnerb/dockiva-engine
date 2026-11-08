#!/usr/bin/env python3
"""Build Dockiva's Go/C helper. Python is build tooling, never bundled/executed."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from build_native import MACOS_MIN

ROOT = Path(__file__).resolve().parents[1]


def validate_profile_pins(catalog, fixture_digests):
    # Catalog pins describe prepared bundle fixtures. A fixture refresh must
    # update its reviewed policy instead of falling through to the default.
    for profile in catalog['images']:
        if not set(profile['image_digests']).issubset(fixture_digests):
            raise ValueError('Compatibility profile has a stale fixture digest: '+profile['id'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('Build on Apple Silicon macOS')
    for flags in ([], ['--with-hvf']):
        subprocess.run([sys.executable, str(ROOT/'scripts/build_native.py'), *flags], check=True)
    # The prepared Redis image is committed; rebuild it only where its pinned fixture
    # exists (a developer checkout). The digest check below covers both cases.
    if (ROOT/'build/database-fixtures-current/redis.json').exists():
        subprocess.run([sys.executable, str(ROOT/'scripts/build_redis_preview.py')], check=True)
    layouts = ('examples/m19/shell-image', 'examples/m11_17/http-image', 'examples/redis-preview/image')
    fixture_digests = {json.loads((ROOT/path/'index.json').read_text())['manifests'][0]['digest'] for path in layouts}
    validate_profile_pins(json.loads((ROOT/'dockivad/profiles.json').read_text()), fixture_digests)
    # Go's external link (clang) takes the minimum from MACOSX_DEPLOYMENT_TARGET. Go's build
    # cache key leaves that variable out, so CGO_CFLAGS carries the minimum too: otherwise
    # cached cgo objects (CI restores the cache) keep the SDK's version.
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET=MACOS_MIN, CGO_CFLAGS='-O2 -g -mmacosx-version-min='+MACOS_MIN)
    subprocess.run(['go', 'build', '-trimpath', '-ldflags=-linkmode=external', '-o', str(ROOT/'build/dockivad'), '.'], cwd=ROOT/'dockivad', env=env, check=True)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic', '-mmacosx-version-min='+MACOS_MIN,
                    str(ROOT/'native/worker_launcher.c'), '-o', str(ROOT/'build/dockiva-worker')], check=True)
    # FEX and its arm64 glibc, for linux/amd64 images: build/x86 (Linux ELF files,
    # not signed). The engine finds it beside the runtime.
    subprocess.run([sys.executable, '-I', str(ROOT/'scripts/build_x86.py')], check=True)
    output = args.output.absolute()
    output.mkdir(parents=True, exist_ok=True)
    target = output/'dockiva-engine'
    if target.is_symlink():
        parser.error('Bundle target must not be a symlink')
    with tempfile.TemporaryDirectory(prefix='.dockivad-build-', dir=output) as temporary:
        stage = Path(temporary)/'dockiva-engine'
        (stage/'_internal/build').mkdir(parents=True)
        shutil.copy2(ROOT/'build/dockivad', stage/'dockivad')
        # Dockiva starts the engine by this name, and it shows in Activity Monitor.
        shutil.copy2(stage/'dockivad', stage/'dockiva-engine')
        for name in ('dockiva-runtime', 'dockiva-hvf', 'dockiva-worker'):
            shutil.copy2(ROOT/'build'/name, stage/'_internal/build'/name)
        shutil.copytree(ROOT/'build/x86', stage/'_internal/build/x86')
        for relative in ('examples/m19/shell-image', 'examples/m11_17/http-image', 'examples/redis-preview'):
            shutil.copytree(ROOT/relative, stage/'_internal'/relative)
        for executable in (stage/'dockivad', stage/'dockiva-engine', stage/'_internal/build/dockiva-runtime', stage/'_internal/build/dockiva-worker'):
            subprocess.run(['codesign', '--force', '--sign', '-', str(executable)], check=True)
        # Native execution: Hypervisor.framework requires this entitlement.
        subprocess.run(['codesign', '--force', '--sign', '-', '--options', 'runtime',
                        '--entitlements', str(ROOT/'native/hvf.entitlements.plist'),
                        str(stage/'_internal/build/dockiva-hvf')], check=True)
        for executable in (stage/'dockivad', stage/'dockiva-engine', *(p for p in (stage/'_internal/build').iterdir() if p.is_file())):
            subprocess.run(['codesign', '--verify', '--strict', str(executable)], check=True)
            # Nothing else catches a program that declares a newer macOS than the app.
            if f' minos {MACOS_MIN}\n' not in subprocess.run(['otool', '-l', str(executable)], capture_output=True, text=True, check=True).stdout:
                raise RuntimeError(f'{executable.name} is not built for macOS {MACOS_MIN}')
        forbidden = [p for p in stage.rglob('*') if p.suffix in ('.py', '.pyc') or 'python' in p.name.lower()]
        if forbidden:
            raise RuntimeError('Python must not be shipped in the engine bundle: '+str(forbidden))
        backup = Path(temporary)/'previous-bundle'
        if target.exists():
            target.rename(backup)
        try:
            stage.rename(target)
        except BaseException:
            if backup.exists():
                backup.rename(target)
            raise
    print(target/'dockiva-engine')


if __name__ == '__main__':
    main()
