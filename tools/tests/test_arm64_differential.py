"""Validate the differential protocol, its failure gates, and ELF construction."""

import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch


MODULE_PATH = Path(__file__).resolve().parents[1] / "arm64_differential.py"
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("arm64_differential", MODULE_PATH)
diff = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = diff
SPEC.loader.exec_module(diff)


class Arm64DifferentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = diff.corpus()

    def setUp(self):
        self.case = self.cases[0]
        self.snapshot = (diff.MAGIC + struct.pack("<Q", diff.case_id(self.case))
                         + struct.pack("<31Q", *range(31))
                         + struct.pack("<Q", 0x60000000) + bytes(range(64)))

    def test_protocol_rejects_truncation_and_stdout_noise(self):
        for raw in (self.snapshot[:-1], b"log\n" + self.snapshot, b"", bytes(336)):
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                diff.decode_snapshot(raw, self.case)

    def test_protocol_rejects_wrong_case_and_reserved_nzcv_bits(self):
        with self.assertRaises(ValueError):
            diff.decode_snapshot(self.snapshot, self.cases[1])
        bad_flags = self.snapshot[:264] + struct.pack("<Q", 1) + self.snapshot[272:]
        with self.assertRaises(ValueError):
            diff.decode_snapshot(bad_flags, self.case)

    def test_comparison_locates_register_flag_and_memory_errors(self):
        state = diff.decode_snapshot(self.snapshot, self.case)
        self.assertEqual(diff.differences(state, state), [])
        broken = bytearray(self.snapshot)
        struct.pack_into("<Q", broken, 16 + 13 * 8, 0x1234)
        struct.pack_into("<Q", broken, 264, 0x10000000)
        broken[272 + 9] ^= 1
        messages = diff.differences(state, diff.decode_snapshot(bytes(broken), self.case))
        self.assertEqual(len(messages), 3)
        self.assertTrue(messages[0].startswith("x13:"))
        self.assertTrue(messages[1].startswith("NZCV:"))
        self.assertTrue(messages[2].startswith("memory[0x9]:"))

    def test_all_elfs_have_loadable_code_and_fixed_writable_snapshot(self):
        names = set()
        for case in self.cases:
            with self.subTest(case=case.name):
                self.assertNotIn(case.name, names)
                names.add(case.name)
                elf = diff.make_elf(case)
                header = struct.unpack_from("<16sHHIQQQIHHHHHH", elf)
                self.assertEqual(header[2], 183)  # EM_AARCH64
                self.assertEqual(header[4], diff.CODE_BASE + 0x1000)
                self.assertEqual(header[10], 2)
                executable = struct.unpack_from("<IIQQQQQQ", elf, 64)
                data = struct.unpack_from("<IIQQQQQQ", elf, 120)
                self.assertEqual(executable[1], 5)
                self.assertEqual(data[1], 6)
                self.assertEqual(data[3], diff.DATA_BASE)
                self.assertEqual(data[5], diff.SNAPSHOT_SIZE)
                self.assertEqual(data[2] % data[7], data[3] % data[7])
                self.assertEqual(elf[data[2]:data[2] + 8], diff.MAGIC)
                self.assertEqual(len(elf), data[2] + data[5])

    def test_full_corpus_covers_all_initial_flag_states(self):
        flags = {case.nzcv for case in self.cases if case.name.startswith("nzcv_")}
        self.assertEqual(flags, {n << 28 for n in range(16)})

    def test_registered_fp_and_saturating_shift_oracles(self):
        from collections import Counter
        from arm64_fmulx_corpus import expected as expected_mulx, reference as multiply
        from arm64_reciprocal_step_corpus import expected as expected_step, reference as step
        from arm64_vector_sat_shift_corpus import expected as expected_shift
        from arm64_indexed_fp_corpus import expected as expected_indexed
        from arm64_vector_doubling_long_corpus import expected as expected_long, reference as doubling_long
        from arm64_vector_fp_unary_corpus import expected as expected_unary, reference as unary
        from arm64_fp_compare_corpus import expected as expected_compare, reference as compare
        from arm64_fp_integer_corpus import expected as expected_integer
        from arm64_fp_precision_corpus import expected as expected_precision
        from arm64_fp_estimate_corpus import expected as expected_estimate
        from arm64_fp_reduction_corpus import expected as expected_reduction
        from arm64_unsigned_estimate_corpus import expected as expected_unsigned
        from arm64_fp_fixed_simd_corpus import expected as expected_fixed, corpus as fixed_corpus
        from arm64_frint_range_corpus import expected as expected_range, reference as frint_range
        from arm64_polynomial_corpus import expected as expected_polynomial, multiply as polynomial_multiply
        from arm64_scalar_variable_shift_corpus import expected as expected_variable, reference as variable_shift
        cases = self.cases
        counts = Counter(case.family for case in cases)
        self.assertEqual(len(cases), 612039)
        self.assertEqual(len({case.name for case in cases}), len(cases))
        self.assertEqual({family: counts[family] for family in ("fmulx", "recipstep", "vecsatshift", "indexedfp", "veclong", "vecunary", "fpcompare", "fpinteger", "fpprecision", "fpestimate", "fpreduction", "unsignedestimate", "fpfixed", "frintrange", "polynomial", "scalarvarshift")},
                         {"fmulx": 3840, "recipstep": 7680, "vecsatshift": 4800, "indexedfp": 51200,
                          "veclong": 8400, "vecunary": 6912, "fpcompare": 64000, "fpinteger": 78336,
                          "fpprecision": 9600, "fpestimate": 15504, "fpreduction": 47360,
                          "unsignedestimate": 9216, "fpfixed": 37856, "frintrange": 23040,
                          "polynomial": 2160, "scalarvarshift": 30400})
        self.assertEqual(Counter(case.name.split("_")[1] for case in cases if case.family == "polynomial"),
                         {"pmul": 720, "pmull": 1440})
        self.assertEqual(Counter(case.name.split("_")[1] for case in cases if case.family == "scalarvarshift"),
                         {"sshl": 1520, "ushl": 1520, "srshl": 1520, "urshl": 1520,
                          "sqshl": 6080, "uqshl": 6080, "sqrshl": 6080, "uqrshl": 6080})
        self.assertEqual(Counter(case.name.split("_")[1] for case in cases if case.family == "frintrange"),
                         {"frint32z": 5760, "frint32x": 5760, "frint64z": 5760, "frint64x": 5760})
        fixed_safe = [case for case in cases if case.family == "fpfixed"]
        fixed_all = fixed_corpus(diff.Case, include_known_bug=True)
        self.assertEqual(len(fixed_all), 39040)
        excluded = {case.name for case in fixed_all} - {case.name for case in fixed_safe}
        self.assertEqual(len(excluded), 1184)
        self.assertTrue(all(name.startswith("fpfixedsimd_fcvtzs_e16_q1_s1_") for name in excluded))
        self.assertFalse(any(case.name.startswith("fpfixedsimd_fcvtzs_e16_q1_s1_") for case in fixed_safe))
        indices = {}
        for case in cases:
            if case.family == "indexedfp":
                _, op, width, arrangement, scalar, index, *_ = case.name.split("_")
                indices.setdefault((op, width, arrangement, scalar), set()).add(int(index[1:]))
        self.assertEqual(len(indices), 32)
        for (_, width, _, _), selected in indices.items():
            self.assertEqual(selected, set(range(128 // int(width[1:]))))
        # FMULX's zero*infinity result is signed 2.0 without IOC. Reciprocal
        # steps use 2.0/1.5 for that special product rather than a NaN.
        for bits, infinity, two, one_half in ((16, 0x7C00, 0x4000, 0x3E00),
                                             (32, 0x7F800000, 0x40000000, 0x3FC00000),
                                             (64, 0x7FF0000000000000, 0x4000000000000000, 0x3FF8000000000000)):
            with self.subTest(bits=bits):
                self.assertEqual(multiply(0, infinity, bits, 0), (two, 0))
                self.assertEqual(multiply(1 << (bits - 1), infinity, bits, 0), (two | (1 << (bits - 1)), 0))
                self.assertEqual(step(0, infinity, bits, False, 0), (two, 0))
                self.assertEqual(step(0, infinity, bits, True, 0), (one_half, 0))
        for bits in (16, 32):
            minimum, maximum = 1 << (bits - 1), (1 << (2 * bits - 1)) - 1
            # Product saturation remains observable even if the subsequent
            # addition of -1 puts the final result back inside the range.
            self.assertEqual(doubling_long(minimum, minimum, (1 << (2 * bits)) - 1, bits, 1),
                             (maximum - 1, True))
        self.assertEqual(unary(0x40800000, 32, "fsqrt", 0), (0x40000000, 0))
        self.assertEqual(unary(0x3FC00000, 32, "frintn", 0), (0x40000000, 0))
        self.assertEqual(unary(0x3FC00000, 32, "frintx", 0), (0x40000000, 16))
        self.assertEqual(frint_range(0x3FC00000, 32, 0, 0), (0x3F800000, 16))
        self.assertEqual(frint_range(0x3FC00000, 32, 1, 0), (0x40000000, 16))
        self.assertEqual(frint_range(0x4F000000, 32, 0, 0), (0xCF000000, 1))
        self.assertEqual(polynomial_multiply(3, 3), 5)  # (x+1)^2 over GF(2).
        self.assertEqual(variable_shift(0x80, 0xFF, 8, 4), (0xC0, 0))
        self.assertEqual(variable_shift(0x7F, 1, 8, 4), (0x7F, 1 << 27))
        self.assertEqual(variable_shift(0x81, 0xFF, 8, 6), (0xC1, 0))
        self.assertEqual(variable_shift(1, 0x12340001, 64, 0), (2, 0))
        for bits, qnan, snan in ((16, 0x7E00, 0x7C01), (32, 0x7FC00000, 0x7F800001),
                                 (64, 0x7FF8000000000000, 0x7FF0000000000001)):
            self.assertEqual(compare(0, 1 << (bits - 1), bits, "fcmeq", 0), ((1 << bits) - 1, 0))
            self.assertEqual(compare(qnan, 0, bits, "fcmeq", 0), (0, 0))
            self.assertEqual(compare(snan, 0, bits, "fcmeq", 0), (0, 1))
            self.assertEqual(compare(qnan, 0, bits, "fcmge", 0), (0, 1))
        references = {"fmulx": expected_mulx, "recipstep": expected_step, "vecsatshift": expected_shift,
                      "indexedfp": expected_indexed, "veclong": expected_long, "vecunary": expected_unary,
                      "fpcompare": expected_compare, "fpinteger": expected_integer,
                      "fpprecision": expected_precision, "fpestimate": expected_estimate,
                      "fpreduction": expected_reduction, "unsignedestimate": expected_unsigned,
                      "fpfixed": expected_fixed, "frintrange": expected_range,
                      "polynomial": expected_polynomial, "scalarvarshift": expected_variable}
        for case in cases:
            if case.family not in references:
                continue
            with self.subTest(case=case.name):
                state = references[case.family](case)
                self.assertEqual(len(state["memory"]), 64)
                self.assertEqual(state["fpsr"] & case.seeds[27], case.seeds[27])
                self.assertEqual(state["fpcr"], case.seeds[28])
                if "registers" in state:
                    self.assertEqual(len(state["registers"]), 31)
                    self.assertEqual(state["registers"]["x27"], state["fpsr"])
                    self.assertEqual(state["registers"]["x28"], state["fpcr"])

    def test_integer174_registered_counts_shapes_and_full_state(self):
        from collections import Counter
        import importlib
        import re
        modules = {
            "mixedsat": ("arm64_mixed_sat_corpus", 19980, 36),
            "pairwiden": ("arm64_pairwise_widen_corpus", 16650, 30),
            "highnarrow": ("arm64_high_narrow_corpus", 24960, 24),
            "abslong": ("arm64_absolute_long_corpus", 22200, 24),
            "absnarrow": ("arm64_absolute_narrow_corpus", 22200, 24),
            "halving": ("arm64_halving_corpus", 29520, 36),
        }
        references = {family: importlib.import_module(module).expected
                      for family, (module, _, _) in modules.items()}
        selected = [case for case in self.cases if case.family in modules]
        self.assertEqual(len(selected), 135510)
        self.assertEqual(Counter(case.family for case in selected),
                         {family: count for family, (_, count, _) in modules.items()})
        for family, (_, _, shapes) in modules.items():
            self.assertEqual(len({re.sub(r"_a\d+_p\d+$", "", case.name)
                                  for case in selected if case.family == family}), shapes)
        for case in selected:
            state = references[case.family](case)
            self.assertEqual(set(state["registers"]), {f"x{index}" for index in range(31)})
            self.assertEqual(state["registers"]["x27"], state["fpsr"])
            self.assertEqual(state["registers"]["x28"], state["fpcr"])
            self.assertEqual(state["registers"]["x29"], diff.MEMORY_BASE)
            self.assertEqual(state["registers"]["x30"], diff.DATA_BASE)
            self.assertEqual(state["nzcv"], case.nzcv)
            self.assertEqual(len(state["memory"]), 64)
            self.assertEqual(state["fpsr"] & case.seeds[27], case.seeds[27])
            self.assertEqual(state["fpcr"], case.seeds[28])

    def test_build_reuses_only_byte_identical_elf_and_repairs_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            diff.build(output, [self.case])
            elf = output / f"{self.case.name}.elf"
            generated = diff.make_elf(self.case)
            with patch.object(Path, "write_bytes", side_effect=AssertionError("identical ELF rewritten")):
                diff.build(output, [self.case])
            corrupt = bytearray(generated)
            corrupt[-1] ^= 1
            elf.write_bytes(corrupt)
            diff.build(output, [self.case])
            self.assertEqual(elf.read_bytes(), generated)
            import json
            manifest = json.loads((output / "corpus.json").read_text())
            self.assertEqual(manifest["cases"][0]["sha256"], diff.sha(elf))


if __name__ == "__main__":
    unittest.main()
