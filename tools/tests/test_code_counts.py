import contextlib
import importlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPTS = Path(__file__).resolve().parents[1] / 'svm-linux-cq'
sys.path.insert(0, str(SCRIPTS))
hot_records = importlib.import_module('hot_records')
fex_join = importlib.import_module('fex_join')
weighted_diff = importlib.import_module('weighted_diff')


def hot(pc=0x1000, entries=100, host=10, versions=1):
    return (f'[svm-hot-all] pc={pc:#x} versions={versions} entries={entries} '
            f'host_bytes={host * 4} host_static={host}\n')


def code(pc, number, entries, host):
    return (f'[svm-hot-code] pc={pc:#x} code={number} entries={entries} '
            f'host_bytes={host * 4} host_static={host}\n')


def fex(pc=0x1000, size=16, guest=8, host=20):
    return (f'[fex-blockstat] rip={pc:#x} guest_bytes={size} '
            f'guest_inst={guest} host_inst={host}\n')


class CodeCountTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def write(self, name, text):
        path = self.root / name
        path.write_text(text)
        return path

    def run_main(self, module, args):
        output = io.StringIO()
        with patch.object(sys, 'argv', ['test', *map(str, args)]), \
                contextlib.redirect_stdout(output):
            rc = module.main()
        return rc, output.getvalue()

    def test_range_weighting_does_not_create_cross_terms(self):
        svm = self.write('svm', hot() + hot(0x1004, entries=1, host=30))
        reference = self.write('fex', fex())
        rc, output = self.run_main(fex_join, ['--svm', svm, '--fex', reference])
        self.assertEqual(rc, 0)
        self.assertIn('entry_weighted_static: svm=1030 total=1030', output)
        self.assertNotIn('4040', output)
        self.assertIn('static: svm_host=40 fex_host=20', output)
        self.assertNotIn('per-pc join', output)

    def multi_version(self):
        return (code(0x1000, 0, 100, 10) + code(0x1000, 1, 1, 30)
                + hot(entries=101, host=30, versions=2)
                + '[svm-hot-end] codes=2 pcs=1 overflow=0\n')

    def test_each_version_keeps_its_own_entry_weight(self):
        path = self.write('svm', self.multi_version())
        unit = hot_records.load_hot(path)[0x1000]
        self.assertEqual(unit.weighted_host(), 1030)
        self.assertEqual(unit.versions, 2)
        self.assertEqual(unit.entries, 101)
        rc, output = self.run_main(fex_join, ['--svm', path, '--fex', self.write('fex', fex())])
        self.assertEqual(rc, 0)
        self.assertIn('svm_entry_weighted=1030', output)

    def test_bad_or_incomplete_hot_captures_fail(self):
        valid = self.multi_version()
        values = [
            '', hot() + hot(), hot(versions=2), '[svm-hot-all] pc=0x1000\n',
            hot().replace('host_bytes=40', 'host_bytes=4'),
            hot().replace('entries=100', 'entries=-1'),
            hot().replace('entries=100', 'entries=100 entries=1'),
            hot(entries=1, host=0),
            valid.replace('[svm-hot-end] codes=2 pcs=1 overflow=0\n', ''),
            valid.replace('overflow=0', 'overflow=1'),
            valid.replace('codes=2', 'codes=3'),
            valid.replace('codes=2', 'codes=18446744073709551615'),
            valid.replace('entries=101', 'entries=102'),
            valid.replace('code=1', 'code=0'),
            valid.replace('code=1', 'code=2'),
            valid + hot(),
        ]
        for text in values:
            with self.subTest(text=text), self.assertRaises(ValueError):
                hot_records.load_hot(self.write('invalid', text))

    def test_conflicting_or_invalid_fex_captures_fail(self):
        for text in ['', '[fex-blockstat] rip=0x1000\n', fex(size=0),
                     fex(guest=0), fex(host=0), fex() + fex(host=21),
                     fex(pc=(1 << 64) - 1)]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                fex_join.parse_fex(self.write('invalid', text))
        self.assertEqual(len(fex_join.parse_fex(self.write('repeat', fex() * 2))), 1)

    def test_missing_ranges_and_zero_entries_do_not_pass(self):
        svm = self.write('svm', hot() + hot(0x2000, entries=1))
        rc, output = self.run_main(fex_join, ['--svm', svm, '--fex', self.write('fex', fex())])
        self.assertEqual(rc, 1)
        self.assertIn('uncovered=10', output)
        self.assertIn('coverage_status=FAIL', output)
        with self.assertRaises(ValueError):
            self.run_main(fex_join, ['--svm', self.write('zero', hot(entries=0)),
                                    '--fex', self.root / 'fex'])

    def test_tightest_range_and_guest_membership(self):
        svm = self.write('svm', hot(0x1008, entries=1))
        reference = self.write('fex', fex(size=32, host=30) + fex(0x1008, size=4, host=2))
        rc, output = self.run_main(fex_join, ['--svm', svm, '--fex', reference])
        self.assertEqual(rc, 0)
        self.assertIn('static: svm_host=10 fex_host=2', output)
        gap = self.write('gap', '[svm-gap-op] unit=0x1008 block=0x1008 guest_pc=0x1010\n')
        rc, output = self.run_main(fex_join, ['--svm', svm, '--fex', reference, '--gap', gap])
        self.assertEqual(rc, 0)
        self.assertIn('static: svm_host=10 fex_host=30', output)

    def test_retained_weights_reject_ambiguous_versions(self):
        path = self.write('multiple', self.multi_version())
        with self.assertRaisesRegex(ValueError, 'multiple code versions'):
            weighted_diff.load_hot(path)

    def test_retained_weights_check_entry_coverage(self):
        baseline = self.write('before', hot(entries=1, host=100000) + hot(0x2000, entries=99, host=1))
        candidate = self.write('after', hot(entries=1, host=100000))
        rc, output = self.run_main(weighted_diff, [baseline, candidate, '--top', '0', '--min-coverage', '99'])
        self.assertEqual(rc, 1)
        self.assertIn('entry_coverage=1.000000%', output)

    def test_single_version_retained_comparison(self):
        baseline = self.write('before', hot())
        candidate = self.write('after', hot(host=9))
        rc, output = self.run_main(weighted_diff, [baseline, candidate, '--fail-on-growth'])
        self.assertEqual(rc, 0)
        self.assertIn('common_baseline=1000 common_candidate=900', output)
        self.assertIn('not dynamic-work or speed ratios', output)


if __name__ == '__main__':
    unittest.main()
