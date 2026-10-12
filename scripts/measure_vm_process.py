#!/usr/bin/env python3
"""Inspect VM process candidates or sample one explicit host PID, read-only."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import time
from benchmark_m14_sustained import Resources

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--list', action='store_true')
p.add_argument('--pid', type=int)
p.add_argument('--duration', type=float, default=60)
p.add_argument('--report', type=Path)
a = p.parse_args()
if a.list:
    result = subprocess.run(['ps', '-axo', 'pid=,comm='], capture_output=True, text=True, check=True)
    for line in result.stdout.splitlines():
        if any(word in line.lower() for word in ('orbstack', 'orb', 'qemu', 'virtualization', 'docker', 'vmm')):
            print(line)
    raise SystemExit(0)
if not a.pid or a.pid < 1 or not a.report or not 2 <= a.duration <= 600:
    p.error('Use --list or --pid PID --duration 2..600 --report FILE')
identity = subprocess.run(['ps', '-p', str(a.pid), '-o', 'comm='], capture_output=True, text=True, check=True).stdout.strip()
collector = Resources(dict(kind='vmfree'), a.pid)
collector.thread.start()
try:
    time.sleep(a.duration)
finally:
    resources = collector.finish()
    resources['scope'] = 'Explicit selected host process; not automatically whole-VM accounting'
    report = dict(host=platform.platform(), pid=a.pid, process=identity,
                  duration_s=a.duration, resources=resources,
                  limitations='Includes other activity in this process. RSS may not include all VM-backed memory or helper processes. CPU/RSS are host process counters, not guest/container counters. PID identity must be checked by the operator.')
    a.report.write_text(json.dumps(report, indent=2) + '\n')
print(a.report)
