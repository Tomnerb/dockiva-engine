import unittest
from scripts.verify_m1 import verify


class IndependentELFTests(unittest.TestCase):
    def test_compiler_built_probes(self):
        report = verify()
        for case in report['cases']:
            with self.subTest(probe=case['name']):
                self.assertTrue(case['passed'], case)


if __name__ == '__main__':
    unittest.main()
