#!/usr/bin/env python3
"""VM-free prototype command entrypoint."""
import hashlib,json,os
from pathlib import Path
import platform,shutil,subprocess,sys
ROOT=Path(__file__).resolve().parent

def main():
    if len(sys.argv)<2:raise SystemExit('Usage: vmfree.py engine|image|container|compose|docker|doctor [arguments]')
    action=sys.argv[1];args=sys.argv[2:]
    scripts={'engine':'daemon.py','image':'registry.py','container':'containers.py','compose':'compose.py'}
    if action in scripts:os.execv(sys.executable,[sys.executable,str(ROOT/scripts[action]),*args])
    if action=='doctor':
        native=ROOT/'build/vmfree-c';home=Path.home()/'.vmfree'
        print(json.dumps(dict(version='0.17.0',host=platform.platform(),machine=platform.machine(),python=sys.version.split()[0],native_exists=native.is_file(),native_sha256=hashlib.sha256(native.read_bytes()).hexdigest() if native.is_file() else None,engine_connection_recorded=(home/'connection.json').exists(),docker_cli=shutil.which('docker'),security_status='prototype; independent isolation review pending'),indent=2));return 0
    if action=='docker':
        # Explicitly target our engine without changing the user's Docker context.
        home=Path(os.environ.get('VMFREE_HOME',str(Path.home()/'.vmfree')))
        connection=json.loads((home/'connection.json').read_text());docker=shutil.which('docker')
        if not docker:raise SystemExit('Docker CLI is not installed; bundled image/container/compose commands remain available')
        env=dict(os.environ);env.pop('DOCKER_CONTEXT',None);env['DOCKER_API_VERSION']='1.45'
        return subprocess.call([docker,'--config',connection['config'],'--host',connection['host'],*args],env=env)
    raise SystemExit('Unknown command: '+action)
if __name__=='__main__':sys.exit(main())
