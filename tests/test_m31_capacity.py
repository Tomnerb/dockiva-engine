import json,os,tempfile,unittest
from pathlib import Path
from containers import lock,reserve_capacity
class CapacityTests(unittest.TestCase):
    def test_active_slots_and_release(self):
        with tempfile.TemporaryDirectory() as temp:
            store=Path(temp);held=[]
            try:
                for i in range(5):
                    p=store/str(i);p.mkdir();(p/'config.json').write_text(json.dumps(dict(memory_limit_mib=256)))
                    if i<4:held.append(lock(p))
                with self.assertRaisesRegex(ValueError,'capacity exceeded'):reserve_capacity(store,store/'4')
                os.close(held.pop());gate=reserve_capacity(store,store/'4');os.close(gate)
            finally:
                for fd in held:os.close(fd)
    def test_memory_reservations_and_idle_configs(self):
        with tempfile.TemporaryDirectory() as temp:
            store=Path(temp)
            for name,memory in [('active',900),('candidate',200),('idle',1024)]:
                p=store/name;p.mkdir();(p/'config.json').write_text(json.dumps(dict(memory_limit_mib=memory)))
            fd=lock(store/'active')
            try:
                with self.assertRaisesRegex(ValueError,'capacity exceeded'):reserve_capacity(store,store/'candidate')
                (store/'candidate/config.json').write_text(json.dumps(dict(memory_limit_mib=100)))
                gate=reserve_capacity(store,store/'candidate');os.close(gate)
            finally:os.close(fd)
