"""Probe-harness checks with a fake RESP server, not database acceptance."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
spec=importlib.util.spec_from_file_location('database_probes',Path(__file__).resolve().parents[1]/'scripts/probe_databases.py')
probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)
class StartupProbeTests(unittest.TestCase):
    def test_protocol_roundtrip_and_shutdown(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);server=root/'fake.py'
            server.write_text('''import socket,sys
with socket.socket() as server:
 server.bind(('127.0.0.1',int(sys.argv[sys.argv.index('--port')+1])));server.listen(1)
 print('Ready to accept connections',flush=True)
 client,_=server.accept()
 with client:
  stream=client.makefile('rb')
  for expected,response in [(b'PING',b'+PONG\\r\\n'),(b'SET',b'+OK\\r\\n'),(b'GET',b'$5\\r\\nvalue\\r\\n'),(b'SHUTDOWN',b'')]:
   request=stream.readline();assert request.split()[0]==expected
   client.sendall(response)
  stream.close()
''')
            result=probe.startup_probe([sys.executable,str(server)],root,'redis')
            self.assertTrue(result['ready'],result);self.assertEqual(len(result['queries']),3)
            self.assertEqual(result['process']['exit_code'],0);self.assertNotIn('stopped_by_verifier',result)
    def test_early_failure_is_not_ready(self):
        with tempfile.TemporaryDirectory() as temporary:
            result=probe.startup_probe([sys.executable,'-c','import sys;print("failed",file=sys.stderr);sys.exit(7)'],Path(temporary),'valkey')
            self.assertFalse(result['ready']);self.assertEqual(result['queries'],[])
            self.assertEqual(result['process']['exit_code'],7);self.assertIn('failed',result['process']['stderr'])
