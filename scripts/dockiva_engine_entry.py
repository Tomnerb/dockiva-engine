"""Legacy PyInstaller entrypoint retained for migration reference; new bundles use Go dockivad."""
import os
from pathlib import Path
import runpy
import sys
# Historical frozen builds used these imports to discover worker dependencies.
import daemon
import containers
import confined_exec
import registry


def main():
    os.umask(0o077)
    modules = {'daemon.py': 'daemon', 'containers.py': 'containers',
               'confined_exec.py': 'confined_exec', 'registry.py': 'registry'}
    if len(sys.argv) > 1 and Path(sys.argv[1]).name in modules:
        module = modules[Path(sys.argv.pop(1)).name]
    else:
        module = 'daemon'
    if module == 'daemon':
        # Seed a bounded known-compatible fixture so the GUI is usable immediately.
        home = Path.home()/'.vmfree'
        if '--home' in sys.argv:
            home = Path(sys.argv[sys.argv.index('--home')+1])
        root = Path(daemon.__file__).parent
        for name, relative in (
            ('dockiva/shell:demo', 'examples/m19/shell-image'),
            ('dockiva/server:demo', 'examples/m11_17/http-image'),
            ('dockiva/redis:preview', 'examples/redis-preview/image'),
        ):
            fixture = root/relative
            if fixture.is_dir():
                registry.private(home)
                if not (home/'images'/registry.key(name)).exists():
                    registry.commit(home/'images', name, fixture)
    runpy.run_module(module, run_name='__main__')

if __name__ == '__main__':
    main()
