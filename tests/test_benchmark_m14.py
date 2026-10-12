import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock,patch

path=Path(__file__).resolve().parents[1]/'scripts/benchmark_m14.py'
spec=importlib.util.spec_from_file_location('benchmark_m14',path)
bench=importlib.util.module_from_spec(spec);spec.loader.exec_module(bench)

class ReadinessTests(unittest.TestCase):
    def test_retries_transient_connections(self):
        proc=Mock();proc.poll.return_value=None
        with patch.object(bench,'request',side_effect=[ConnectionRefusedError('not ready'),ConnectionResetError('reset'),1]) as request:
            self.assertEqual(bench.wait_http_ready(proc,1234,1),3)
            self.assertEqual(request.call_count,3)

    def test_wrong_response_fails(self):
        proc=Mock();proc.poll.return_value=None
        with patch.object(bench,'request',side_effect=ValueError('Wrong response')) as request:
            with self.assertRaises(ValueError):bench.wait_http_ready(proc,1234,1)
            self.assertEqual(request.call_count,1)

    def test_backend_exit_fails(self):
        proc=Mock();proc.poll.return_value=7;proc.returncode=7
        with self.assertRaisesRegex(RuntimeError,'status 7'):bench.wait_http_ready(proc,1234,1)

    def test_timeout_is_bounded(self):
        proc=Mock();proc.poll.return_value=None
        with patch.object(bench,'request',side_effect=ConnectionRefusedError('not ready')):
            with self.assertRaisesRegex(RuntimeError,'Docker port mapping'):bench.wait_http_ready(proc,1234,.02)

    def test_delayed_listener_counts_in_startup(self):
        code='''import socket,time
s=socket.socket();s.bind(('127.0.0.1',0))
print('PORT '+str(s.getsockname()[1]),flush=True)
time.sleep(.2)
s.listen(8)
while True:
 c,_=s.accept()
 with c:
  c.recv(4096)
  c.sendall(b'HTTP/1.1 200 OK\\r\\nConnection: close\\r\\n\\r\\nPersistent VM-free HTTP server OK')
'''
        result=bench.sample({'command':[sys.executable,'-u','-c',code]},2,1)
        self.assertGreaterEqual(result['startup_to_first_response_ms'],190)
        # A connect attempt may block until listen starts on some hosts.
        # Startup latency must include that delay; retries have a separate test.
        self.assertGreaterEqual(result['readiness_attempts'],1)
        self.assertEqual(result['requests'],2)

if __name__=='__main__':unittest.main()
