#!/usr/bin/env python3
"""Check backend gate arithmetic using clearly synthetic unit data, never releases."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[2]

class GateTests(unittest.TestCase):
    def test_real_gate_reports_partial(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build/backend-training') as directory:
            output=pathlib.Path(directory)/'gate.json'
            result=subprocess.run([sys.executable,'-B',str(ROOT/'tools/backend/check_backend_release.py'),
                '--build-dir',str(ROOT/'build/backend-service'),'--models',str(ROOT/'build/no-such-models'),
                '--junit',str(ROOT/'build/backend-service/backend-final.xml'),'--output',str(output)],
                capture_output=True,text=True,encoding='utf-8')
            self.assertEqual(result.returncode,1)
            report=json.loads(output.read_text(encoding='utf-8'))
            self.assertFalse(report['stable_ready'])
            for language in ('en','zh','fr','ja','ko'):
                self.assertEqual(next(row for row in report['checks'] if row['id']=='quality-'+language)['status'],'blocked')
    def test_output_outside_build_refused(self):
        result=subprocess.run([sys.executable,'-B',str(ROOT/'tools/backend/check_backend_release.py'),
            '--build-dir',str(ROOT/'build/backend-service'),'--models',str(ROOT/'build/no-such-models'),
            '--junit',str(ROOT/'build/no.xml'),'--output',str(ROOT/'must-not-exist.json')],capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse((ROOT/'must-not-exist.json').exists())

if __name__=='__main__':unittest.main()
