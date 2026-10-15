#!/usr/bin/env python3
"""Host-local microbenchmarks: Python versus C, not Docker or VM performance."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import resource
import statistics
import struct
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from examples.build_hello import build


def worker(command):
    # Each worker creates just one measured child, avoiding cumulative RSS across samples.
    start=time.perf_counter()
    result=subprocess.run(command,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=30)
    wall=time.perf_counter()-start
    usage=resource.getrusage(resource.RUSAGE_CHILDREN)
    rss=usage.ru_maxrss/(1024 if platform.system()=='Darwin' else 1)
    return dict(wall_ms=wall*1000,cpu_ms=(usage.ru_utime+usage.ru_stime)*1000,
                peak_rss_kib=rss,status=result.returncode,stderr=result.stderr.decode(errors='replace'))


def main():
    if len(sys.argv)>1 and sys.argv[1]=='--worker':
        print(json.dumps(worker(sys.argv[2:])));return 0
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--samples',type=int,default=7)
    p.add_argument('--steps',type=int,default=500000)
    p.add_argument('--report',type=Path,default=Path('m2-benchmark.json'))
    args=p.parse_args()
    if not 3<=args.samples<=100 or not 1000<=args.steps<=10000000:p.error('Use 3..100 samples and 1000..10000000 steps')
    native=ROOT/'build/dockiva-runtime'
    if not native.exists():p.error('Run python3 scripts/build_native.py first')
    results={}
    with tempfile.TemporaryDirectory() as tmp:
        loop=bytearray(build());struct.pack_into('<I',loop,0x100,0x14000000)
        path=Path(tmp)/'loop.elf';path.write_bytes(loop)
        workloads={'startup_exit':(ROOT/'examples/independent/exit_group.elf',42,100000),
                   'branch_dispatch':(path,125,args.steps)}
        for workload,(elf,status,steps) in workloads.items():
            commands={'python':[sys.executable,str(ROOT/'runtime.py')], 'c':[str(native)]}
            samples={key:[] for key in commands}
            # One warmup per backend, then alternate ordering to reduce ordering bias.
            for iteration in range(args.samples+1):
                order=list(commands) if iteration%2==0 else list(reversed(commands))
                for key in order:
                    command=commands[key]+['run',str(elf),'--max-steps',str(steps)]
                    raw=subprocess.check_output([sys.executable,str(Path(__file__).resolve()),'--worker',*command],text=True,timeout=40)
                    sample=json.loads(raw)
                    if sample['status']!=status or (status==125 and 'budget exhausted' not in sample['stderr']):
                        raise RuntimeError(f'{key}/{workload}: unexpected result {sample}')
                    if iteration:samples[key].append(sample)
            results[workload]={key:{'samples':data,**{f'median_{metric}':statistics.median(x[metric] for x in data) for metric in ('wall_ms','cpu_ms','peak_rss_kib')}} for key,data in samples.items()}
            results[workload]['python_over_c_wall_ratio']=results[workload]['python']['median_wall_ms']/results[workload]['c']['median_wall_ms']
    report=dict(host=platform.platform(),python=sys.version,samples=args.samples,branch_steps=args.steps,
                native_sha256=hashlib.sha256(native.read_bytes()).hexdigest(),
                build=json.loads((ROOT/'build/dockiva-runtime.json').read_text()),workloads=results,
                limitations=['Synthetic microbenchmarks, not Docker workloads or native ARM64 application execution.',
                'Wall time includes process launch, ELF loading and teardown; branch budget exhaustion is deliberate.',
                'CPU and peak RSS are OS child-process accounting; inherited spawn memory can set an RSS floor.',
                'Warm runs; measurements include scheduler noise. No VM baseline or JIT comparison.'])
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    for name,data in results.items():
        print(f"{name}: Python {data['python']['median_wall_ms']:.3f} ms | C {data['c']['median_wall_ms']:.3f} ms | ratio {data['python_over_c_wall_ratio']:.2f}x")
    print(f'Report: {args.report}')
    return 0

if __name__=='__main__':sys.exit(main())
