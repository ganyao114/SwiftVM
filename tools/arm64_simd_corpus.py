"""Integer SIMD expansion cases with deterministic bit-pattern operands."""


def corpus(case_type):
    seeds = {1: 0x807F00FF017E02FD, 2: 0x800000007FFFFFFF,
             3: 0xFF8001007F02FE03, 4: 0x7FFFFFFF80000000,
             5: 0x0123456789ABCDEF, 6: 0xFEDCBA9876543210,
             7: 0x1020304000010203, 8: 0xFFFFFEFD807F100F,
             9: 0x201F100F03020100, 10: 0xFF80403F302F2221}
    init = tuple(word for reg in range(5) for word in
                 (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                  0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    store = tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))
    cases = []

    def add(name, word, assembly):
        cases.append(case_type(name, init + (word,) + store, assembly,
                               dict(seeds), 0xB0000000, "simd"))

    same = {"cmeq": 0x2E208C00, "cmgt": 0x0E203400,
            "cmge": 0x0E203C00, "cmhi": 0x2E203400,
            "cmhs": 0x2E203C00, "cmtst": 0x0E208C00,
            "smax": 0x0E206400, "smin": 0x0E206C00,
            "umax": 0x2E206400, "umin": 0x2E206C00,
            "mla": 0x0E209400, "mls": 0x2E209400,
            "addp": 0x0E20BC00, "smaxp": 0x0E20A400,
            "sminp": 0x0E20AC00, "umaxp": 0x2E20A400, "uminp": 0x2E20AC00}
    for name, base in same.items():
        for width in (64, 128):
            for size, bits in enumerate((8, 16, 32, 64)):
                if bits == 64 and (width == 64 or name in ("smax", "smin", "umax", "umin", "mla", "mls", "smaxp", "sminp", "umaxp", "uminp")):
                    continue
                add(f"simd_{name}_{width}_e{bits}", base | ((width == 128) << 30) | (size << 22) | (2 << 16) | (1 << 5),
                    f"{name} v0.{width // bits}{'bhsd'[size]}, v1, v2")
    misc = {"abs": 0x0E20B800, "neg": 0x2E20B800,
            "cmeqz": 0x0E209800, "cmgtz": 0x0E208800,
            "cmgez": 0x2E208800, "cmltz": 0x0E20A800,
            "cmlez": 0x2E209800}
    for name, base in misc.items():
        for width in (64, 128):
            for size, bits in enumerate((8, 16, 32, 64)):
                if bits == 64 and width == 64:
                    continue
                add(f"simd_{name}_{width}_e{bits}", base | ((width == 128) << 30) | (size << 22) | (1 << 5), name)
    perm = {"uzp1": 0x0E001800, "uzp2": 0x0E005800,
            "zip1": 0x0E003800, "zip2": 0x0E007800,
            "trn1": 0x0E002800, "trn2": 0x0E006800}
    for name, base in perm.items():
        for width in (64, 128):
            for size, bits in enumerate((8, 16, 32, 64)):
                if width // bits < 2:
                    continue
                add(f"simd_{name}_{width}_e{bits}", base | ((width == 128) << 30) | (size << 22) | (2 << 16) | (1 << 5), name)
    for count in range(1, 5):
        for width in (64, 128):
            for extension in (False, True):
                name = "tbx" if extension else "tbl"
                add(f"simd_{name}{count}_{width}_alias", 0x0E000000 | ((width == 128) << 30) |
                    (4 << 16) | ((count - 1) << 13) | (extension << 12), name)
    reduce = {"addv": 0x0E31B800, "saddlv": 0x0E303800,
              "uaddlv": 0x2E303800, "smaxv": 0x0E30A800,
              "sminv": 0x0E31A800, "umaxv": 0x2E30A800,
              "uminv": 0x2E31A800}
    for name, base in reduce.items():
        for width in (64, 128):
            for size, bits in enumerate((8, 16, 32)):
                if width == 64 and bits == 32:
                    continue
                add(f"simd_{name}_{width}_e{bits}", base | ((width == 128) << 30) | (size << 22) | (1 << 5), name)
    wide = {"saddl": 0x0E200000, "uaddl": 0x2E200000,
            "ssubl": 0x0E202000, "usubl": 0x2E202000,
            "saddw": 0x0E201000, "uaddw": 0x2E201000,
            "ssubw": 0x0E203000, "usubw": 0x2E203000,
            "smull": 0x0E20C000, "umull": 0x2E20C000,
            "smlal": 0x0E208000, "umlal": 0x2E208000,
            "smlsl": 0x0E20A000, "umlsl": 0x2E20A000}
    for name, base in wide.items():
        for upper in (False, True):
            for size, bits in enumerate((8, 16, 32)):
                add(f"simd_{name}{'2' if upper else ''}_e{bits}", base | (upper << 30) | (size << 22) | (2 << 16) | (1 << 5), name)
    return cases
