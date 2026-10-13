"""Scalar FP cases capturing result bits, FPCR, FPSR and architectural NZCV."""


def corpus(case_type):
    cases = []
    setup = (0xD51B441B, 0xD51B443C)  # MSR FPCR,X27; MSR FPSR,X28
    capture = (0xD53B443B, 0xD53B441C, 0x3D8003A0)  # FPSR->X27, FPCR->X28, STR Q0

    def add(name, instructions, lhs=0, rhs=0, third=0, fpcr=0, bits=32, gpr=False, nzcv=0xB0000000):
        base = 0x1E270000 if bits == 32 else 0x9E670000
        vector_init = tuple(base | (reg << 5) | reg for reg in (1, 2, 3))
        if gpr:
            vector_init = ()
        cases.append(case_type(name, setup + vector_init + tuple(instructions) + capture,
                               name, {1: lhs, 2: rhs, 3: third, 27: fpcr, 28: 0},
                               nzcv, "fp"))

    binary = {"add": 0x1E222820, "sub": 0x1E223820, "mul": 0x1E220820,
              "div": 0x1E221820, "max": 0x1E224820, "min": 0x1E225820}
    for mode in range(4):
        add(f"fp_add_round_{mode}", (binary["add"],), 0x3F800000, 0x33800000, fpcr=mode << 22)
        add(f"fp_add_cancel_{mode}", (binary["add"],), 0x3F800000, 0xBF800000, fpcr=mode << 22)
        add(f"fp_mul_overflow_{mode}", (binary["mul"],), 0x7F7FFFFF, 0x40000000, fpcr=mode << 22)
    add("fp_div_by_zero", (binary["div"],), 0x3F800000, 0)
    add("fp_sqrt_negative", (0x1E21C020,), 0xBF800000)
    add("fp_add_snan", (binary["add"],), 0x7F800123, 0x7FC00456)
    add("fp_add_snan_dn", (binary["add"],), 0x7F800123, 0x7FC00456, fpcr=1 << 25)
    add("fp_add_input_denormal_fz", (binary["add"],), 1, 0x3F800000, fpcr=1 << 24)
    add("fp_div_output_denormal_fz", (binary["div"],), 0x00800000, 0x40000000, fpcr=1 << 24)
    add("fp_cmp_qnan", (0x1E222020,), 0x7FC00123, 0x3F800000)
    add("fp_cmpe_qnan", (0x1E222030,), 0x7FC00123, 0x3F800000)
    for name, word in (("n", 0x1E244020), ("a", 0x1E264020), ("x", 0x1E274020)):
        add(f"fp_frint{name}_half", (word,), 0x40200000)
    add("fp_cvt_s_to_d", (0x1E22C020,), 0x40200000)
    add("fp_cvtzs_nan", (0x9E380020,), 0x7FC00123)
    add("fp_scvtf_negative", (0x9E620020,), (1 << 64) - 1, bits=64, gpr=True)
    add("fp_fma_fused", (0x1F020C20,), 0x3F800001, 0x3F7FFFFE, 0xBF800000)
    for operation in ("min", "max"):
        add(f"fp_{operation}_zero", (binary[operation],), 0x80000000, 0)
    add("fp_div_denormal_gradual", (binary["div"],), 0x00800000, 0x40000000)
    add("fp_double_div", (binary["div"] | 0x00400000,), 0x3FF0000000000000,
        0x4008000000000000, bits=64)
    add("fp_fnmul_qnan", (0x1E228820,), 0x7F800123, 0x7FC00456)
    add("fp_fnmul_inf_zero", (0x1E228820,), 0x7F800000, 0)
    add("fp_fmadd_inf_zero_qnan", (0x1F020C20,), 0x7F800000, 0, 0x7FC00456)
    add("fp_frinti_negative_zero", (0x1E27C020,), 0x80000000)
    add("fp_fccmp_false", (0x1E220420,), 0x3F800000, 0x3F800000)
    add("fp_fccmp_true", (0x1E220420,), 0x3F800000, 0x3F800000, nzcv=0x40000000)
    add("fp_fccmpe_false_nan", (0x1E220430,), 0x7FC00123, 0x3F800000)
    add("fp_fccmpe_true_nan", (0x1E220430,), 0x7FC00123, 0x3F800000, nzcv=0x40000000)
    add("fp_fz_tiny_inexact", (binary["div"],), 0x00800000, 0x40400000, fpcr=1 << 24)
    return cases
