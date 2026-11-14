"""Synthetic parser regression; these fixtures are never acceptance evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("verify_root_junit", ROOT / "tools/ci/verify_root_junit.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RootJUnitTests(unittest.TestCase):
    def test_exact_execution_and_fail_closed_variants(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build", prefix="root-junit-unit-") as temporary:
            directory = Path(temporary)
            registered = directory / "registered.json"
            junit = directory / "ctest.xml"
            registered.write_text(json.dumps({"tests": [{"name": "unit.a"}]}), encoding="utf-8")
            passing = '<testsuite tests="1" failures="0"><testcase name="unit.a" status="run"/></testsuite>'
            junit.write_text(passing, encoding="utf-8")
            self.assertEqual(module.verify(registered, junit), 1)
            for text in (
                '<testsuite tests="1"><testcase name="unit.a"><skipped/></testcase></testsuite>',
                '<testsuite tests="1"><testcase name="unit.a"><failure/></testcase></testsuite>',
                '<testsuite tests="1"><testcase name="unit.b"/></testsuite>',
                '<testsuite tests="2"><testcase name="unit.a"/></testsuite>',
                '<testsuite tests="1"><testcase name="unit.a" status="notrun"/></testsuite>',
                '<!DOCTYPE testsuite><testsuite tests="1"><testcase name="unit.a"/></testsuite>',
                '<testsuite tests="2"><testcase name="unit.a"/><testcase name="unit.a"/></testsuite>',
            ):
                with self.subTest(text=text):
                    junit.write_text(text, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        module.verify(registered, junit)


if __name__ == "__main__":
    unittest.main()
