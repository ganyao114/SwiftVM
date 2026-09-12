import importlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'svm-linux-cq'))
runner = importlib.import_module('run_limits')


class RunLimitTests(unittest.TestCase):
    def run_child(self, code, limits):
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            result = runner.run_limited([sys.executable, '-c', code], stdout=out, stderr=err,
                                        limits=limits)
            out.seek(0)
            return result, out.read().decode()

    def test_short_child_preserves_output_and_exit_status(self):
        result, output = self.run_child("print('checked'); raise SystemExit(42)",
                                       runner.RunLimits(wall_seconds=3, cpu_fraction=0.2))
        self.assertEqual(result.return_code, 42)
        self.assertEqual(output, 'checked\n')
        self.assertIsNone(result.stop_reason)

    def test_deadline_stops_parent_and_worker(self):
        result, output = self.run_child(
            "import subprocess,sys,time; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            "print(p.pid,flush=True); time.sleep(30)",
            runner.RunLimits(wall_seconds=0.5, cpu_fraction=0.5))
        self.assertEqual(result.stop_reason, 'wall_limit')
        self.assertLess(result.elapsed_s, 3)
        self.assertNotEqual(result.return_code, 0)
        if output.strip():
            state = subprocess.run(['ps', '-o', 'stat=', '-p', output.strip()],
                                   capture_output=True, text=True).stdout.strip()
            self.assertTrue(not state or state.startswith('Z'), state)

    def test_invalid_limits_do_not_launch_a_child(self):
        for limits in (runner.RunLimits(cpu_fraction=0), runner.RunLimits(cpu_fraction=2),
                       runner.RunLimits(wall_seconds=float('nan')),
                       runner.RunLimits(rss_mib=0)):
            with self.subTest(limits=limits), self.assertRaises(ValueError):
                runner.run_limited(['does-not-exist'], stdout=None, stderr=None, limits=limits)

    def test_output_limit_caps_the_saved_stream(self):
        result, output = self.run_child("import os; os.write(1,b'x'*(2*1024*1024))",
                                       runner.RunLimits(wall_seconds=4, cpu_fraction=0.2,
                                                        output_mib=1))
        self.assertEqual(result.stop_reason, 'output_limit')
        self.assertEqual(len(output), 1024 * 1024)

    def test_code_cache_backing_file_is_not_an_output_stream(self):
        result, output = self.run_child(
            "import os,tempfile; f=tempfile.TemporaryFile(); "
            "os.ftruncate(f.fileno(),16*1024*1024); print('cache ready')",
            runner.RunLimits(wall_seconds=3, output_mib=1))
        self.assertEqual(result.return_code, 0)
        self.assertIsNone(result.stop_reason)
        self.assertEqual(output, 'cache ready\n')

    def test_kernel_limits_include_stricter_ancestor_groups(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            controls = root / 'controls'
            child = controls / 'parent' / 'child'
            child.mkdir(parents=True)
            membership = root / 'membership'
            membership.write_text('0::/parent/child\n')
            (child / 'cpu.max').write_text('100000 100000\n')
            (child.parent / 'cpu.max').write_text('10000 100000\n')
            (child / 'memory.max').write_text('1024\n')
            (child.parent / 'memory.max').write_text('2048\n')
            (child / 'memory.swap.max').write_text('max\n')
            (child.parent / 'memory.swap.max').write_text('0\n')
            limits = runner.kernel_limits(membership, controls)
        self.assertEqual(limits, {'source': 'cgroup_v2', 'cpu_fraction': 0.1,
                                  'memory_bytes': 1024, 'swap_bytes': 0})

    def test_missing_kernel_controls_do_not_claim_a_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(runner.kernel_limits(root / 'missing', root), {})


if __name__ == '__main__':
    unittest.main()
