import unittest
from scripts.verify_m30 import classify
class ClassificationTests(unittest.TestCase):
    def setUp(self):
        self.ok=dict(exit_code=0,stdout='ok\n',stderr='')
        self.other=dict(exit_code=1,stdout='',stderr='denied\n')
    def test_matching_reference(self):
        self.assertEqual(classify(self.ok,self.ok,self.ok),'matched')
    def test_local_is_not_differential_evidence(self):
        self.assertEqual(classify(self.ok,self.ok),'local-only')
    def test_known_gap_requires_both_expected_outcomes(self):
        self.assertEqual(classify(self.other,self.other,self.ok,self.ok,'FS-001'),'declared-policy-gap')
        self.assertEqual(classify(self.ok,self.other,self.ok,self.ok,'FS-001'),'regression')
        self.assertEqual(classify(self.other,self.other,self.other,self.ok,'FS-001'),'reference-regression')
    def test_unclassified_difference_fails(self):
        self.assertEqual(classify(self.other,self.other,self.ok,self.ok),'unclassified-discrepancy')
