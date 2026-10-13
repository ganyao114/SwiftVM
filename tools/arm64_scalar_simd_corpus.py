"""Scalar integer SIMD arithmetic, count boundaries, and saturating QC math."""

import re


def corpus(case_type):
    cases = []
    bases = {"add": 0x5EE08400, "sub": 0x7EE08400,
             "cmeq": 0x7EE08C00, "cmgt": 0x5EE03400, "cmge": 0x5EE03C00,
             "cmhi": 0x7EE03400, "cmhs": 0x7EE03C00, "cmtst": 0x5EE08C00,
             "sshl": 0x5EE04400, "ushl": 0x7EE04400,
             "sqadd": 0x5E200C00, "uqadd": 0x7E200C00,
             "sqsub": 0x5E202C00, "uqsub": 0x7E202C00}
    for op, base in bases.items():
        for bits in (8, 16, 32, 64) if op in ("sqadd", "uqadd", "sqsub", "uqsub") else (64,):
            mask, sign = (1 << bits) - 1, 1 << (bits - 1)
            if op in ("sshl", "ushl"):
                operands = [("s" + str(count), sign + 3, (0xA55AA55AA55AA500 | (count & 255)))
                            for count in (0, 1, -1, 63, 64, 65, -63, -64, -65, 127, -128)]
            else:
                operands = [("max", sign - 1, 1), ("min", sign, mask),
                            ("unsigned", mask, 1), ("exact", 12, 3), ("equal", mask, mask)]
            for label, lhs, rhs in operands:
                for alias in ("none", "src", "rhs", "all"):
                    for sticky in (0, 1):
                        rn, rm = (1, 1 if alias == "all" else 2)
                        rd = 1 if alias in ("src", "all") else 2 if alias == "rhs" else 0
                        seeds = {1: 0x1122334455667788, 2: 0x8877665544332211,
                                 3: lhs, 4: 0xFFEEDDCCBBAA9988,
                                 5: rhs, 6: 0xAABBCCDDEEFF0011,
                                 27: 0x9F | (sticky << 27)}
                        init = tuple(word for reg in range(3) for word in
                                     (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                                      0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
                        word = base | (rm << 16) | (rn << 5) | rd
                        if op in ("sqadd", "uqadd", "sqsub", "uqsub"):
                            word |= (bits.bit_length() - 4) << 22
                        name = f"scalarneon_{op}_e{bits}_{label}_{alias}_qc{sticky}"
                        cases.append(case_type(name, (0xD51B443B,) + init + (word, 0xD53B443B, 0x3D8003A0 | rd),
                                               name, seeds, 0xB0000000, "scalarneon"))
    return cases


def expected(case):
    match = re.fullmatch(r"scalarneon_([a-z]+)_e(8|16|32|64)_[a-z0-9-]+_(none|src|rhs|all)_qc[01]", case.name)
    if not match:
        raise ValueError(f"Unrecognized scalar SIMD case: {case.name}")
    op, bits, alias = match.groups()
    bits = int(bits)
    mask, sign = (1 << bits) - 1, 1 << (bits - 1)
    lhs = case.seeds[3] & mask
    rhs = (case.seeds[3] if alias == "all" else case.seeds[5]) & mask
    signed_lhs, signed_rhs = lhs - (1 << bits) if lhs & sign else lhs, rhs - (1 << bits) if rhs & sign else rhs
    fpsr = case.seeds[27]
    if op in ("add", "sub"):
        value = lhs + rhs if op == "add" else lhs - rhs
    elif op in ("cmeq", "cmgt", "cmge", "cmhi", "cmhs", "cmtst"):
        predicate = {"cmeq": lhs == rhs, "cmgt": signed_lhs > signed_rhs,
                     "cmge": signed_lhs >= signed_rhs, "cmhi": lhs > rhs,
                     "cmhs": lhs >= rhs, "cmtst": bool(lhs & rhs)}[op]
        value = mask if predicate else 0
    elif op in ("sshl", "ushl"):
        count = rhs & 255
        count = count - 256 if count & 128 else count
        operand = signed_lhs if op == "sshl" else lhs
        value = operand << count if count >= 0 else operand >> -count
    else:
        signed = op.startswith("s")
        minimum, maximum = (-sign, sign - 1) if signed else (0, mask)
        a, b = (signed_lhs, signed_rhs) if signed else (lhs, rhs)
        raw = a + b if op.endswith("add") else a - b
        value = max(minimum, min(raw, maximum))
        fpsr |= (raw != value) << 27
    return (value & mask).to_bytes(bits // 8, "little").ljust(16, b"\0"), fpsr
