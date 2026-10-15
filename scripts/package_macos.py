#!/usr/bin/env python3
"""Build an unsigned or Developer-ID signed macOS CLI distribution archive."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import zipfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,default=ROOT/'dist/vmfree-macos.zip');p.add_argument('--identity',help='Developer ID Application signing identity');p.add_argument('--notary-profile',help='Existing notarytool keychain profile; requires --identity');a=p.parse_args()
    if platform.system()!='Darwin':p.error('Run packaging on macOS')
    if a.notary_profile and not a.identity:p.error('Notarization requires a signing identity')
    subprocess.run(['python3',str(ROOT/'scripts/build_native.py')],check=True)
    subprocess.run(['python3',str(ROOT/'scripts/verify_m11_17.py'),'--report',str(ROOT/'dist-validation.json')],check=True)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        stage=Path(tmp)/'vmfree';stage.mkdir()
        for item in ('native','scripts','examples','third_party','docs','tests'):
            shutil.copytree(ROOT/item,stage/item,ignore=shutil.ignore_patterns('__pycache__'))
        for pth in ROOT.glob('*.py'):shutil.copy2(pth,stage/pth.name)
        for name in ('README.md','VALIDATION.md'):shutil.copy2(ROOT/name,stage/name)
        (stage/'build').mkdir();shutil.copy2(ROOT/'build/dockiva-runtime',stage/'build/dockiva-runtime')
        shutil.copy2(ROOT/'scripts/install_macos.py',stage/'install.py')
        if a.identity:
            subprocess.run(['codesign','--force','--options','runtime','--timestamp','--sign',a.identity,str(stage/'build/dockiva-runtime')],check=True)
            subprocess.run(['codesign','--verify','--strict','--verbose=2',str(stage/'build/dockiva-runtime')],check=True)
        (stage/'distribution.json').write_text(json.dumps(dict(platform=platform.platform(),signed=bool(a.identity),requires='Python 3.10+ installed by the user',version='0.17.0'),indent=2))
        subprocess.run(['ditto','-c','-k','--keepParent',str(stage),str(a.output.resolve())],check=True)
    if a.notary_profile:
        # ZIP submissions can be notarized but cannot be stapled as ZIP files.
        result=subprocess.run(['xcrun','notarytool','submit',str(a.output.resolve()),'--keychain-profile',a.notary_profile,'--wait','--output-format','json'],check=True,capture_output=True,text=True)
        receipt=json.loads(result.stdout);a.output.with_suffix('.notary.json').write_text(json.dumps(receipt,indent=2)+'\n')
        if receipt.get('status')!='Accepted':raise SystemExit('Notarization was not accepted; inspect the saved receipt')
    print(a.output.resolve())
if __name__=='__main__':main()
