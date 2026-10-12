#!/usr/bin/env python3
"""Replay pinned prepared database roots without Docker; report every startup gap."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
from probe_databases import ROOT,run,startup_probe

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures',type=Path,default=ROOT/'build/database-fixtures-current')
    parser.add_argument('--native',type=Path,default=ROOT/'build/vmfree-c')
    parser.add_argument('--target',choices=['postgresql','redis','valkey'])
    parser.add_argument('--trace',action='store_true')
    parser.add_argument('--report',type=Path,default=ROOT/'evidence/database-services.json')
    a=parser.parse_args();rows=[]
    for name in ['postgresql','redis','valkey']:
        if a.target and a.target!=name:continue
        pin=json.loads((a.fixtures/(name+'.json')).read_text());source=a.fixtures/name
        for file,expected in pin['files'].items():
            path=source/file.lstrip('/')
            if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=expected['sha256']:raise ValueError('Fixture changed: '+str(path))
        with tempfile.TemporaryDirectory(prefix='vmfree-service-') as temporary:
            root=Path(temporary)/'root';shutil.copytree(source,root)
            options=['--root',str(root),'--dynamic','--job-control','--scheduled-network','--elf-limit-mib','64','--mmap-limit-mib','128','--max-steps','200000000','--env','PATH=/usr/local/bin:/usr/bin:/bin']
            if a.trace:options+=['--trace']
            if name=='postgresql':
                options[options.index('--mmap-limit-mib')+1]='64'
                options+=['--uid','70','--gid','70']
            binary=pin['binary'];command=[str(a.native.resolve()),'run',str(root/binary.lstrip('/')),*options,'--argv0',binary]
            row=dict(database=name,image_id=pin['image_id'],service_qualified=False)
            row['version']=run([*command,'--','--version'])
            if name=='postgresql':
                row['initialization']=run([str(a.native.resolve()),'run',str(root/'usr/local/bin/initdb'),*options,'--argv0','/usr/local/bin/initdb','--writable','--','--no-locale','--encoding=UTF8','--auth=trust','-D','/data'])
            if name=='postgresql' and row['initialization']['exit_code']!=0:
                row['startup']=dict(stage='blocked by failed cluster initialization',ready=False)
            else:
                row['startup']=startup_probe(command,root,name)
            rows.append(row);print(json.dumps(row),flush=True)
        a.report.parent.mkdir(parents=True,exist_ok=True)
        a.report.write_text(json.dumps(dict(scope='Prepared unchanged ELF fixtures; no Docker execution or fallback. No persistence qualification.',runtime_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest(),results=rows),indent=2)+'\n')
    return 0 if all(r['startup']['ready'] for r in rows) else 1
if __name__=='__main__':raise SystemExit(main())
