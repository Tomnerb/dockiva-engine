import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import benchmark_m14_sustained as bench

SERVER = '''import socket
s=socket.socket();s.bind(('127.0.0.1',0));s.listen(8)
print('PORT '+str(s.getsockname()[1]),flush=True)
while True:
 c,_=s.accept()
 with c:
  c.recv(4096)
  c.sendall(b'HTTP/1.1 200 OK\\r\\nConnection: close\\r\\n\\r\\nPersistent VM-free HTTP server OK')
'''

class SustainedTests(unittest.TestCase):
    def test_host_process_sampling_during_docker_load(self):
        spec = dict(kind='linux-vm', command=[sys.executable, '-u', '-c', SERVER])
        seen = []
        def run(command, **kwargs):
            seen.append(command)
            self.assertEqual(command, ['ps', '-p', '47448', '-o', 'rss=', '-o', 'time='])
            return subprocess.CompletedProcess(command, 0, '8192 00:00.20')
        with patch.object(bench, 'host_identity', return_value='start-time OrbStack Helper'), patch.object(bench.subprocess, 'run', side_effect=run):
            result = bench.sample(spec, .7, .05, 2, False, 20, False,
                                  dict(pid=47448, identity='start-time OrbStack Helper'))
        self.assertEqual(result['host_vm_process']['status'], 'ok')
        self.assertEqual(result['host_vm_process']['pid'], 47448)
        self.assertGreaterEqual(len(seen), 2)
        self.assertNotIn('resources', result)

    def test_replaced_host_pid_rejected_before_launch(self):
        with patch.object(bench, 'host_identity', return_value='different start-time'), patch.object(bench.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(RuntimeError, 'process changed'):
                bench.sample(dict(kind='linux-vm', command=['docker', 'run', 'image']), 1, 0, 1, False,
                             host_vm=dict(pid=42, identity='old start-time'))
            launch.assert_not_called()

    def test_fixed_rate_is_paced(self):
        with patch.object(bench, 'request', return_value=.1):
            result = bench.workload(1234, .3, 2, 20)
        self.assertEqual(result['requests'], 6)
        self.assertEqual(result['target_requests_per_second'], 20)
        self.assertGreaterEqual(result['elapsed_s'], .29)

    def test_cpu_clock_formats(self):
        self.assertEqual(bench.cpu_seconds('01:02.50'), 62.5)
        self.assertEqual(bench.cpu_seconds('02:01:02'), 7262)
        self.assertEqual(bench.cpu_seconds('1-02:01:02'), 93662)

    def test_budget_does_not_edit_guest_arguments(self):
        command = ['runtime', 'run', 'server', '--', '--max-steps', '5']
        result = bench.native_command(command)
        self.assertEqual(result, ['runtime', 'run', 'server', '--max-steps', '10000000000', '--', '--max-steps', '5'])
        self.assertEqual(command[-1], '5')

    def test_duration_and_native_resources(self):
        spec = dict(kind='vmfree', command=[sys.executable, '-u', '-c', SERVER])
        real_run = subprocess.run
        def run(command, **kwargs):
            if command[0] == 'ps':
                return subprocess.CompletedProcess(command, 0, '2048 00:00.10')
            return real_run(command, **kwargs)
        with patch.object(bench.subprocess, 'run', side_effect=run):
            result = bench.sample(spec, 1.3, .05, 2, True)
        self.assertGreaterEqual(result['elapsed_s'], 1.3)
        self.assertGreater(result['requests'], 2)
        self.assertEqual(result['resources']['status'], 'ok')
        self.assertGreater(result['resources']['sampled_peak_rss_bytes'], 0)

    def test_bad_http_stops_workload(self):
        with patch.object(bench, 'request', side_effect=ValueError('invalid HTTP')):
            with self.assertRaisesRegex(ValueError, 'invalid HTTP'):
                bench.workload(12345, 10, 4)

    def test_docker_stats_preserves_context(self):
        spec = dict(kind='linux-vm', command=['docker', '--context', 'orbstack', 'run', '--rm', '--name', 'benchmark', 'image'])
        collector = bench.Resources(spec, 42)
        def stats(command, **kwargs):
            self.assertEqual(command, ['docker', '--context', 'orbstack', 'stats', '--no-stream', '--format', '{{json .}}', 'benchmark'])
            collector.stop.set()
            return subprocess.CompletedProcess(command, 0, json.dumps(dict(CPUPerc='12.5%', MemUsage='2MiB / 8GiB')))
        with patch.object(bench.subprocess, 'run', side_effect=stats):
            collector.collect()
        self.assertEqual(collector.rows[0]['cpu_percent'], 12.5)
        self.assertEqual(collector.rows[0]['memory_usage_and_limit'], '2MiB / 8GiB')

    def test_resource_failure_is_explicit(self):
        collector = bench.Resources(dict(kind='vmfree'), 42)
        with patch.object(bench.subprocess, 'run', side_effect=RuntimeError('ps failed')):
            collector.thread.start()
            collector.thread.join(timeout=2)
            result = collector.finish()
        self.assertEqual(result['status'], 'incomplete')
        self.assertTrue(result['errors'])

if __name__ == '__main__':
    unittest.main()
