import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import uuid
import xml.etree.ElementTree as ET


REPO = Path(__file__).resolve().parents[2]
CONTRACT = REPO / 'tools/installer/installer_contract.ps1'
BUILD_BOOTSTRAPPER = REPO / 'tools/installer/build_bootstrapper.ps1'


class InstallerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output_dir = cls.build_dir / 'installer'
        cls.manifest_path = cls.output_dir / 'installer-manifest.json'
        cls.msi_manifest = json.loads(cls.manifest_path.read_text(encoding='utf-8-sig'))
        cls.exe_manifest = json.loads((cls.output_dir / 'bootstrapper-manifest.json').read_text(encoding='utf-8-sig'))
        cls.msi_path = Path(cls.msi_manifest['installer']['path'])
        cls.exe_path = Path(cls.exe_manifest['bootstrapper']['path'])
        cls.exe_bytes = cls.exe_path.read_bytes()
        cls.overlay = cls.exe_manifest['overlay_byte_offset']

    def setUp(self):
        run = tempfile.TemporaryDirectory(prefix='test-static-', dir=self.output_dir)
        self.addCleanup(run.cleanup)
        self.run_dir = Path(run.name)

    def run_contract(self, code, expected_exit, error='', **inputs):
        command = (
            '$ErrorActionPreference="Stop"; Set-StrictMode -Version Latest; '
            '. $env:EPO_INSTALLER_TEST_CONTRACT; ' + code
        )
        environment = dict(os.environ, EPO_INSTALLER_TEST_CONTRACT=str(CONTRACT),
                           TEMP=str(self.run_dir), TMP=str(self.run_dir))
        environment.update({f'EPO_INSTALLER_TEST_{name}': str(value) for name, value in inputs.items()})
        result = subprocess.run([self.powershell, '-NoProfile', '-NonInteractive', '-Command', command],
                                env=environment, capture_output=True, text=True, errors='replace', timeout=120)
        self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
        if error:
            self.assertIn(error, result.stderr)

    def test_consumer_recomputes_real_package(self):
        self.run_contract(
            'Assert-EpoCanvaInstallerPackage -BuildDir $env:EPO_INSTALLER_TEST_BUILD '
            '-ExePath $env:EPO_INSTALLER_TEST_EXE -ExpectedCommit $env:EPO_INSTALLER_TEST_COMMIT '
            '-ExpectedPackageSha256 $env:EPO_INSTALLER_TEST_DIGEST | Out-Null',
            0, BUILD=self.build_dir, EXE=self.exe_path, COMMIT=self.msi_manifest['destination_commit'],
            DIGEST=self.msi_manifest['package_manifest_sha256'],
        )

    def test_consumer_rejects_duplicate_schema_fields(self):
        manifest = self.run_dir / 'duplicate-bootstrapper.json'
        original = (self.output_dir / 'bootstrapper-manifest.json').read_text(encoding='utf-8-sig')
        manifest.write_text('{"format":3,' + original.lstrip()[1:], encoding='utf-8')
        self.run_contract(
            'Assert-EpoCanvaInstallerPackage -BuildDir $env:EPO_INSTALLER_TEST_BUILD '
            '-ExePath $env:EPO_INSTALLER_TEST_EXE -ExpectedCommit $env:EPO_INSTALLER_TEST_COMMIT '
            '-ExpectedPackageSha256 $env:EPO_INSTALLER_TEST_DIGEST '
            '-BootstrapperManifest $env:EPO_INSTALLER_TEST_MANIFEST | Out-Null',
            1, BUILD=self.build_dir, EXE=self.exe_path, COMMIT=self.msi_manifest['destination_commit'],
            DIGEST=self.msi_manifest['package_manifest_sha256'], MANIFEST=manifest,
        )

    def test_static_overlay_rejects_corruption(self):
        pe = struct.unpack_from('<I', self.exe_bytes, 60)[0]
        digest = self.exe_manifest['embedded_msi_sha256'].encode('ascii')
        image_digest_byte_offset = self.exe_bytes.index(digest)
        alterations = {
            'wrong machine': (pe + 4, b'\x4c\x01', 'not an AMD64 PE'),
            'DLL': (pe + 23, bytes([self.exe_bytes[pe + 23] | 0x20]), 'not an x64 PE32+ executable'),
            'overlay magic': (self.overlay, b'INVALID!', 'Missing EPOINST1'),
            'embedded byte count': (self.overlay + 8, bytes(8), 'Embedded MSI byte count'),
            'compiled MSI digest': (image_digest_byte_offset, b'z' * 64, 'compiled MSI digest'),
            'embedded MSI digest': (self.overlay + 16, bytes(32), 'do not have the same identity'),
        }
        command = 'Get-InstallerExeFacts $env:EPO_INSTALLER_TEST_EXE $env:EPO_INSTALLER_TEST_MSI | Out-Null'
        for name, (byte_offset, replacement, error) in alterations.items():
            with self.subTest(name=name):
                bad_exe = self.run_dir / 'bad.exe'
                bad_exe.write_bytes(self.exe_bytes[:byte_offset] + replacement +
                                    self.exe_bytes[byte_offset + len(replacement):])
                self.run_contract(command, 1, error, EXE=bad_exe, MSI=self.msi_path)
        truncated = self.run_dir / 'truncated.exe'
        truncated.write_bytes(self.exe_bytes[:-1])
        self.run_contract(command, 1, 'Embedded MSI byte count', EXE=truncated, MSI=self.msi_path)
        appended = self.run_dir / 'appended.exe'
        appended.write_bytes(self.exe_bytes + b'!')
        self.run_contract(command, 1, 'Unexpected trailing bytes', EXE=appended, MSI=self.msi_path)
        changed_msi = self.run_dir / 'changed.msi'
        shutil.copyfile(self.msi_path, changed_msi)
        with changed_msi.open('ab') as stream:
            stream.write(b'!')
        self.run_contract(command, 1, 'do not have the same identity', EXE=self.exe_path, MSI=changed_msi)

    def test_native_verification_never_creates_temp_payload(self):
        temporary = self.run_dir / 'empty-temp'
        temporary.mkdir()
        environment = dict(os.environ, TEMP=str(temporary), TMP=str(temporary))

        def probe(path, expected, *arguments):
            result = subprocess.run([str(path), '--verify', *arguments], env=environment, timeout=20)
            self.assertEqual(result.returncode, expected)
            self.assertEqual(list(temporary.iterdir()), [])

        probe(self.exe_path, 0)
        damaged = bytearray(self.exe_bytes)
        damaged[-1] ^= 1
        changed = self.run_dir / 'changed.exe'
        changed.write_bytes(damaged)
        probe(changed, 2)
        damaged[self.overlay + 16:self.overlay + 48] = hashlib.sha256(damaged[self.overlay + 48:]).digest()
        changed.write_bytes(damaged)
        probe(changed, 2)
        probe(changed, 4, '--invalid-static-test-option')

    def test_msi_consumer_rejects_invalid_evidence(self):
        for group, key, value, error in [
            ('extraction', 'verified', 'true', 'lacks verified extraction'),
            ('extraction', 'verified', False, 'lacks verified extraction'),
            ('extraction', 'diagnose_exit', None, 'failed or missing'),
            ('extraction', 'static_exit', '0', 'failed or missing'),
            ('builder', 'build_exit', 1, 'failed or missing'),
            ('builder', 'package_sha256', '0' * 64, 'differs from the pinned WiX'),
        ]:
            with self.subTest(group=group, key=key, value=value):
                manifest = json.loads(self.manifest_path.read_text(encoding='utf-8-sig'))
                manifest[group][key] = value
                bad = self.run_dir / 'bad-manifest.json'
                bad.write_text(json.dumps(manifest), encoding='utf-8')
                self.run_contract('Assert-InstallerMsiManifest $env:EPO_INSTALLER_TEST_MANIFEST '
                                  '$env:EPO_INSTALLER_TEST_MSI | Out-Null',
                                  1, error, MANIFEST=bad, MSI=self.msi_path)

    def test_administrative_sequence_rejects_unexpected_action(self):
        altered = self.run_dir / 'unexpected-admin-action.msi'
        shutil.copyfile(self.msi_path, altered)
        self.run_contract(
            '$installer = New-Object -ComObject WindowsInstaller.Installer; '
            '$database = $installer.GetType().InvokeMember("OpenDatabase", "InvokeMethod", $null, '
            '$installer, @($env:EPO_INSTALLER_TEST_MSI, 1)); '
            "$query = 'DELETE FROM `AdminExecuteSequence` WHERE `Action` = ''InstallValidate'''; "
            '$view = $database.GetType().InvokeMember("OpenView", "InvokeMethod", $null, $database, @($query)); '
            '$null = $view.GetType().InvokeMember("Execute", "InvokeMethod", $null, $view, @()); '
            '$null = $database.GetType().InvokeMember("Commit", "InvokeMethod", $null, $database, @()); '
            '$null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view); '
            '$null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database); '
            '$null = [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer); '
            'Get-InstallerMsiFacts $env:EPO_INSTALLER_TEST_MSI | Out-Null',
            1, 'unexpected or custom administrative action', MSI=altered,
        )

    def test_read_failure_and_missing_file_propagate(self):
        locked = self.run_dir / 'locked.msi'
        locked.write_bytes(b'installer lock fixture')
        self.run_contract(
            '$lease = [IO.File]::Open($env:EPO_INSTALLER_TEST_MSI, "Open", "ReadWrite", "None"); '
            'try { Get-InstallerFileIdentity $env:EPO_INSTALLER_TEST_MSI | Out-Null } finally { $lease.Dispose() }',
            1, MSI=locked,
        )
        self.run_contract('Get-InstallerFileIdentity $env:EPO_INSTALLER_TEST_MSI | Out-Null',
                          1, MSI=self.run_dir / 'missing.msi')

    def test_bootstrapper_rejects_outside_preset_output(self):
        outside = self.build_dir.parent / f'disallowed-setup-{uuid.uuid4().hex}.exe'
        result = subprocess.run([self.powershell, '-NoProfile', '-NonInteractive', '-File', str(BUILD_BOOTSTRAPPER),
                                 '-BuildDir', str(self.build_dir), '-OutputPath', str(outside)],
                                capture_output=True, text=True, errors='replace', timeout=15)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('outside', result.stderr)
        self.assertFalse(outside.exists())

    def test_version_resource_matches_msi(self):
        self.run_contract(
            '$facts=Get-InstallerExeFacts $env:EPO_INSTALLER_TEST_EXE $env:EPO_INSTALLER_TEST_MSI; '
            'if ($facts.product_version -cne $env:EPO_INSTALLER_TEST_VERSION -or '
            '$facts.file_version -cne $env:EPO_INSTALLER_TEST_NUMERIC) { throw "Version resource mismatch" }',
            0, EXE=self.exe_path, MSI=self.msi_path, VERSION=self.exe_manifest['product_version'],
            NUMERIC=self.msi_manifest['msi_product_version'] + '.0')

    def test_path_aliases_and_reparse_ancestors_rejected(self):
        for relative in ['../escape', 'file.txt:stream', 'NUL.txt', 'COM1', 'trailing.', 'trailing ']:
            with self.subTest(relative=relative):
                self.run_contract('Assert-InstallerBuildPath $env:EPO_INSTALLER_TEST_PATH '
                                  '$env:EPO_INSTALLER_TEST_ROOT | Out-Null',
                                  1, PATH=str(self.run_dir) + '/' + relative, ROOT=self.run_dir)
        real = self.run_dir / 'real'
        real.mkdir()
        (real / 'boundary').mkdir()
        self.run_contract(
            '$link=Join-Path $env:EPO_INSTALLER_TEST_ROOT "redirect"; '
            '$null=New-Item -ItemType Junction -Path $link -Target $env:EPO_INSTALLER_TEST_REAL; '
            'try { Assert-InstallerBuildPath (Join-Path $link "boundary/file") '
            '(Join-Path $link "boundary") | Out-Null } finally { [IO.Directory]::Delete($link) }',
            1, 'reparse point', ROOT=self.run_dir, REAL=real)

    def test_helper_source_toolchain_and_exit_tampering_rejected(self):
        original = json.loads(Path(self.msi_manifest['helper']['receipt_path']).read_text(encoding='utf-8-sig'))
        variants = [
            ('source', lambda x: x['source']['files'][0].update(sha256='0' * 64)),
            ('compiler', lambda x: x['toolchain']['compiler_file'].update(sha256='0' * 64)),
            ('linker', lambda x: x['toolchain']['linker_file'].update(bytes=0)),
            ('configure exit', lambda x: x.update(configure_exit=None)),
            ('build exit', lambda x: x.update(build_exit='0')),
            ('boolean format', lambda x: x.update(format=True)),
            ('in progress', lambda x: x.update(status='in_progress')),
            ('configuration', lambda x: x.update(configuration='Debug')),
        ]
        for name, alter in variants:
            with self.subTest(name=name):
                value = json.loads(json.dumps(original))
                alter(value)
                path = self.run_dir / 'helper-build.json'
                path.write_text(json.dumps(value), encoding='utf-8')
                self.run_contract('Assert-InstallerHelperReceipt $env:EPO_INSTALLER_TEST_REPO '
                                  '$env:EPO_INSTALLER_TEST_RECEIPT $env:EPO_INSTALLER_TEST_HELPER | Out-Null',
                                  1, REPO=REPO, RECEIPT=path, HELPER=original['binary']['path'])

    def test_wix_cache_poisoning_rejected(self):
        cache = self.run_dir / 'wix-cache'
        shutil.copytree(REPO / 'build/_local/wix-tool', cache)
        executable = cache / '.store/wix/6.0.2/wix/6.0.2/tools/net6.0/any/wix.exe'
        command = '. $env:EPO_INSTALLER_TEST_TOOL; Get-PinnedInstallerWix $env:EPO_INSTALLER_TEST_WIX | Out-Null'
        inputs = dict(TOOL=REPO / 'tools/installer/wix_tool.ps1', WIX=cache / 'wix.exe')
        self.run_contract(command, 0, **inputs)
        original = executable.read_bytes()
        executable.write_bytes(original + b'poison')
        self.run_contract(command, 1, 'differs from the pinned archive', **inputs)
        executable.write_bytes(original)
        extra = executable.parent / 'unlisted.dll'
        extra.write_bytes(b'poison')
        self.run_contract(command, 1, 'Unpinned file', **inputs)
        extra.unlink()
        executable.unlink()
        self.run_contract(command, 1, **inputs)

    def test_msi_directory_traversal_and_property_override_rejected(self):
        queries = [
            "UPDATE `Directory` SET `DefaultDir` = '..' WHERE `Directory` = 'EpoCanvaFolder'",
            "UPDATE `Directory` SET `Directory_Parent` = 'EpoCanvaFolder' WHERE `Directory` = 'EpoCanvaFolder'",
            "INSERT INTO `Property` (`Property`, `Value`) VALUES ('EpoCanvaFolder', 'C:\\outside')",
        ]
        for index, query in enumerate(queries):
            with self.subTest(query=query):
                path = self.run_dir / f'directory-{index}.msi'
                shutil.copyfile(self.msi_path, path)
                self.run_contract(
                    '$installer=New-Object -ComObject WindowsInstaller.Installer; '
                    '$database=$installer.GetType().InvokeMember("OpenDatabase","InvokeMethod",$null,$installer,'
                    '@($env:EPO_INSTALLER_TEST_MSI,1)); '
                    '$view=$database.GetType().InvokeMember("OpenView","InvokeMethod",$null,$database,'
                    '@($env:EPO_INSTALLER_TEST_QUERY)); '
                    '$null=$view.GetType().InvokeMember("Execute","InvokeMethod",$null,$view,@()); '
                    '$null=$database.GetType().InvokeMember("Commit","InvokeMethod",$null,$database,@()); '
                    'foreach ($object in @($view,$database,$installer)) '
                    '{ $null=[Runtime.InteropServices.Marshal]::FinalReleaseComObject($object) }; '
                    'Get-InstallerMsiFacts $env:EPO_INSTALLER_TEST_MSI | Out-Null',
                    1, MSI=path, QUERY=query)

    def test_rehashed_msi_report_does_not_hide_source_change(self):
        manifest = json.loads(self.manifest_path.read_text(encoding='utf-8-sig'))
        manifest['source']['files'][0]['sha256'] = '0' * 64
        body = ''.join(f"{r['path']}\t{r['sha256']}\t{r['bytes']}\n" for r in manifest['source']['files'])
        manifest['source']['sha256'] = hashlib.sha256(body.encode()).hexdigest()
        path = self.run_dir / 'rehash.json'
        path.write_text(json.dumps(manifest), encoding='utf-8')
        self.run_contract('Assert-InstallerMsiManifest $env:EPO_INSTALLER_TEST_MANIFEST '
                          '$env:EPO_INSTALLER_TEST_MSI | Out-Null',
                          1, 'source provenance differs', MANIFEST=path, MSI=self.msi_path)

    def test_midstream_read_error_propagates(self):
        source = ('public sealed class BrokenRead : System.IO.MemoryStream {'
                  'public BrokenRead() : base(new byte[32]) {} '
                  'public override int Read(byte[] b, int o, int n) {'
                  'throw new System.IO.IOException("injected read failure"); }}')
        self.run_contract('Add-Type -TypeDefinition $env:EPO_INSTALLER_TEST_SOURCE; '
                          '$s=New-Object BrokenRead; try { Get-InstallerSegmentSha256 $s 0 32 | Out-Null } '
                          'finally { $s.Dispose() }', 1, 'injected read failure', SOURCE=source)

    def test_hash_failure_cannot_return_identity_under_continue(self):
        self.run_contract('$ErrorActionPreference="Continue"; '
                          '$identity=Get-InstallerFileIdentity $env:EPO_INSTALLER_TEST_MISSING; '
                          'if ($identity) { exit 91 }', 1, MISSING=self.run_dir / 'absent.dll')

    def test_helper_has_no_external_vc_runtime_imports(self):
        binary = Path(self.msi_manifest['helper']['binary_path']).read_bytes()
        pe = struct.unpack_from('<I', binary, 60)[0]
        self.assertEqual(struct.unpack_from('<H', binary, pe + 4)[0], 0x8664)
        sections = struct.unpack_from('<H', binary, pe + 6)[0]
        optional = pe + 24
        section_table = optional + struct.unpack_from('<H', binary, pe + 20)[0]

        def byte_offset(rva):
            for index in range(sections):
                entry = section_table + index * 40
                size, address, raw_size, raw = struct.unpack_from('<IIII', binary, entry + 8)
                if address <= rva < address + max(size, raw_size):
                    return raw + rva - address
            self.fail('Unmapped PE import RVA')

        import_rva = struct.unpack_from('<I', binary, optional + 120)[0]
        cursor = byte_offset(import_rva)
        imports = []
        while any(binary[cursor:cursor + 20]):
            name = byte_offset(struct.unpack_from('<I', binary, cursor + 12)[0])
            imports.append(binary[name:binary.index(b'\0', name)].decode('ascii').lower())
            cursor += 20
        self.assertTrue(imports)
        self.assertFalse([name for name in imports if name.startswith(('msvcp', 'vcruntime', 'api-ms-win-crt'))], imports)

    def test_upgrade_removal_is_inside_rollback_transaction(self):
        self.run_contract(
            '$installer=New-Object -ComObject WindowsInstaller.Installer; '
            '$database=$installer.GetType().InvokeMember("OpenDatabase","InvokeMethod",$null,$installer,'
            '@($env:EPO_INSTALLER_TEST_MSI,0)); try { '
            "$rows=@(Get-InstallerMsiRows $database 'SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`' 2); "
            '$sequence=@{}; foreach ($row in $rows) { $sequence.Add($row[0],[int]$row[1]) }; '
            'if ($sequence.RemoveExistingProducts -le $sequence.InstallInitialize -or '
            '$sequence.RemoveExistingProducts -ge $sequence.InstallFinalize) { throw "Upgrade outside transaction" }; '
            '} finally { foreach ($object in @($database,$installer)) '
            '{ $null=[Runtime.InteropServices.Marshal]::FinalReleaseComObject($object) } }', 0, MSI=self.msi_path)

    def test_static_extraction_propagates_native_failure(self):
        failing_tool = self.run_dir / 'fail.cmd'
        failing_tool.write_text('@exit /b 23\n', encoding='ascii')
        self.run_contract(
            '. $env:EPO_INSTALLER_TEST_STATIC; '
            'Export-InstallerMsiStatic $env:EPO_INSTALLER_TEST_MSI '
            '$env:EPO_INSTALLER_TEST_TOOL '
            '$env:EPO_INSTALLER_TEST_RUN (Join-Path $env:EPO_INSTALLER_TEST_RUN "extracted") | Out-Null',
            1, 'Static MSI extraction failed with exit 23', STATIC=REPO / 'tools/installer/static_msi.ps1',
            MSI=self.msi_path, RUN=self.run_dir, TOOL=failing_tool)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--powershell', default='powershell.exe')
    parser.add_argument('--junit', type=Path)
    options, unittest_options = parser.parse_known_args()
    InstallerContractTests.build_dir = options.build_dir.resolve()
    if InstallerContractTests.build_dir.parent != REPO / 'build':
        parser.error('--build-dir must be a preset under this repository build directory')
    InstallerContractTests.powershell = options.powershell
    program = unittest.main(argv=[__file__, *unittest_options], exit=False)
    if options.junit:
        result = program.result
        suite = ET.Element('testsuite', name='installer-static-contract', tests=str(result.testsRun),
                           failures=str(len(result.failures)), errors=str(len(result.errors)),
                           skipped=str(len(result.skipped)))
        failed = {test.id(): text for test, text in result.failures}
        errors = {test.id(): text for test, text in result.errors}
        # A failed subtest is a failure of its owning registered test.
        for name in unittest.defaultTestLoader.getTestCaseNames(InstallerContractTests):
            case = ET.SubElement(suite, 'testcase', name=name, classname='InstallerContractTests')
            for tag, issues in [('failure', failed), ('error', errors)]:
                for identity, detail in issues.items():
                    if identity.endswith('.' + name) or ('.' + name + ' ') in identity:
                        ET.SubElement(case, tag).text = detail
        ET.ElementTree(suite).write(options.junit, encoding='utf-8', xml_declaration=True)
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
