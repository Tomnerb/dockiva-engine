"""Recovery verifier failure detection; mocked I/O is not Redis acceptance."""
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import MagicMock,patch

SCRIPTS=Path(__file__).resolve().parents[1]/'scripts'
sys.path.insert(0,str(SCRIPTS))
try:
    import verify_redis_recovery as recovery
    import verify_redis_preset as acceptance
finally:
    sys.path.pop(0)

class RecoveryVerifierTests(unittest.TestCase):
    def verify_replies(self,replies):
        client=MagicMock();client.__enter__.return_value=client
        with patch.object(recovery.socket,'create_connection',return_value=client),patch.object(recovery,'request',side_effect=replies):
            return recovery.verify_values(12345,{'acknowledged-key':'expected-value'})

    def test_missing_acknowledged_key_fails(self):
        with self.assertRaisesRegex(RuntimeError,'Lost or corrupted acknowledged key'):
            self.verify_replies(['PONG',None])

    def test_corrupted_acknowledged_value_fails(self):
        with self.assertRaisesRegex(RuntimeError,'Lost or corrupted acknowledged key'):
            self.verify_replies(['PONG','wrong-value'])

    def test_exact_recovered_value_passes(self):
        self.assertEqual(self.verify_replies(['PONG','expected-value']),1)

    def test_early_runtime_exit_fails(self):
        result=recovery.run_cycle([sys.executable,'-c','raise SystemExit(7)'],12345,{},'final_recovery',0,4,25,0)
        self.assertFalse(result['passed']);self.assertEqual(result['exit'],7)
        self.assertIn('readiness failed',result['error'])

    def test_resp_counter_replies_are_integers(self):
        self.assertEqual(acceptance.request(MagicMock(),io.BytesIO(b':42\r\n'),'INCR','key'),42)
        with self.assertRaises(ValueError):
            acceptance.request(MagicMock(),io.BytesIO(b':bad\r\n'),'INCR','key')
