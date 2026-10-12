#!/usr/bin/env python3
"""Alternating, duration-based HTTP benchmarks with optional resource sampling."""
import argparse
from array import array
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import platform
import selectors
import signal
import statistics
import subprocess
import tempfile
import threading
import time
from benchmark_m14 import request, wait_http_ready


def cpu_seconds(value):
    days, sep, clock = value.partition('-')
    total = float(days) * 86400 if sep else 0
    parts = (clock if sep else value).split(':')
    for i, part in enumerate(reversed(parts)):
        total += float(part) * 60 ** i
    return total


def docker_prefix(command):
    # Supported input is the existing benchmark's explicit Docker run array.
    index = command.index('run')
    tail = command[index + 1:]
    name = tail[tail.index('--name') + 1]
    return command[:index], name


def native_command(command):
    command = list(command)
    end = command.index('--') if '--' in command else len(command)
    if '--max-steps' in command[:end]:
        command[command.index('--max-steps') + 1] = '10000000000'
    else:
        command[end:end] = ['--max-steps', '10000000000']
    return command


class Resources:
    def __init__(self, spec, pid):
        self.spec, self.pid = spec, pid
        self.stop = threading.Event()
        self.rows, self.errors = [], []
        self.start = time.monotonic()
        self.thread = threading.Thread(target=self.collect, daemon=True)

    def collect(self):
        try:
            if self.spec['kind'] == 'vmfree':
                while not self.stop.is_set():
                    result = subprocess.run(['ps', '-p', str(self.pid), '-o', 'rss=', '-o', 'time='],
                                            capture_output=True, text=True, timeout=3, check=True)
                    rss, cpu = result.stdout.split()
                    self.rows.append(dict(elapsed_s=time.monotonic() - self.start,
                                          rss_bytes=int(rss) * 1024, cpu_seconds=cpu_seconds(cpu)))
                    self.stop.wait(.5)
            else:
                prefix, name = docker_prefix(self.spec['command'])
                while not self.stop.is_set():
                    result = subprocess.run(prefix + ['stats', '--no-stream', '--format', '{{json .}}', name],
                                            capture_output=True, text=True, timeout=8, check=True)
                    row = json.loads(result.stdout)
                    self.rows.append(dict(elapsed_s=time.monotonic() - self.start,
                                          cpu_percent=float(row['CPUPerc'].rstrip('%')),
                                          memory_usage_and_limit=row['MemUsage']))
                    self.stop.wait(.5)
        except Exception as exc:
            self.errors.append(str(exc))

    def finish(self, stopped_at=None):
        cutoff = (time.monotonic() if stopped_at is None else stopped_at) - self.start
        self.stop.set()
        self.thread.join(timeout=10)
        if self.thread.is_alive():
            self.errors.append('Resource collector did not stop')
        self.rows = [row for row in self.rows if row['elapsed_s'] <= cutoff]
        report = dict(scope='native process' if self.spec['kind'] == 'vmfree' else 'Docker container',
                      samples=self.rows, errors=self.errors,
                      status='ok' if len(self.rows) >= 2 and not self.errors else 'incomplete')
        if self.spec['kind'] == 'vmfree' and len(self.rows) >= 2:
            first, last = self.rows[0], self.rows[-1]
            report['sampled_cpu_percent_one_core'] = 100 * (last['cpu_seconds'] - first['cpu_seconds']) / (last['elapsed_s'] - first['elapsed_s'])
            report['sampled_peak_rss_bytes'] = max(row['rss_bytes'] for row in self.rows)
        return report


def workload(port, seconds, concurrency, rate=None):
    begin = time.monotonic()
    deadline = begin + seconds
    stop = threading.Event()
    def worker(worker_id):
        slot = worker_id
        latencies = array('d')
        try:
            while time.monotonic() < deadline and not stop.is_set():
                if rate:
                    scheduled = begin + slot / rate
                    slot += concurrency
                    if scheduled >= deadline:
                        break
                    delay = scheduled - time.monotonic()
                    if delay > 0 and stop.wait(delay):
                        break
                    # Drop stale slots rather than sending catch-up bursts.
                    if time.monotonic() - scheduled >= concurrency / rate:
                        continue
                latencies.append(request(port))
        except Exception:
            stop.set()
            raise
        return latencies
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(worker, i) for i in range(concurrency)]
        groups = [f.result() for f in futures]
    if rate and time.monotonic() < deadline:
        time.sleep(deadline - time.monotonic())
    elapsed = time.monotonic() - begin
    values = sorted(v for group in groups for v in group)
    if not values:
        raise RuntimeError('No requests completed')
    return dict(target_requests_per_second=rate,
                target_fraction_achieved=(len(values) / elapsed / rate) if rate else None,
                requests=len(values), elapsed_s=elapsed, requests_per_second=len(values) / elapsed,
                latency_median_ms=statistics.median(values),
                latency_p95_ms=values[math.ceil(len(values) * .95) - 1])


def host_identity(pid):
    return subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=', '-o', 'comm='],
                          capture_output=True, text=True, timeout=3, check=True).stdout.strip()


def sample(spec, duration, warmup, concurrency, resources, rate=None, profile=False, host_vm=None):
    command = native_command(spec['command']) if spec['kind'] == 'vmfree' else spec['command']
    if profile and spec['kind'] == 'vmfree':
        end = command.index('--') if '--' in command else len(command)
        command.insert(end, '--profile')
    collector = None
    host_collector = None
    if host_vm and spec['kind'] == 'linux-vm':
        if host_identity(host_vm['pid']) != host_vm['identity']:
            raise RuntimeError('Selected host process changed; rerun --list and select its current PID')
    with tempfile.TemporaryFile() as stderr:
        started = time.monotonic()
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr)
        try:
            pending = b''
            port = None
            with selectors.DefaultSelector() as sel:
                sel.register(proc.stdout, selectors.EVENT_READ)
                while port is None:
                    if time.monotonic() - started > 30:
                        raise RuntimeError('Guest readiness timeout')
                    if not sel.select(.1):
                        continue
                    data = os.read(proc.stdout.fileno(), 512)
                    if not data:
                        stderr.seek(0)
                        raise RuntimeError('Guest exited before readiness: ' + stderr.read(4096).decode(errors='replace'))
                    pending += data
                    if len(pending) > 8192:
                        raise RuntimeError('Too much output before readiness')
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        if line.startswith(b'PORT '):
                            port = spec.get('port') or int(line[5:])
                            break
            attempts = wait_http_ready(proc, port)
            startup = (time.monotonic() - started) * 1000
            if warmup:
                workload(port, warmup, concurrency, rate)
            if resources:
                collector = Resources(spec, proc.pid)
                collector.thread.start()
            if host_vm and spec['kind'] == 'linux-vm':
                host_collector = Resources(dict(kind='vmfree'), host_vm['pid'])
                host_collector.thread.start()
            result = workload(port, duration, concurrency, rate)
            stopped_at = time.monotonic()
            result.update(command=command, startup_to_first_response_ms=startup,
                          readiness_attempts=attempts, warmup_s=warmup, concurrency=concurrency)
            if collector:
                result['resources'] = collector.finish(stopped_at)
                collector = None
            if host_collector:
                host_result = host_collector.finish(stopped_at)
                host_collector = None
                host_result.update(pid=host_vm['pid'], identity=host_vm['identity'],
                                   scope='Selected host VM helper process during Docker load; not complete whole-VM accounting')
                try:
                    if host_identity(host_vm['pid']) != host_vm['identity']:
                        raise RuntimeError('Host process identity changed during sampling')
                except Exception as exc:
                    host_result['errors'].append(str(exc))
                    host_result['status'] = 'incomplete'
                result['host_vm_process'] = host_result
            if profile and spec['kind'] == 'vmfree':
                proc.send_signal(signal.SIGUSR1)
                snapshot = None
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and snapshot is None:
                    for line in os.pread(stderr.fileno(), 65536, 0).splitlines():
                        if line.startswith(b'VMFREE_PROFILE '):
                            try:
                                snapshot = json.loads(line[len(b'VMFREE_PROFILE '):])
                            except json.JSONDecodeError:
                                pass
                    if snapshot is None:
                        time.sleep(.05)
                result['profile'] = dict(status='ok' if snapshot else 'incomplete',
                                         scope='Cumulative startup, warmup, workload and snapshot wait; event counts, not CPU time',
                                         counters=snapshot)
            if proc.poll() is not None:
                raise RuntimeError(f'Backend exited during workload: {proc.returncode}')
            return result
        finally:
            if host_collector:
                host_collector.finish()
            if collector:
                collector.finish()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stdout.close()
            if spec.get('cleanup'):
                subprocess.run(spec['cleanup'], capture_output=True, timeout=15)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--samples', type=int, default=4)
    p.add_argument('--duration', type=float, default=15)
    p.add_argument('--warmup', type=float, default=2)
    p.add_argument('--concurrency', type=int, default=4)
    p.add_argument('--resources', action='store_true')
    p.add_argument('--rate', type=float, help='Total target HTTP requests/sec across all workers')
    p.add_argument('--host-vm-pid', type=int, help='Read-only host process sampling during Docker load only')
    p.add_argument('--profile', action='store_true', help='Opt-in native event counters; requires rebuilt runtime')
    a = p.parse_args()
    if not (2 <= a.samples <= 20 and a.samples % 2 == 0 and 1 <= a.duration <= 60
            and 0 <= a.warmup <= 10 and 1 <= a.concurrency <= 8):
        p.error('Use an even sample count 2..20, duration 1..60s, warmup 0..10s, concurrency 1..8')
    if a.rate is not None and not 1 <= a.rate <= 100000:
        p.error('Rate must be 1..100000 requests/sec')
    host_vm = None
    if a.host_vm_pid is not None:
        if a.host_vm_pid < 1:
            p.error('Host VM PID must be positive')
        host_vm = dict(pid=a.host_vm_pid, identity=host_identity(a.host_vm_pid))
        if not host_vm['identity']:
            p.error('Host VM process identity is empty')
    specs = json.loads(a.config.read_text())
    if len(specs) != 2 or {s['kind'] for s in specs} != {'vmfree', 'linux-vm'}:
        p.error('Config must contain exactly one vmfree and one linux-vm backend')
    for spec in specs:
        if not isinstance(spec['command'], list) or not all(isinstance(v, str) for v in spec['command']):
            p.error('Commands must be argument arrays')
        if a.resources and spec['kind'] == 'linux-vm':
            docker_prefix(spec['command'])
    report = dict(host=platform.platform(), completed=False, resources_enabled=a.resources,
                  target_requests_per_second=a.rate, profiling_enabled=a.profile,
                  whole_vm_resources='not measured completely; selected process proxy only' if host_vm else 'not measured',
                  selected_host_process=host_vm, order=[], results=[],
                  methodology='Alternating AB/BA rounds; fresh launch each sample; warmup excluded from throughput; startup includes launcher and forwarding readiness; resources sampled during load; pending final requests included in elapsed time; native budget 10 billion instructions',
                  limitations='Native RSS and Docker CLI memory are different metrics. Docker stats excludes whole-VM overhead. Resource polling perturbs timings; use the separate uninstrumented report for performance. Python client can limit throughput.')
    rows = {s['kind']: dict(name=s['name'], kind=s['kind'], samples=[]) for s in specs}
    report['results'] = list(rows.values())
    try:
        for round_number in range(a.samples):
            for spec in (specs if round_number % 2 == 0 else list(reversed(specs))):
                report['order'].append(dict(round=round_number + 1, kind=spec['kind']))
                print(f"Starting {spec['name']} {round_number + 1}/{a.samples}", flush=True)
                row = sample(spec, a.duration, a.warmup, a.concurrency, a.resources, a.rate, a.profile, host_vm)
                rows[spec['kind']]['samples'].append(row)
                a.report.write_text(json.dumps(report, indent=2) + '\n')
                print(f"PASS {spec['name']}: {row['requests']} requests, {row['requests_per_second']:.0f} req/s", flush=True)
        report['completed'] = True
        if host_vm:
            report['host_process_collection_completed'] = all(s['host_vm_process']['status'] == 'ok' for s in rows['linux-vm']['samples'])
        if a.rate:
            report['matched_rate_achieved'] = all(s['target_fraction_achieved'] >= .95 for r in rows.values() for s in r['samples'])
        if a.profile:
            report['profile_collection_completed'] = all(s['profile']['status'] == 'ok' for s in rows['vmfree']['samples'])
        if a.resources:
            report['resource_collection_completed'] = all(s['resources']['status'] == 'ok' for r in rows.values() for s in r['samples'])
    except Exception as exc:
        report['error'] = str(exc)
        raise
    finally:
        a.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Report: {a.report}', flush=True)


if __name__ == '__main__':
    main()
