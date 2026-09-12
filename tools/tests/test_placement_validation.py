import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import patch


def load_script(name):
    path = Path(__file__).resolve().parents[1] / (name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load_script("placement_2x2_wall")
audit = load_script("placement_2x2_analyze")


def host_row(pc=0x1000, data=b"\x1f\x20\x03\xd5"):
    digest = 1469598103934665603
    for byte in data:
        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"[svm-host] pc={pc:#x} size={len(data)} hash={digest:016x} bytes={data.hex()}\n"


class LayoutAuditTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.paths = [Path(self.directory.name) / str(i) for i in range(4)]
        for path in self.paths:
            path.write_text(host_row())

    def run_audit(self):
        args = types.SimpleNamespace(**{
            f"cell{i + 1}": path for i, path in enumerate(self.paths)
        })
        with contextlib.redirect_stdout(io.StringIO()):
            audit.command_audit(args)

    def test_valid_records_and_repeated_identical_entries(self):
        self.paths[0].write_text(host_row() * 2)
        self.run_audit()

    def test_empty_truncated_and_wrong_checksum_records_fail(self):
        for text in ("", host_row().split("bytes=")[0],
                     host_row().replace("size=4", "size=8"),
                     host_row().replace("1f2003d5", "00000000")):
            with self.subTest(text=text):
                self.paths[0].write_text(text)
                with self.assertRaises(SystemExit):
                    self.run_audit()

    def test_different_address_sets_fail(self):
        self.paths[1].write_text(host_row() + host_row(0x2000))
        with self.assertRaises(SystemExit):
            self.run_audit()

    def test_conflicting_duplicate_address_fails(self):
        self.paths[0].write_text(host_row() + host_row(data=b"\0" * 4))
        with self.assertRaises(SystemExit):
            self.run_audit()


class BoundedRunnerTests(unittest.TestCase):
    def run_command(self, code, timeout=2):
        with patch.object(runner, "COMMAND", [sys.executable, "-c", code]), \
             patch.object(runner, "foreign_cpu", return_value=0), \
             patch.object(runner.os, "getloadavg", return_value=(0, 0, 0)):
            return runner.run_one("cell1", timeout_s=timeout)

    def test_success_drains_output(self):
        record = self.run_command("print('x' * 100000); "
                                  "print('crcfinal      : 0x4983'); "
                                  "print('Correct operation validated')")
        self.assertEqual(record["rc"], 0)
        self.assertTrue(record["oracle"])
        self.assertFalse(record["timed_out"])

    def test_timeout_reaps_child_and_descendants(self):
        started = time.monotonic()
        record = self.run_command(
            "import subprocess,sys,time; "
            "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
            "time.sleep(30)", timeout=0.15)
        self.assertTrue(record["timed_out"])
        self.assertNotEqual(record["rc"], 0)
        self.assertLess(time.monotonic() - started, 2)


if __name__ == "__main__":
    unittest.main()
