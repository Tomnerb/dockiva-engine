#!/usr/bin/env python3
"""Install this CLI release into a versioned directory without changing shell config."""
import argparse,json,os
from pathlib import Path
import platform,shutil,sys

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--prefix',type=Path,default=Path.home()/'.local/share/vmfree');p.add_argument('--source',type=Path);a=p.parse_args()
    if platform.system()!='Darwin':p.error('This installer targets macOS')
    if sys.version_info<(3,10):p.error('Python 3.10+ required')
    source=a.source or (Path(__file__).resolve().parent if Path(__file__).name=='install.py' else Path(__file__).resolve().parents[1])
    if not (source/'build/dockiva-runtime').is_file():p.error('Package lacks a Mac executable; run package_macos.py first')
    prefix=a.prefix.expanduser().absolute()
    if prefix.is_symlink():p.error('Install prefix cannot be a symlink')
    prefix.mkdir(parents=True,exist_ok=True,mode=0o700)
    target=prefix/'0.17.0'
    if target.exists():p.error('Version 0.17.0 already exists; keep it for rollback or choose another prefix')
    shutil.copytree(source,target,ignore=shutil.ignore_patterns('dist','__pycache__','.git'))
    current=prefix/'current'
    if current.exists() and not current.is_symlink():p.error('current exists and is not an installation symlink')
    temp=prefix/'current.new';temp.symlink_to(target);os.replace(temp,current)
    print('Installed: '+str(target))
    print('Start: '+sys.executable+' '+str(current/'daemon.py'))
    print('State stays in ~/.vmfree. No login item or Docker context was changed.')
if __name__=='__main__':main()
