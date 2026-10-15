#!/usr/bin/env python3
"""Validate M3 C guest behavior and optionally compare successful programs with Linux."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]

def run(command,env=None):
    p=subprocess.run(command,capture_output=True,timeout=10,env=env)
    return p.stdout,p.stderr,p.returncode

def encoded(r):return dict(stdout_hex=r[0].hex(),stderr_hex=r[1].hex(),exit_code=r[2])

def verify(native,reference=None):
    directory=ROOT/'examples/m3';manifest=json.loads((directory/'build-manifest.json').read_text())
    for name,digest in manifest['sources'].items():
        if hashlib.sha256((directory/name).read_bytes()).hexdigest()!=digest:raise ValueError('Source hash mismatch: '+name)
    for f in manifest['files']:
        if hashlib.sha256((directory/f['name']).read_bytes()).hexdigest()!=f['sha256']:raise ValueError('ELF hash mismatch: '+f['name'])
    cases=[]
    def case(name,elf,args,env,expected,fault=False):
        path=directory/(elf+'.elf');command=[str(native),'run',str(path)]
        for k,v in env.items():command+=['--env',k+'='+v]
        command+=['--',*args]
        actual=run(command)
        passed=(actual[0]==b'' and actual[2]==125 and b'vmfree:' in actual[1]) if fault else actual==expected
        row=dict(name=name,elf=path.name,args=args,env=env,actual=encoded(actual),passed=passed)
        if expected is not None:row['expected']=encoded(expected)
        if reference is not None and not fault:
            ref=run([*reference,str(path),*args],env=env)
            row['linux_reference']=encoded(ref);row['passed']=passed and ref==expected and ref==actual
        elif fault:row['linux_reference']='not-run: interpreter rejection check'
        cases.append(row)
    usage=(b'usage: demo DIGITS\n',b'',2)
    invalid=(b'error: use 1-9 digits\n',b'',3)
    case('no_arguments','demo',[],{},usage)
    case('digits_and_environment','demo',['12345'],{'MODE':'demo'},(b'sum=15\nMODE=demo\n',b'',15))
    case('zero','demo',['0'],{},(b'sum=00\n',b'',0))
    case('maximum_sum','demo',['999999999'],{},(b'sum=81\n',b'',81))
    case('leading_zeroes','demo',['0012'],{},(b'sum=03\n',b'',3))
    case('invalid_character','demo',['1x'],{},invalid)
    case('empty_argument','demo',[''],{},invalid)
    case('too_many_digits','demo',['1234567890'],{},invalid)
    case('extra_argument','demo',['1','2'],{},usage)
    case('non_ascii','demo',['ខ្មែរ'],{},invalid)
    case('environment_limit','demo',['1'],{'VALUE':'a'*160},(b'sum=01\n'+(b'VALUE='+b'a'*160)[:128]+b'\n',b'',1))
    case('empty_environment_value','demo',['9'],{'MODE':''},(b'sum=09\nMODE=\n',b'',9))
    for name in ['flags','memory','startup','control']:case(name,name,[],{},(b'',b'',0))
    for name in ['readonly_fault','stack_exec_fault','unmapped_fault','unaligned_sp']:case(name,name,[],{},None,True)
    case('argument_count_limit','demo',['x']*64,{},None,True)
    case('startup_bytes_limit','demo',['1'],{'BIG':'a'*65536},None,True)
    # Avoid retaining the large synthetic limit value in the report.
    cases[-1]['env']={'BIG':'<65536 test bytes>'}
    return dict(host=platform.platform(),native=str(native),reference=reference,passed=all(c['passed'] for c in cases),cases=cases)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime')
    g=p.add_mutually_exclusive_group();g.add_argument('--qemu');g.add_argument('--native-linux',action='store_true')
    p.add_argument('--report',type=Path);a=p.parse_args()
    if a.native_linux and (platform.system()!='Linux' or platform.machine() not in ('aarch64','arm64')):p.error('--native-linux requires ARM64 Linux')
    if a.qemu and platform.system()!='Linux':p.error('--qemu requires Linux')
    try:
        report=verify(a.native.resolve(),[] if a.native_linux else [a.qemu] if a.qemu else None)
        report['reference_kind']='native-arm64-linux' if a.native_linux else 'qemu-user-linux' if a.qemu else 'not-run'
        for r in report['cases']:print(('PASS ' if r['passed'] else 'FAIL ')+r['name'])
        print('Linux reference: '+report['reference_kind'])
        if a.report:a.report.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
        return 0 if report['passed'] else 1
    except (OSError,ValueError,KeyError,subprocess.TimeoutExpired) as exc:
        print('Verification failed: '+str(exc),file=sys.stderr);return 1
if __name__=='__main__':sys.exit(main())
