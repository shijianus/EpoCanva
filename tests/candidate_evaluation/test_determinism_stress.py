import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('stress_candidate_determinism', ROOT / 'tools/backend/stress_candidate_determinism.py')
STRESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STRESS)


class StressControls(unittest.TestCase):
    def test_same_alternating_sequence_in_every_process_is_not_deterministic(self):
        report = STRESS.assess([[b'a', b'b']] * 20, 2)
        self.assertEqual(report['deterministic_case_rate'], 0.0)
        self.assertFalse(report['in_process_deterministic'])

    def test_cross_process_change_fails(self):
        report = STRESS.assess([[b'a'] * 8, [b'b'] * 8], 8)
        self.assertFalse(report['cross_process_deterministic'])

    def test_complete_identical_matrix(self):
        report = STRESS.assess([[b'a'] * 8 for _ in range(20)], 8)
        self.assertEqual(report['queries'], 160)
        self.assertEqual(report['deterministic_case_rate'], 1.0)

    def test_incomplete_matrix_rejected(self):
        with self.assertRaises(ValueError):
            STRESS.assess([[b'a'] * 7], 8)

    def test_artifact_containment(self):
        for path in (ROOT / 'outside.json', ROOT / 'build/../outside.json', ROOT.parent / 'outside.json'):
            with self.assertRaises(ValueError):
                STRESS.confined(path)
        self.assertEqual(STRESS.confined(ROOT / 'build/stress/result.json'), ROOT / 'build/stress/result.json')


if __name__ == '__main__':
    unittest.main()
