"""Real Git + sequential CTest proof; synthetic config contains no credentials."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ci'))
from preserve_git_environment import preserve


class GitEnvironmentTests(unittest.TestCase):
    def environment(self):
        env = dict(os.environ)
        # Isolate synthetic command-scope data; the real inherited configuration
        # is neither inspected nor logged, and the parent is untouched.
        env.update(GIT_CONFIG_COUNT='5', GIT_CONFIG_PARAMETERS="'epoci.order=old'",
            GIT_CONFIG_KEY_0='credential.helper', GIT_CONFIG_VALUE_0='',
            GIT_CONFIG_KEY_1='epoci.value', GIT_CONFIG_VALUE_1="quote' slash\\ space\nline",
            GIT_CONFIG_KEY_2='epoci.order', GIT_CONFIG_VALUE_2='middle',
            GIT_CONFIG_KEY_3='epoci.order', GIT_CONFIG_VALUE_3='last',
            GIT_CONFIG_KEY_4='epoci.empty', GIT_CONFIG_VALUE_4='')
        return env

    def test_real_git_retains_empty_quoted_values_and_order(self):
        original = self.environment()
        saved = dict(original)
        transformed = preserve(original)
        self.assertEqual(original, saved)
        for key, expected in [('epoci.empty', b'\n'), ('epoci.order', b'middle\nlast\nold\n'),
                              ('epoci.value', b"quote' slash\\ space\nline\n")]:
            result = subprocess.run(['git', 'config', '--get-all', key], env=transformed, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, expected)
            before = subprocess.run(['git', 'config', '--get-all', key], env=original, capture_output=True)
            self.assertEqual(before.returncode, result.returncode)
            self.assertEqual(before.stdout, result.stdout)

    def test_missing_value_is_never_reinvented_as_empty(self):
        env = self.environment()
        env.pop('GIT_CONFIG_VALUE_0')
        with self.assertRaises(ValueError):
            preserve(env)

    def test_sequential_ctest_environment_restore_keeps_effective_git_config(self):
        ctest = os.environ.get('EPOCANVA_CTEST')
        self.assertTrue(ctest and Path(ctest).is_file(), 'Actual CTest is required')
        work = ROOT / 'build/ci-env-tests'
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            directory = Path(directory)
            child = directory / 'child.py'
            child.write_text("import subprocess\n"
                "for k,v in [('epoci.empty',b'\\n'),('epoci.order',b'middle\\nlast\\nold\\n')]:\n"
                " r=subprocess.run(['git','config','--get-all',k],capture_output=True)\n"
                " assert r.returncode==0 and r.stdout==v, 'Git config transport changed'\n", encoding='utf-8')
            (directory / 'CTestTestfile.cmake').write_text(''.join(
                f'add_test(env.{i} "{Path(sys.executable).as_posix()}" "{child.as_posix()}")\n'
                f'set_tests_properties(env.{i} PROPERTIES ENVIRONMENT "PYTHONIOENCODING=utf-8")\n'
                for i in range(3)), encoding='utf-8')
            command = [ctest, '--test-dir', str(directory), '--no-tests=error', '--output-on-failure']
            good = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/ci/preserve_git_environment.py'),
                '--', *command], env=self.environment(), capture_output=True, text=True)
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
            # Preserve the real wrapper exit contract, too.
            failed = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/ci/preserve_git_environment.py'),
                '--', sys.executable, '-c', 'raise SystemExit(8)'], env=self.environment(), capture_output=True)
            self.assertEqual(failed.returncode, 8)


if __name__ == '__main__':
    unittest.main()
