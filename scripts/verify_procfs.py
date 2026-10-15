#!/usr/bin/env python3
"""Check the synthetic /proc with busybox top, ps, free and uptime (alpine) under the
hvf runtime: a CPU-bound process shows its CPU time, memory and the container's
totals are consistent, and an exec run (--proc-peer) sees the main run's processes.
--root: an extracted alpine root; by default alpine:latest is extracted from
$DOCKER_HOST (or the CLI test engine) into build/procfs-alpine."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MEMORY_MIB = 256


def alpine():
    root = ROOT/'build/procfs-alpine'
    if (root/'bin/busybox').exists():
        return root
    env = dict(os.environ)
    env.setdefault('DOCKER_HOST', 'unix://'+str(Path.home()/'.vmfree-clitest/docker.sock'))
    with tempfile.TemporaryDirectory() as temp:
        image = Path(temp)/'alpine.tar'
        subprocess.run(['docker', 'save', 'alpine:latest', '-o', str(image)], check=True, env=env, timeout=120)
        with tarfile.open(image) as outer:
            outer.extractall(temp, filter='data')
        root.mkdir(parents=True)
        for layer in json.loads((Path(temp)/'manifest.json').read_text())[0]['Layers']:
            with tarfile.open(Path(temp)/layer) as inner:
                inner.extractall(root, filter='fully_trusted')
    return root


def run(root, script, *extra, background=False):
    command = [str(ROOT/'build/dockiva-hvf'), 'run', str(root/'bin/busybox'), '--root', str(root), '--dynamic', '--cpu-backend', 'hvf',
               '--process-tree', '--scheduled-network', '--cpus', '4', '--memory-limit-mib', str(MEMORY_MIB), '--elf-limit-mib', '64',
               '--mmap-limit-mib', '2048', '--env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin', *extra, '--argv0', 'sh', '--', '-c', script]
    if background:
        return subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    r = subprocess.run(command, capture_output=True, timeout=60)
    assert r.returncode == 0, (script, r.returncode, r.stderr.decode())
    return r.stdout.decode()


def section(text, name):
    return text.split('=='+name+'==\n')[1].split('\n==')[0]


def seconds(value):  # [[dd-]hh:]mm:ss
    total = 0
    for part in value.replace('-', ':').split(':'):
        total = total*60+int(part)
    return total


def check_own(out):
    top = section(out, 'top')
    assert re.search(r'^Mem: \d+K used, \d+K free', top, re.M) and re.search(r'^CPU: .*idle', top, re.M), top
    assert re.search(r'^Load average: [\d.]+ [\d.]+ [\d.]+ \d+/\d+ \d+$', top, re.M), top
    busy = re.search(r'^CPU:\s+(\d+)% usr\s+(\d+)% sys', top, re.M)
    assert int(busy[1])+int(busy[2]) >= 15, top           # one of 4 CPUs is busy: about 25%
    row = [line for line in top.splitlines() if line.endswith(' yes')]
    assert row and int(row[0].split()[-2].rstrip('%')) >= 15, top
    ps = [line.split(None, 4) for line in section(out, 'ps').splitlines()[1:]]
    yes = [r for r in ps if r[4] == 'yes'][0]
    assert seconds(yes[1]) >= 1 and int(yes[2]) > 0 and seconds(yes[3]) >= 2, ps   # TIME, RSS, ELAPSED
    shell = [r for r in ps if r[0] == '1'][0]
    assert seconds(shell[1]) == 0, ps                      # a sleeping shell used no CPU
    free = section(out, 'free').splitlines()
    total, used, unused = (int(v) for v in free[1].split()[1:4])
    assert total == MEMORY_MIB*1024 and 0 < used < total and used+unused == total and '-/+' not in '\n'.join(free), free
    meminfo = dict(line.split(':') for line in section(out, 'meminfo').splitlines())
    assert int(meminfo['MemTotal'].split()[0]) == total and 'SReclaimable' in meminfo, meminfo
    assert re.search(r'up \d+ min, .*load average: [\d.]+, [\d.]+, [\d.]+', section(out, 'uptime')), out
    stat = section(out, 'stat').splitlines()
    cpus = [line for line in stat if re.match(r'cpu\d', line)]
    assert len(stat[0].split()) == 11 and len(cpus) == 4 and any(line.startswith('btime ') for line in stat), stat
    first, second = (sum(int(v) for v in line.split()[1:4]) for line in (stat[0], section(out, 'stat2').splitlines()[0]))
    assert second > first, (stat[0], section(out, 'stat2'))  # top and vmstat read deltas: totals grow
    assert section(out, 'relative').startswith('cpu '), out # busybox top chdirs to /proc and opens "stat"
    assert section(out, 'task').split() == ['1'], out
    assert section(out, 'exe').strip() == 'symbolic link', out  # lstat: libstdc++'s read_symlink (FEXServer) checks
    print('PASS own run: top, ps, free, uptime, /proc/stat, relative paths, task, lstat of /proc/self/exe')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path)
    a = p.parse_args()
    root = a.root or alpine()
    script = ('yes >/dev/null & sleep 2; echo ==top==; top -b -n1; echo ==ps==; ps -o pid,time,rss,etime,args; '
              'echo ==free==; free; echo ==meminfo==; cat /proc/meminfo; echo ==uptime==; uptime; '
              'echo ==stat==; cat /proc/stat; sleep 0.3; echo ==stat2==; cat /proc/stat; '
              'echo ==relative==; cd /proc && head -1 stat; cd /; echo ==task==; ls /proc/1/task; '
              'echo ==exe==; stat -c %F /proc/self/exe; echo ==; kill $!')
    out = run(root, script)
    check_own(out)
    # Where Linux on arm64 (no randomization) loads a PIE program and its dynamic loader;
    # the brk heap follows the program.
    maps = [line.split() for line in run(root, 'cat /proc/self/maps').splitlines()]
    ranges = [tuple(int(v, 16) for v in m[0].split('-')) for m in maps]
    assert ranges[maps.index(next(m for m in maps if m[1] == 'r-xp' and m[-1] != '[vdso]'))][0] == 0xaaaaaaaa0000, maps
    assert any(0xfffff0000000 <= low and high <= 0xfffff8000000 and m[1] == 'r-xp' for (low, high), m in zip(ranges, maps)), maps
    heap = ranges[maps.index(next(m for m in maps if m[-1] == '[heap]'))]
    assert any(high == heap[0] and 0xaaaaaaaa0000 <= low for low, high in ranges), maps
    print('PASS PIE program at 0xaaaaaaaa0000, ld-musl below 0xfffff8000000, heap after the program')
    with tempfile.TemporaryDirectory() as temp:
        table = Path(temp)/'procs'
        main_run = run(root, 'yes >/dev/null & sleep 6; kill $!', '--proc-publish', str(table), background=True)
        try:
            time.sleep(3)
            out = run(root, 'echo ==ps==; ps -o pid,time,rss,etime,args; echo ==uptime==; cat /proc/uptime; echo ==top==; top -b -n1; echo ==self==; echo $$; cut -d" " -f1 /proc/$$/stat; readlink /proc/$$/exe; readlink /proc/self/exe; echo ==', '--proc-peer', str(table))
        finally:
            main_run.wait(timeout=30)
        ps = [line.split(None, 4) for line in section(out, 'ps').splitlines()[1:]]
        yes = [r for r in ps if r[4] == 'yes']
        # The main run's processes at 10000+pid; this run's own under the pids getpid returns.
        assert yes and yes[0][0] == '10002' and seconds(yes[0][1]) >= 1 and int(yes[0][2]) > 0 and seconds(yes[0][3]) >= 2, ps
        assert any(r[0] == '1' and r[4].startswith('sh -c echo') for r in ps), ps
        own = section(out, 'self').split()
        assert own[0] == own[1] == '1' and own[2] == own[3] == '/bin/busybox', own   # /proc/<getpid()> is the caller (.NET)
        assert float(section(out, 'uptime').split()[0]) >= 2, out  # the container's uptime, not this run's
        row = [line for line in section(out, 'top').splitlines() if line.endswith(' yes')]
        assert row and int(row[0].split()[-2].rstrip('%')) >= 10, out   # top's 100 ms first frame sees the main run's usage
    print('PASS exec run: the main run\'s processes with their CPU time (ps, top), memory and the container\'s uptime; /proc/<getpid()> is the caller')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
