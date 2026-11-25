"""Synthetic API/archive fixtures exercise the actual release CI consumer.

No record here grants runner authorization or represents a remote execution.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
LAYERS = ('contract-gate', 'root-unit-tests', 'backend', 'windows-product',
          'windows-integration-acceptance', 'full-quality-stress-acceptance')
SHA = 'a' * 40
STEPS = {
    'backend': ('verify-immutable-cache', 'verify-workspace-inputs', 'configure', 'build',
                'registered', 'ctest', 'verify-junit', 'verify-workspace-inputs-after'),
    'windows-product': ('build-product', 'stage-product', 'stage-source', 'build-helper',
                        'build-msi', 'build-bootstrapper', 'record-installer', 'installer-contract', 'diagnose-package',
                        'release-check', 'verify-junit', 'verify-workspace-inputs-after'),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class CiProvenanceTests(unittest.TestCase):
    def setUp(self):
        work = ROOT / 'build/ci-provenance-tests'
        work.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=work)
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)

    def ref(self, path):
        return dict(path=path.relative_to(ROOT).as_posix(), sha256=digest(path), bytes=path.stat().st_size)

    def write(self, name, value):
        path = self.work / name
        path.write_text(json.dumps(value), encoding='utf-8')
        return self.ref(path)

    def fixture(self, mutate=lambda d: None):
        common = dict(repository='shijianus/EpoCanva', run_id=123, run_attempt=2, source_commit=SHA)
        run = dict(id=123, run_attempt=2, head_sha=SHA, repository={'full_name': common['repository']},
                   path='.github/workflows/windows-ci.yml', event='workflow_dispatch',
                   status='completed', conclusion='success')
        jobs = []
        summaries = []
        junit = self.work / 'ctest.xml'
        junit.write_text('<testsuite tests="1" failures="0" skipped="0"><testcase name="unit.a" status="run"/></testsuite>', encoding='utf-8')
        inventory = self.write('registered.json', dict(kind='ctestInfo', version={'major': 1, 'minor': 0},
                                                      tests=[{'name': 'unit.a', 'command': ['fixture.exe']}]))
        product = {}
        for key in ('package', 'receipt', 'source_manifest', 'source_zip', 'installer_manifest', 'installer_exe',
                    'product_exe', 'tsf_dll', 'rime_dll'):
            file = self.work / (key + '.fixture')
            file.write_text('SYNTHETIC ' + key, encoding='utf-8')
            product[key] = self.ref(file)
        gate_sets = {
            'windows-product': ('source-clean', 'verified-build', 'engine-identity', 'build-tests', 'package-integrity',
                'package-identity', 'package-path-hygiene', 'corresponding-source', 'installer-artifact',
                'installer-toolchain', 'package-diagnose', 'inputs-stable'),
            'windows-integration-acceptance': ('installer-lifecycle', 'real-host', 'english-capabilities',
                'offline-privacy', 'language-acceptance', 'host-soak', 'inputs-stable'),
            'full-quality-stress-acceptance': ('candidate-quality', 'stress-reference-model', 'inputs-stable'),
        }
        for index, layer in enumerate(LAYERS):
            hosted = index < 2
            jobs.append(dict(id=1000 + index, run_id=123, run_attempt=2, name=layer, head_sha=SHA,
                status='completed', conclusion='success', runner_id=(10 + index if hosted else 99),
                runner_name=(f'hosted-{index}' if hosted else 'approved-fixture'), runner_group_id=(0 if hosted else 5),
                labels=(['windows-2022'] if hosted else ['self-hosted', 'Windows', 'X64', 'epocanva-acceptance-x64', 'unique-fixture']),
                steps=[dict(name='Execute', status='completed', conclusion='success')]))
            required = ('registered', 'ctest', 'verify-junit') if hosted else STEPS.get(layer,
                ('authorized-plan', 'verify-junit', 'release-check', 'verify-workspace-inputs-after', 'reference-stress'))
            summaries.append(dict(format='epocanva-windows-ci-layer-v2', layer=layer, source_commit=SHA,
                run_id='123', run_attempt='2', runner_name=jobs[-1]['runner_name'],
                status='passed', exit_code=0, started_utc='2026-11-25T00:00:00Z', completed_utc='2026-11-25T00:01:00Z',
                workflow_sha256=digest(ROOT / '.github/workflows/windows-ci.yml'),
                input_lock_sha256=digest(ROOT / 'tools/ci/inputs.lock.json'),
                exit_codes={name: 0 for name in required}, outputs=dict(registered=inventory, junit=self.ref(junit))))
            if layer in gate_sets:
                release = dict(format=2, status='completed', source={'commit': SHA}, acceptance_profile='internal-unsigned',
                    exit_code=1, checks=[dict(id=gate, status='passed') for gate in gate_sets[layer]])
                summaries[-1]['exit_codes']['release-check'] = 1
                summaries[-1]['outputs']['release_check'] = self.write(layer + '-release.json', release)
                summaries[-1]['product'] = {key: ref['sha256'] for key, ref in product.items()}
                summaries[-1]['outputs'].update({'product-' + key: ref for key, ref in product.items()})
                if layer == 'windows-product':
                    summaries[-1]['outputs']['installer_junit'] = self.ref(junit)
        approval_record = self.work / 'SYNTHETIC-NOT-APPROVAL.txt'
        approval_record.write_text('Parser fixture only. No real authorization.', encoding='utf-8')
        authorization = dict(**common, format='epocanva-ci-runner-authorization-v1',
            decision='approved-isolated-acceptance', owner='fixture', record_id='fixture-not-approval',
            runner_id=99, runner_name='approved-fixture', runner_group_id=5,
            runner_label='unique-fixture', environment_id='synthetic', snapshot_restore_policy='synthetic',
            record=self.ref(approval_record))
        d = dict(run=run, jobs=dict(total_count=6, jobs=jobs), summaries=summaries, authorization=authorization,
                 package_manifest_sha256=product['package']['sha256'], installer_sha256=product['installer_exe']['sha256'])
        mutate(d)
        artifacts, entries = [], []
        for index, (layer, summary) in enumerate(zip(LAYERS, d['summaries'])):
            reference = self.write(layer + '.json', summary)
            archive = self.work / (layer + '.zip')
            with zipfile.ZipFile(archive, 'w') as z:
                z.write(ROOT / reference['path'], 'summary.json')
            artifacts.append(dict(id=2000 + index, name=f'{layer}-{SHA}-123-2', expired=False,
                size_in_bytes=archive.stat().st_size, workflow_run=dict(id=123, head_sha=SHA),
                digest='sha256:' + digest(archive)))
            entries.append(dict(job=layer, job_id=1000 + index, artifact_id=2000 + index,
                archive=self.ref(archive), summary=reference, summary_archive_path='summary.json'))
        d['artifacts'] = dict(total_count=6, artifacts=artifacts)
        d['manifest'] = dict(**common, format='epocanva-ci-artifacts-v1', entries=entries)
        return d

    def invoke(self, d):
        d['manifest']['github_artifacts'] = self.write('artifacts-api.json', d['artifacts'])
        report = dict(format='epocanva-acceptance-ci-provenance-v1', commit=SHA, result='passed',
            package_manifest_sha256=d['package_manifest_sha256'], installer_sha256=d['installer_sha256'],
            run_id=d.get('report_run_id', 123), started_utc='2026-11-25T00:00:00Z', completed_utc='2026-11-25T00:01:00Z',
            github_run=self.write('run.json', d['run']), github_jobs=self.write('jobs.json', d['jobs']),
            artifact_manifest=self.write('manifest.json', d['manifest']),
            runner_authorization=self.write('authorization.json', d['authorization']))
        fixture = self.write('report.json', report)
        return subprocess.run([os.environ.get('EPOCANVA_TEST_POWERSHELL', 'powershell'), '-NoProfile', '-File',
            str(ROOT / 'tests/ci_gate/ci_provenance_driver.ps1'), '-Fixture', str(ROOT / fixture['path'])],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=45)

    def test_synthetic_complete_chain_passes(self):
        result = self.invoke(self.fixture())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_api_identity_and_authorization_negatives(self):
        cases = [
            ('run-id-string', lambda d: d['run'].update(id='123')),
            ('run-id-float', lambda d: d['run'].update(id=123.0)),
            ('run-id-boolean', lambda d: d['run'].update(id=True)),
            ('boolean-success', lambda d: d['run'].update(conclusion=True)),
            ('report-id-string', lambda d: d.update(report_run_id='123')),
            ('run-attempt-string', lambda d: d['run'].update(run_attempt='2')),
            ('old-sha', lambda d: d['run'].update(head_sha='b' * 40)),
            ('wrong-workflow', lambda d: d['run'].update(path='.github/workflows/other.yml')),
            ('untrusted-pr', lambda d: d['run'].update(event='pull_request')),
            ('incomplete-pagination', lambda d: d['jobs'].update(total_count=7)),
            ('count-float', lambda d: d['jobs'].update(total_count=6.0)),
            ('old-job-run', lambda d: d['jobs']['jobs'][2].update(run_id=122)),
            ('old-job-attempt', lambda d: d['jobs']['jobs'][2].update(run_attempt=1)),
            ('job-id-boolean', lambda d: d['jobs']['jobs'][2].update(id=True)),
            ('duplicate-job-id', lambda d: d['jobs']['jobs'][2].update(id=1000)),
            ('skipped-job', lambda d: d['jobs']['jobs'][2].update(conclusion='skipped')),
            ('cancelled-job', lambda d: d['jobs']['jobs'][2].update(conclusion='cancelled')),
            ('missing-layer', lambda d: d['jobs']['jobs'][2].update(name='canary')),
            ('skipped-step', lambda d: d['jobs']['jobs'][2]['steps'][0].update(conclusion='skipped')),
            ('boolean-step-success', lambda d: d['jobs']['jobs'][2]['steps'][0].update(conclusion=True)),
            ('hosted-is-not-isolated', lambda d: d['jobs']['jobs'][2].update(labels=['windows-2022'])),
            ('wrong-runner', lambda d: d['jobs']['jobs'][2].update(runner_id=98)),
            ('wrong-runner-group', lambda d: d['jobs']['jobs'][2].update(runner_group_id=6)),
            ('pending-authorization', lambda d: d['authorization'].update(decision='pending')),
            ('old-authorization', lambda d: d['authorization'].update(source_commit='b' * 40)),
            ('runner-id-string', lambda d: d['authorization'].update(runner_id='99')),
            ('expired-artifact', lambda d: d['artifacts']['artifacts'][0].update(expired=True)),
            ('old-artifact-run', lambda d: d['artifacts']['artifacts'][0]['workflow_run'].update(id=122)),
            ('old-artifact-attempt', lambda d: d['artifacts']['artifacts'][0].update(name=f'contract-gate-{SHA}-123-1')),
            ('artifact-digest', lambda d: d['artifacts']['artifacts'][0].update(digest='sha256:' + 'f' * 64)),
            ('artifact-id-string', lambda d: d['manifest']['entries'][0].update(artifact_id='2000')),
            ('wrong-artifact-job', lambda d: d['manifest']['entries'][0].update(job_id=1001)),
            ('wrong-product', lambda d: d.update(package_manifest_sha256='b' * 64)),
            ('wrong-installer', lambda d: d.update(installer_sha256='b' * 64)),
            ('archive-traversal', lambda d: d['manifest']['entries'][0].update(summary_archive_path='../summary.json')),
        ]
        for name, change in cases:
            with self.subTest(case=name):
                d = self.fixture()
                change(d)
                result = self.invoke(d)
                self.assertEqual(result.returncode, 1, name + ': ' + result.stdout + result.stderr)
                self.assertIn('SYNTHETIC CONTRACT REJECTED', result.stdout)

    def test_rehashed_outer_reports_cannot_bless_missing_execution_or_altered_junit(self):
        changes = [
            ('in-progress', lambda s: s.update(status='in_progress')),
            ('missing-exit', lambda s: s.pop('exit_code')),
            ('exit-bool', lambda s: s.update(exit_code=False)),
            ('exit-float', lambda s: s.update(exit_code=0.0)),
            ('ctest-failed', lambda s: s['exit_codes'].update(ctest=8)),
            ('missing-command', lambda s: s['exit_codes'].pop('ctest')),
            ('old-source', lambda s: s.update(source_commit='b' * 40)),
            ('old-workflow', lambda s: s.update(workflow_sha256='b' * 64)),
            ('old-lock', lambda s: s.update(input_lock_sha256='b' * 64)),
            ('missing-junit', lambda s: s['outputs'].pop('junit')),
        ]
        for name, change in changes:
            with self.subTest(case=name):
                d = self.fixture(lambda d: change(d['summaries'][0]))
                result = self.invoke(d)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for tag in ('failure', 'skipped', 'error'):
            with self.subTest(tag=tag):
                d = self.fixture()
                (self.work / 'ctest.xml').write_text(f'<testsuite tests="1"><testcase name="unit.a"><{tag}/></testcase></testsuite>', encoding='utf-8')
                result = self.invoke(d)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
