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


class FullSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.case = diff.full_state_cases()[0]
        self.raw = diff.encode_full_snapshot(self.case, pc=0x401234, sp=diff.CAPTURE_SP,
            registers=list(range(31)), vectors=[(i << 120) | i for i in range(32)],
            nzcv=0x60000000, fpcr=0x00400000, fpsr=0x08000091, memory=bytes(range(64)))

    @staticmethod
    def oracle_module():
        location = MODULE_PATH.parents[1] / "source/tests/loongarch64/run_arm64_unicorn.py"
        spec = importlib.util.spec_from_file_location("full_snapshot_unicorn", location)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def oracle(self):
        try:
            return self.oracle_module().Oracle()
        except ImportError as error:
            self.skipTest(f"Unicorn/Capstone unavailable: {error}")

    def test_complete_protocol_is_explicit_and_preserves_legacy(self):
        self.assertEqual(len(self.raw), 904)
        state = diff.decode_snapshot(self.raw, self.case, 2)
        self.assertEqual(state["snapshot_version"], 2)
        self.assertEqual(set(state["registers"]), {f"x{i}" for i in range(31)})
        self.assertEqual(set(state["vectors"]), {f"q{i}" for i in range(32)})
        legacy = (diff.MAGIC + struct.pack("<Q31QQ", diff.case_id(self.case), *range(31), 0x60000000)
                  + bytes(range(64)))
        old_state = diff.decode_snapshot(legacy, self.case, 1)
        self.assertEqual(set(old_state), {"case", "registers", "nzcv", "memory"})
        self.assertEqual(len(legacy), 336)
        self.assertEqual(diff.differences(old_state, state),
                         ["snapshot protocol versions differ; incomplete and full state cannot be compared"])
        with self.assertRaises(ValueError):
            diff.decode_snapshot(legacy, self.case, 2)
        with self.assertRaises(ValueError):
            diff.decode_snapshot(self.raw, self.case, 1)

    def test_complete_protocol_rejects_corruption_and_unknown_descriptors(self):
        malformed = [self.raw[:-1], self.raw + b"noise", self.raw[:8] + bytes(8) + self.raw[16:]]
        for offset, format_code, value in ((16, "I", 336), (20, "I", 0),
                                         (824, "Q", diff.MEMORY_BASE + 8), (832, "Q", 63),
                                         (800, "Q", 1)):
            broken = bytearray(self.raw)
            struct.pack_into("<" + format_code, broken, offset, value)
            malformed.append(bytes(broken))
        for raw in malformed:
            with self.subTest(raw=raw[:24]), self.assertRaises(ValueError):
                diff.decode_snapshot(raw, self.case, 2)

    def test_negative_state_pollution_locates_all_fields_and_vector_halves(self):
        expected = diff.decode_snapshot(self.raw, self.case, 2)
        for name, offset in (("PC", 24), ("SP", 32), ("x30", 40 + 30 * 8),
                             ("q0", 288), ("q31", 288 + 31 * 16 + 15),
                             ("FPCR", 808), ("FPSR", 816), ("NZCV", 803),
                             ("memory[0x3f]", 840 + 63)):
            broken = bytearray(self.raw)
            broken[offset] ^= 0x10 if name == "NZCV" else 1
            messages = diff.differences(expected, diff.decode_snapshot(bytes(broken), self.case, 2))
            with self.subTest(field=name):
                self.assertEqual(len(messages), 1)
                self.assertTrue(messages[0].startswith(name + ":"), messages)
        controls = diff.snapshot_negative_controls(self.raw, self.case)
        self.assertEqual(controls["executed"], 10)
        self.assertEqual(controls["rejected"], 10)

    def test_real_guest_capture_matches_independent_precapture_unicorn_state(self):
        oracle = self.oracle()
        for case in diff.full_state_cases():
            with self.subTest(case=case.name):
                raw, counts = oracle.execute(case, diff.make_elf(case, 2), True, 2)
                state = diff.decode_snapshot(raw, case, 2)
                self.assertEqual(counts["state_captures"], 1)
                self.assertEqual(counts["syscalls"], 2)
                self.assertEqual(int(state["pc"], 16), diff.snapshot_boundary(case, 2))
                self.assertGreater(counts["instructions"], 150)
                self.assertEqual(len(state["memory"]), 128)
                if case == self.case:
                    self.assertEqual(int(state["registers"]["x0"], 16), diff.MASK64)
                    self.assertEqual(int(state["registers"]["x1"], 16), case.seeds[1])
                    self.assertEqual(int(state["registers"]["x30"], 16), case.seeds[30])
                    self.assertEqual(int(state["sp"], 16), case.sp)
                    self.assertEqual(int(state["vectors"]["q31"], 16), case.vectors[31])
                    self.assertEqual(int(state["fpcr"], 16), case.fpcr)
                    self.assertEqual(int(state["fpsr"], 16), case.fpsr)
                if case.name == "state_guest_x30_sp":
                    self.assertEqual(int(state["sp"], 16), case.sp + 32)
                    self.assertEqual(int(state["registers"]["x30"], 16), 0x1234 + 0x5678)
                if case.name == "state_guest_q31":
                    self.assertEqual(state["vectors"]["q31"], state["vectors"]["q0"])
                if case.name == "state_scalar_fp_clears_upper":
                    self.assertEqual(int(state["vectors"]["q31"], 16), diff.vector_seed(case, 0) & diff.MASK64)

    def test_real_harness_vector_and_fp_pollution_is_rejected(self):
        oracle = self.oracle()
        source = diff.make_elf(self.case, 2)
        capture_offset = diff.snapshot_boundary(self.case, 2) - diff.CODE_BASE
        q31_store = 0x3D800000 | (((288 + 31 * 16) // 16) << 10) | 31
        for original, replacement, field in ((q31_store, q31_store - 1, "q31"),
                                              (0xD53B4421, 0xD53B4401, "FPSR")):
            elf = bytearray(source)
            offset = elf.index(struct.pack("<I", original), capture_offset)
            struct.pack_into("<I", elf, offset, replacement)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field):
                oracle.execute(self.case, bytes(elf), True, 2)

    def test_full_snapshot_step_proof_counts_real_instructions_and_rejects_fallback(self):
        module = self.oracle_module()
        _, counts = self.oracle().execute(self.case, diff.make_elf(self.case, 2), True, 2)
        n = counts["instructions"]
        good = f"[arm64-step] calls={n} retired={n - 2} syscalls=2 run_calls=0".encode()
        self.assertEqual(module.step_proof(good, counts)["retired"], n - 2)
        for raw in (good + good, good.replace(b"run_calls=0", b"run_calls=1"),
                    f"[arm64-step] calls={n + 1} retired={n - 1} syscalls=2 run_calls=0".encode()):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                module.step_proof(raw, counts)

    def test_capture_preserves_memory_with_sp_at_observation_boundaries(self):
        from dataclasses import replace
        oracle = self.oracle()
        expected_memory = bytes((0x80 + i * 17) & 0xFF for i in range(64))
        for sp in (diff.MEMORY_BASE - 16, diff.MEMORY_BASE, diff.MEMORY_BASE + 1,
                   diff.MEMORY_BASE + 48, diff.MEMORY_BASE + 63, diff.MEMORY_BASE + 64):
            case = replace(self.case, sp=sp)
            with self.subTest(sp=hex(sp)):
                elf = diff.make_elf(case, 2)
                self.assertEqual(struct.unpack_from("<IIQQQQQQ", elf, 120)[6], diff.FULL_DATA_SIZE)
                raw, counts = oracle.execute(case, elf, True, 2)
                state = diff.decode_snapshot(raw, case, 2)
                self.assertEqual(int(state["sp"], 16), sp)
                self.assertEqual(state["memory"], expected_memory.hex())
                self.assertEqual(int(state["nzcv"], 16), case.nzcv)
                self.assertEqual(counts["state_captures"], 1)

    def test_real_rcpc_sp_below_and_above_memory_uses_disjoint_capture_scratch(self):
        from arm64_rcpc_corpus import corpus, expected
        oracle = self.oracle()
        names = {"rcpc_stlurx_o255_p0_sp", "rcpc_ldapursbx_o255_p0_sp",
                 "rcpc_stlurx_o-256_p0_sp", "rcpc_ldapursbx_o-256_p0_sp"}
        selected = [case for case in corpus(diff.Case) if case.name in names]
        self.assertEqual({case.name for case in selected}, names)
        for case in selected:
            with self.subTest(case=case.name):
                raw, counts = oracle.execute(case, diff.make_elf(case, 2), True, 2)
                state = diff.decode_snapshot(raw, case, 2)
                self.assertEqual(int(state["sp"], 16), case.seeds[1])
                self.assertEqual(state["memory"], expected(case)["memory"].hex())
                self.assertEqual(int(state["nzcv"], 16), case.nzcv)
                self.assertEqual(counts["state_captures"], 1)

    def test_capture_rejects_scratch_aliases_and_unmapped_addresses(self):
        from dataclasses import replace
        oracle = self.oracle()
        for address, reason in ((diff.MEMORY_BASE, "aliases observed memory"),
                                (diff.FULL_SNAPSHOT_BASE, "aliases snapshot"),
                                (diff.VECTOR_SEEDS_BASE, "aliases vector seeds"),
                                (diff.DATA_BASE - 16, "outside the mapped"),
                                (diff.DATA_BASE + diff.FULL_DATA_SIZE - 8, "outside the mapped"),
                                (diff.DATA_BASE + diff.FULL_DATA_SIZE, "outside the mapped")):
            sp = address - diff.CAPTURE_SCRATCH_OFFSET
            with self.subTest(address=hex(address)):
                with self.assertRaisesRegex(ValueError, reason):
                    diff.make_elf(replace(self.case, sp=sp), 2)
                moved = replace(self.case, instructions=(0x9100003F,),
                                seeds={**self.case.seeds, 1: sp})  # mov sp, x1
                with self.assertRaisesRegex(ValueError, reason):
                    oracle.execute(moved, diff.make_elf(moved, 2), True, 2)

    def test_capture_sp_and_nzcv_modifications_are_rejected(self):
        oracle = self.oracle()
        source = diff.make_elf(self.case, 2)
        capture_offset = diff.snapshot_boundary(self.case, 2) - diff.CODE_BASE
        for offset, opcode, field in ((capture_offset, 0x910043FF, "SP"),
                                      (source.index(struct.pack("<I", 0x910003E1), capture_offset),
                                       0xB10003E1, "NZCV")):
            elf = bytearray(source)
            struct.pack_into("<I", elf, offset, opcode)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field):
                oracle.execute(self.case, bytes(elf), True, 2)

    def test_protocol_migration_cannot_overwrite_legacy_elf_evidence(self):
        legacy_case = diff.Case("legacy_nop", (0xD503201F,), "nop")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            diff.build(output, [legacy_case], 1)
            elf = output / "legacy_nop.elf"
            before = elf.read_bytes()
            with self.assertRaisesRegex(ValueError, "different snapshot protocol"):
                diff.build(output, [legacy_case], 2)
            self.assertEqual(elf.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
