import contextlib
import hashlib
import importlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'svm-linux-cq'))
capture = importlib.import_module('quick_shape')
runner = importlib.import_module('run_limits')

COMPLETE = (
    '[svm-hot-code] pc=0x1000 code=0 entries=3 host_bytes=8 host_static=2\n'
    '[svm-hot-all] pc=0x1000 versions=1 entries=3 host_bytes=8 host_static=2\n'
    '[svm-hot-end] codes=1 pcs=1 overflow=0\n'
)


class CaptureTests(unittest.TestCase):
    def collect(self, directory, *, lines=COMPLETE, exit_code=42, stop=None,
                extra=(), change_input=False, static=False):
        root = Path(directory)
        guest = root / 'guest'
        guest.write_bytes(b'guest input')
        out = root / 'output'

        def run(command, *, stdout, stderr, env, cwd, limits):
            self.assertEqual(env['SVM_JIT_CACHE'], '')
            self.assertNotIn('SVM_EXEC_TRACE', env)
            self.assertEqual(limits.cpu_fraction, 0.1)
            stdout.write(b'checked\n')
            if static:
                stderr.write(lines.encode())
            else:
                Path(env['SVM_RA_HOT_COALESCE']).write_text(lines)
            if change_input:
                guest.write_bytes(b'changed input')
            return runner.RunResult(exit_code, 0.01, 123, stop)

        argv = ['quick_shape.py', '--svm', sys.executable, '--guest', str(guest),
                '--out', str(out), '--expect-exit', '42', *extra]
        if static:
            argv.append('--static-only')
        with patch.object(sys, 'argv', argv), patch.object(capture, 'run_limited', run), \
                patch.dict(os.environ, {'SVM_EXEC_TRACE': '1'}), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = capture.main()
        return result, json.loads((out / 'run.json').read_text()), out

    def test_capture_records_checked_inputs_results_and_limits(self):
        digest = hashlib.sha256(b'checked\n').hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            result, record, _ = self.collect(directory, extra=(
                '--expect-output', f'stdout.log={digest}'))
        self.assertEqual(result, 0)
        self.assertTrue(record['valid'])
        self.assertEqual(record['outputs']['stdout.log']['sha256'], digest)
        self.assertEqual(record['inputs'][1]['sha256'], hashlib.sha256(b'guest input').hexdigest())
        self.assertFalse(record['elapsed_is_performance_measurement'])
        self.assertEqual(record['result']['return_code'], 42)

    def test_incomplete_or_incorrect_runs_cannot_pass(self):
        cases = (
            ({'lines': COMPLETE.rsplit('[svm-hot-end]', 1)[0]}, 'final record'),
            ({'exit_code': 0}, 'expected exit'),
            ({'stop': 'wall_limit'}, 'wall_limit'),
            ({'change_input': True}, 'input changed'),
            ({'extra': ('--oracle', 'absent.bin')}, 'result missing'),
            ({'extra': ('--expect-output', 'stdout.log=' + '0' * 64)}, 'checksum differs'),
        )
        for options, error in cases:
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                result, record, _ = self.collect(directory, **options)
                self.assertEqual(result, 1)
                self.assertFalse(record['valid'])
                self.assertTrue(any(error in item for item in record['errors']), record)

    def test_static_capture_keeps_each_version_without_inventing_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            result, record, out = self.collect(directory, static=True, lines=(
                '[svm-host] pc=0x1000 size=8 code=0\n'
                '[svm-host] pc=0x1000 size=12 code=1\n'))
            unit = capture.load_hot(out / 'shape.hot')[0x1000]
        self.assertEqual(result, 0)
        self.assertEqual(record['mode'], 'static_bytes')
        self.assertEqual(unit.versions, 2)
        self.assertEqual(unit.host_static, 5)
        self.assertEqual(unit.entries, 0)
        self.assertEqual(unit.weighted_host(), 0)

    def test_result_paths_cannot_reuse_external_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for name in ('../existing.bin', str(root.parent / 'existing.bin')):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    capture.output_file(root, name)


if __name__ == '__main__':
    unittest.main()
