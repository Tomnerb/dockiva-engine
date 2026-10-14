#!/usr/bin/env python3
"""Exercise Dockiva's adapter against an isolated real VM-free engine."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dockiva', type=Path, required=True)
    parser.add_argument('--helper', type=Path, default=ROOT/'build/dockiva-go/dockiva-engine/dockiva-engine')
    args = parser.parse_args()
    helper = args.helper.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='dv-', dir='/tmp') as directory:
        home = Path(directory)
        subprocess.run([str(helper), '--home', str(home), '--import-name', 'vmfree/test:demo', '--import-layout', str(ROOT/'examples/m19/shell-image')], check=True, capture_output=True)
        with (home/'engine.log').open('w+') as log:
            engine = subprocess.Popen([str(helper), '--home', str(home)], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if (home/'connection.json').exists():
                        break
                    if engine.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read())
                    time.sleep(.05)
                else:
                    raise RuntimeError('engine startup timeout')
                env = dict(os.environ, DOCKIVA_TEST_VMFREE_HOME=str(home))
                result = subprocess.run(['go', 'test', '.', '-run', 'VMFree', '-count=1', '-v'], cwd=args.dockiva.resolve(), env=env, timeout=180)
                return result.returncode
            finally:
                # Clean only this verifier's dedicated store, including failed tests.
                for action in ('stop', 'rm'):
                    subprocess.run([sys.executable, str(ROOT/'containers.py'), '--state-dir', str(home/'containers'), action, 'dockiva-preview-test'], capture_output=True, timeout=30)
                engine.terminate()
                try:
                    engine.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    engine.kill()
                    engine.wait()

if __name__ == '__main__':
    sys.exit(main())
