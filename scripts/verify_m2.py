#!/usr/bin/env python3
"""Compare the compiled interpreter with M1 probes and bounded failure cases."""
import argparse
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from examples.build_hello import build
from scripts.verify_m1 import EXPECTED, capture, serialize, verify


def check(native):
    baseline=verify()  # Validates M1 source/binary hashes and Python results.
    rows=[]
    for name, expected in EXPECTED.items():
        result=capture([str(native),'run',str(ROOT/'examples/independent'/f'{name}.elf')])
        rows.append(dict(name=name,passed=result==expected,actual=serialize(result)))
    original=build()
    cases={}
    for n in [0,6,63,90,len(original)-1]: cases[f'truncated_{n}']=original[:n]
    def patch(name,offset,fmt,value):
        data=bytearray(original);struct.pack_into(fmt,data,offset,value);cases[name]=data
    patch('wrong_arch',18,'<H',62)
    patch('phoff_overflow',32,'<Q',2**64-1)
    patch('oversize_memory',104,'<Q',32*1024*1024)
    patch('nonexec_entry',68,'<I',4)
    patch('unknown_instruction',0x100,'<I',0xffffffff)
    patch('unknown_syscall',0x10c,'<I',0xd2800008|(999<<5))
    patch('loop_budget',0x100,'<I',0x14000000)
    patch('backward_branch',0x104,'<I',0x17ffffff)
    patch('reserved_movz',0x100,'<I',0x52c00000)
    with tempfile.TemporaryDirectory() as tmp:
        for name,data in cases.items():
            elf=Path(tmp)/'probe.elf';elf.write_bytes(data)
            args=['run',str(elf),'--max-steps','1000']
            py=capture([sys.executable,str(ROOT/'runtime.py'),*args])
            c=capture([str(native),*args])
            # Diagnostics differ deliberately; errors must be bounded and explicit.
            passed=py[2]==c[2]==125 and py[0]==c[0]==b'' and bool(c[1])
            rows.append(dict(name=name,passed=passed,actual=serialize(c)))
    return dict(host=platform.platform(),native=str(native),passed=baseline['passed'] and all(r['passed'] for r in rows),cases=rows)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=ROOT/'build/dockiva-runtime')
    p.add_argument('--report',type=Path)
    args=p.parse_args()
    try:
        report=check(args.native.resolve())
        for row in report['cases']:print(('PASS ' if row['passed'] else 'FAIL ')+row['name'])
        if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
        return 0 if report['passed'] else 1
    except (OSError,ValueError,subprocess.TimeoutExpired) as exc:
        print(f'Verification failed: {exc}',file=sys.stderr);return 1

if __name__=='__main__':sys.exit(main())
