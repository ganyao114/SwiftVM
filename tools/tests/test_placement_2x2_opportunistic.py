import importlib.util
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "placement_2x2_opportunistic.py"
spec = importlib.util.spec_from_file_location("opportunistic", SCRIPT)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
ORACLE = "print('crcfinal      : 0x4983'); print('Correct operation validated')"


class ProcessMeasurementTests(unittest.TestCase):
    def run_guest(self, code, timeout=3.0):
        with patch.object(runner, "COMMAND", [sys.executable, "-c", code]), \
             patch.object(runner, "foreign_cpu", return_value=0.0), \
             patch.object(runner.os, "getloadavg", return_value=(0.0, 0.0, 0.0)):
            return runner.run_one("cell1", timeout_s=timeout)

    def test_output_larger_than_pipe_is_drained(self):
        record = self.run_guest("print('x' * 4_000_000); " + ORACLE)
        self.assertTrue(record["clean"])
        self.assertEqual(record["rc"], 0)
        self.assertFalse(record["timed_out"])

    def test_short_process_is_not_rounded_to_sampling_period(self):
        record = self.run_guest("import time; time.sleep(0.05); " + ORACLE)
        self.assertTrue(record["clean"])
        self.assertGreaterEqual(record["wall_s"], 0.05)
        self.assertLess(record["wall_s"], 0.75)

    def test_timeout_kills_descendants_holding_stdout(self):
        start = time.monotonic()
        record = self.run_guest(
            "import subprocess,sys,time; "
            "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
            "time.sleep(30)", timeout=0.15)
        self.assertTrue(record["timed_out"])
        self.assertFalse(record["clean"])
        self.assertNotEqual(record["rc"], 0)
        self.assertLess(time.monotonic() - start, 2.0)


if __name__ == "__main__":
    unittest.main()
