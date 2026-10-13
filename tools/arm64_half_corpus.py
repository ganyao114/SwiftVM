"""Binary16 scalar cases with explicit IEEE/ARM result-bit expectations."""

EXPECTATIONS = {}


def expected(case):
    if case.name not in EXPECTATIONS:
        raise ValueError(f"Half case lacks independent expectations: {case.name}")
    return EXPECTATIONS[case.name]


def corpus(case_type):
    cases = []
    setup = (0xD51B441B, 0xD51B443C)
    capture = (0xD53B443B, 0xD53B441C, 0x3D8003A0)

    def add(name, word, lhs=0, rhs=0, third=0, fpcr=0, result=0, flags=0,
            nzcv=0xB0000000, final_nzcv=None, source=16, gpr=False, integer_result=None):
        name = "half_" + name
        move = {16: 0x1EE70000, 32: 0x1E270000, 64: 0x9E670000}[source]
        init = () if gpr else tuple(move | (reg << 5) | reg for reg in (1, 2, 3))
        cases.append(case_type(name, setup + init + (word,) + capture, name,
                               {1: lhs, 2: rhs, 3: third, 27: fpcr, 28: 0}, nzcv, "half"))
        EXPECTATIONS[name] = {"memory": result.to_bytes(16, "little"), "fpsr": flags,
                              "fpcr": fpcr, "nzcv": nzcv if final_nzcv is None else final_nzcv}
        if integer_result is not None:
            EXPECTATIONS[name]["x0"] = integer_result

    binary = {"add": 0x1EE22820, "sub": 0x1EE23820, "mul": 0x1EE20820,
              "div": 0x1EE21820, "max": 0x1EE24820, "min": 0x1EE25820}
    for mode in range(4):
        add(f"add_round_{mode}", binary["add"], 0x3C00, 0x1000, fpcr=mode << 22,
            result=0x3C01 if mode == 1 else 0x3C00, flags=16)
        add(f"cancel_{mode}", binary["add"], 0x3C00, 0xBC00, fpcr=mode << 22,
            result=0x8000 if mode == 2 else 0)
        add(f"overflow_{mode}", binary["mul"], 0x7BFF, 0x4000, fpcr=mode << 22,
            result=0x7C00 if mode in (0, 1) else 0x7BFF, flags=20)
    add("divzero", binary["div"], 0x3C00, 0, result=0x7C00, flags=2)
    add("sqrt_negative", 0x1EE1C020, 0xBC00, result=0x7E00, flags=1)
    add("sqrt_four", 0x1EE1C020, 0x4400, result=0x4000)
    add("add_snan", binary["add"], 0x7C01, 0x7E02, result=0x7E01, flags=1)
    add("add_snan_dn", binary["add"], 0x7C01, 0x7E02, fpcr=1 << 25, result=0x7E00, flags=1)
    add("fnmul_snan", 0x1EE28820, 0x7C01, 0x7E02, result=0xFE01, flags=1)
    add("fnmul_infzero", 0x1EE28820, 0x7C00, 0, result=0xFE00, flags=1)
    add("fma_infzero_qnan", 0x1FC20C20, 0x7C00, 0, 0x7E02, result=0x7E00, flags=1)
    add("fma_fused", 0x1FC20C20, 0x3C01, 0x3BFE, 0xBC00, result=0x8010)
    add("input_subnormal", binary["add"], 1, 0x3C00, result=0x3C00, flags=16)
    add("input_fz16", binary["add"], 1, 0x3C00, fpcr=1 << 19, result=0x3C00)
    add("input_fz32_only", binary["add"], 1, 0x3C00, fpcr=1 << 24, result=0x3C00, flags=16)
    add("output_gradual", binary["div"], 0x0400, 0x4000, result=0x0200)
    add("output_fz16", binary["div"], 0x0400, 0x4000, fpcr=1 << 19, result=0, flags=8)
    add("output_fz16_inexact", binary["div"], 0x0400, 0x4200, fpcr=1 << 19, result=0, flags=8)
    add("cmp_qnan", 0x1EE22020, 0x7E01, 0x3C00, final_nzcv=0x30000000)
    add("cmpe_qnan", 0x1EE22030, 0x7E01, 0x3C00, flags=1, final_nzcv=0x30000000)
    for op, word, result, flags in (("n", 0x1EE44020, 0x4000, 0),
                                    ("a", 0x1EE64020, 0x4200, 0),
                                    ("x", 0x1EE74020, 0x4000, 16)):
        add("frint" + op, word, 0x4100, result=result, flags=flags)
    add("frinti_negativezero", 0x1EE7C020, 0x8000, result=0x8000)
    add("min_zero", binary["min"], 0x8000, 0, result=0x8000)
    add("max_zero", binary["max"], 0x8000, 0, result=0)
    add("convert_h_s", 0x1EE24020, 0x4100, result=0x40200000)
    add("convert_h_d", 0x1EE2C020, 0x4100, result=0x4004000000000000)
    add("convert_s_h", 0x1E23C020, 0x40200000, source=32, result=0x4100)
    add("convert_d_h", 0x1E63C020, 0x4004000000000000, source=64, result=0x4100)
    add("convert_hsub_s_fz16", 0x1EE24020, 1, fpcr=1 << 19, result=0x33800000)
    add("convert_hsub_d_fz16", 0x1EE2C020, 1, fpcr=1 << 19, result=0x3E70000000000000)
    add("convert_s_hsub_fz16", 0x1E23C020, 0x33800000, source=32, fpcr=1 << 19, result=1)
    add("convert_s_htiny_fz16", 0x1E23C020, 0x33000000, source=32, fpcr=1 << 19, result=0, flags=24)
    add("convert_d_hsub_fz16", 0x1E63C020, 0x3E70000000000000, source=64, fpcr=1 << 19, result=1)
    add("convert_d_htiny_fz16", 0x1E63C020, 0x3E60000000000000, source=64, fpcr=1 << 19, result=0, flags=24)
    add("convert_ssub_h_fz32", 0x1E23C020, 1, source=32, fpcr=1 << 24, result=0, flags=128)
    add("convert_dsub_h_fz32", 0x1E63C020, 1, source=64, fpcr=1 << 24, result=0, flags=128)
    add("scvtf_w", 0x1EE20020, 0xFFFFFFFF, gpr=True, result=0xBC00)
    add("ucvtf_w", 0x1EE30020, 1024, gpr=True, result=0x6400)
    for scale in (24, 25):
        word = 0x1EC20020 | ((64 - scale) << 10)
        add(f"fixed_scvtf_sub_s{scale}", word, 1, gpr=True,
            result=1 if scale == 24 else 0, flags=0 if scale == 24 else 24)
        add(f"fixed_scvtf_sub_s{scale}_fz16", word, 1, gpr=True,
            fpcr=1 << 19, result=0, flags=8)
    word = 0x1ED80020 | (40 << 10)
    add("fixed_fcvtzs_sub_s24", word, 1, integer_result=1)
    add("fixed_fcvtzs_sub_s24_fz16", word, 1, fpcr=1 << 19, integer_result=0)
    for integer_width in (32, 64):
        for scale in (1, integer_width // 2, integer_width):
            for unsigned in (False, True):
                word = 0x1EC00020 | ((integer_width == 64) << 31) | (64 - scale) << 10
                op = "ucvtf" if unsigned else "scvtf"
                operand = 1 << max(0, scale - 2)
                add(f"fixed_{op}_i{integer_width}_s{scale}", word | ((3 if unsigned else 2) << 16),
                    operand, gpr=True, result=0x3800 if scale == 1 else 0x3400)
                op = "fcvtzu" if unsigned else "fcvtzs"
                maximum = (1 << (integer_width - int(not unsigned))) - 1
                scaled = 5 * (1 << scale) // 4
                clipped = min(scaled, maximum)
                flags = 1 if scaled > maximum else 16 if scale == 1 else 0
                add(f"fixed_{op}_i{integer_width}_s{scale}", word | ((25 if unsigned else 24) << 16),
                    0x3D00, flags=flags, integer_result=clipped)
    # Bitwise vector FP16 transforms do not process NaNs or flush subnormals.
    # AH=1 is excluded: Unicorn 2.1.4 masks FPCR.AH writes (see standalone probe).
    import struct
    profiles = ((0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x83FF, 0x0400, 0x8400),
                (0x7C01, 0xFC01, 0x7E55, 0xFE55, 0x7FFF, 0xFFFF, 0x7DFF, 0xFDFF),
                (0x3C00, 0xBC00, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x3555, 0xB555),
                (0x0123, 0x8123, 0x7C55, 0xFC55, 0x7E01, 0xFE01, 0x4567, 0xC567))
    for operation, base in (("fabs", 0x0EF8F800), ("fneg", 0x2EF8F800)):
        for q in (0, 1):
            for alias in (False, True):
                for index, lanes in enumerate(profiles):
                    source = struct.pack("<8H", *lanes)
                    old = source if alias else bytes.fromhex("aabbccddeeff00112233445566778899")
                    seed = {1: int.from_bytes(old[:8], "little"), 2: int.from_bytes(old[8:], "little"),
                            3: int.from_bytes(source[:8], "little"), 4: int.from_bytes(source[8:], "little"),
                            27: 0x03080000, 28: 0x0800009F}
                    init = (0x9E670020, 0x9EAF0040, 0x9E670061, 0x9EAF0081)
                    word = base | (q << 30) | (0 if alias else 1 << 5)
                    name = f"half_vector_{operation}_q{q}_{'alias' if alias else 'distinct'}_p{index}"
                    capture_bits = (0xD53B443B, 0xD53B441C, 0x3D8003A0, 0x3D8007A1)
                    cases.append(case_type(name, setup + init + (word,) + capture_bits,
                                           name, seed, 0xB0000000, "half"))
                    transformed = [value & 0x7FFF if operation == "fabs" else value ^ 0x8000
                                   for value in lanes[:8 if q else 4]]
                    result = struct.pack(f"<{len(transformed)}H", *transformed).ljust(16, b"\0")
                    EXPECTATIONS[name] = {"memory": result + source, "fpsr": seed[28],
                                          "fpcr": seed[27], "nzcv": 0xB0000000}
    return cases
