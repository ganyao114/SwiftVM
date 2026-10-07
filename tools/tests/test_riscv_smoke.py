import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    'riscv_smoke', Path(__file__).resolve().parents[1] / 'run_riscv_smoke.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RiscvSmokeTests(unittest.TestCase):
    def test_zero_and_partial_execution_cannot_pass(self):
        cases = [{'name': 'first'}, {'name': 'second'}]
        for output in ('', 'OK 2\n', 'PASS first\nOK 2\n',
                       'PASS first\nPASS first\nOK 2\n'):
            with self.subTest(output=output), self.assertRaises(RuntimeError):
                runner.validate_execution(0, output, cases)
        with self.assertRaises(RuntimeError):
            runner.validate_execution(0, 'OK 0\n', [])
        self.assertEqual(runner.validate_execution(
            0, 'PASS first\nPASS second\nOK 2\n', cases), 2)

    def test_negative_control_requires_the_expected_failure(self):
        cases = [{'name': 'first'}]
        for code, output in ((0, 'FAIL first\n'), (1, ''),
                             (132, 'FAIL first\n'), (1, 'FAIL other\n')):
            with self.subTest(code=code, output=output), self.assertRaises(RuntimeError):
                runner.validate_execution(code, output, cases, negative=True)
        self.assertEqual(runner.validate_execution(1, 'FAIL first\n', cases, negative=True), 0)

    def test_bundle_requires_execution_cases_for_every_function(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'identity.bin').write_bytes(bytes.fromhex('67800000'))
            manifest = {
                'format_version': 1, 'isa': 'rv64g', 'abi': 'lp64d',
                'functions': [{'name': 'identity', 'file': 'identity.bin', 'size': 4, 'cookie': 1}],
                'cases': [],
            }
            (path / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'execution cases'):
                runner.load_bundle(path)
            value = '0x0000000000000000'
            manifest['cases'] = [{'name': 'identity.0', 'function': 'identity',
                                  'args': [value, value], 'memory': [value] * 4,
                                  'expected_result': value, 'expected_memory': [value] * 4}]
            (path / 'manifest.json').write_text(json.dumps(manifest))
            functions, cases = runner.load_bundle(path)
            self.assertEqual(len(functions), 1)
            self.assertEqual(len(cases), 1)
            (path / 'unused.bin').write_bytes(bytes.fromhex('67800000'))
            manifest['functions'].append({'name': 'unused', 'file': 'unused.bin', 'size': 4, 'cookie': 2})
            (path / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'Every generated function'):
                runner.load_bundle(path)


if __name__ == '__main__':
    unittest.main()
