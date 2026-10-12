#!/usr/bin/env python3
"""Compare cache on/off in one native binary using alternating order."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import platform
from benchmark_m14_sustained import sample

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache',choices=['lookup','decode'],default='lookup')
    p.add_argument('--config',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--samples',type=int,default=4)
    p.add_argument('--duration',type=float,default=15)
    p.add_argument('--warmup',type=float,default=2)
    p.add_argument('--concurrency',type=int,default=4)
    p.add_argument('--rate',type=float,default=3000,help='Total target req/s; 0 means unpaced')
    p.add_argument('--resources',action='store_true')
    p.add_argument('--profile',action='store_true')
    a=p.parse_args()
    if not (2<=a.samples<=20 and a.samples%2==0 and 1<=a.duration<=60 and 0<=a.warmup<=10 and 1<=a.concurrency<=8 and 0<=a.rate<=100000):
        p.error('Even samples 2..20; duration 1..60; warmup 0..10; concurrency 1..8; rate 0..100000')
    found=[s for s in json.loads(a.config.read_text()) if s['kind']=='vmfree']
    if len(found)!=1:p.error('Expected one vmfree backend in config')
    cache_flag='--no-'+a.cache+'-cache'
    base=found[0]
    command=list(base['command']);end=command.index('--') if '--' in command else len(command)
    if '--profile' in command[:end]:p.error('Remove --profile from config; use the benchmark flag')
    command=[v for i,v in enumerate(command) if not (i<end and v==cache_flag)]
    variants=[]
    for enabled in (False,True):
        spec=copy.deepcopy(base);spec['command']=list(command)
        spec['name']='Cache on' if enabled else 'Cache off'
        if not enabled:
            end=spec['command'].index('--') if '--' in spec['command'] else len(spec['command'])
            spec['command'].insert(end,cache_flag)
        variants.append(spec)
    report=dict(cache=a.cache,completed=False,host=platform.platform(),native_sha256=hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
                rate=a.rate,resources_enabled=a.resources,profiling_enabled=a.profile,
                methodology='One binary, cache off/on with balanced AB/BA rounds. Warmup excluded from load metrics. Counters include warmup and startup. Native instruction budget 10 billion.',
                results=[dict(name=s['name'],samples=[]) for s in variants],order=[])
    try:
        for round_number in range(a.samples):
            for index in ([0,1] if round_number%2==0 else [1,0]):
                spec=variants[index];print(f"Starting {spec['name']} {round_number+1}/{a.samples}",flush=True)
                report['order'].append(dict(round=round_number+1,name=spec['name']))
                result=sample(spec,a.duration,a.warmup,a.concurrency,a.resources,a.rate or None,a.profile)
                report['results'][index]['samples'].append(result)
                a.report.write_text(json.dumps(report,indent=2)+'\n')
                print(f"PASS {spec['name']}: {result['requests_per_second']:.0f} req/s",flush=True)
        report['completed']=True
        samples=[s for r in report['results'] for s in r['samples']]
        if a.rate:report['matched_rate_achieved']=all(s['target_fraction_achieved']>=.95 for s in samples)
        if a.resources:report['resource_collection_completed']=all(s['resources']['status']=='ok' for s in samples)
        if a.profile:report['profile_collection_completed']=all(s['profile']['status']=='ok' for s in samples)
    except Exception as exc:
        report['error']=str(exc);raise
    finally:a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(a.report)

if __name__=='__main__':main()
