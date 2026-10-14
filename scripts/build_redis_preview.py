#!/usr/bin/env python3
"""Package the already-pinned prepared Redis fixture for local Dockiva testing."""
import gzip,hashlib,json,tarfile,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
import oci
from scripts.build_m6 import tar_bytes,write_layout

def main():
    source=ROOT/'build/database-fixtures-current/redis';pin=json.loads(source.with_suffix('.json').read_text())
    entries=[]
    for name,info in sorted(pin['files'].items()):
        p=source/name.lstrip('/');data=p.read_bytes()
        if p.is_symlink() or hashlib.sha256(data).hexdigest()!=info['sha256']:raise RuntimeError('Fixture mismatch: '+name)
        entries.append((name.lstrip('/'),data,tarfile.REGTYPE,p.stat().st_mode&0o777))
    entries.append(('data',b'',tarfile.DIRTYPE,0o700))
    destination=ROOT/'examples/redis-preview/image'
    layer=tar_bytes(entries)
    # gzip output differs between zlib versions: keep the committed layout (and its
    # pinned digest) while its layer still holds exactly these files.
    if (destination/'index.json').exists():
        current=oci.Image(destination)
        manifest=json.loads((destination/'blobs/sha256'/current.digest[7:]).read_bytes())
        if len(manifest['layers'])==1 and gzip.decompress((destination/'blobs/sha256'/manifest['layers'][0]['digest'][7:]).read_bytes())==layer:
            print((destination.parent/'profile.json').read_text().strip());return
    write_layout(destination,[layer],dict(Entrypoint=[pin['binary']],Cmd=['--bind','127.0.0.1','--port','16379','--dir','/data','--save','','--appendonly','yes','--appendfsync','always','--daemonize','no'],Env=[],WorkingDir='/'),compressed=True)
    profile=dict(name='dockiva/redis:preview',digest=oci.Image(destination).digest,source_image=pin['image_id'],scope='Prepared Redis 7.4.2, native execution, private container data, loopback TCP')
    (destination.parent/'profile.json').write_text(json.dumps(profile,indent=2)+'\n');print(json.dumps(profile))
if __name__=='__main__':main()
