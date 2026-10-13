"""Independent lane-level mathematical expectations for generated integer SIMD."""

import re
import struct


def expected_memory(case):
    if not case.name.startswith("simd_"):
        return None
    vectors = [struct.pack("<QQ", case.seeds[reg], case.seeds[reg + 1]) for reg in (1, 3, 5, 7, 9)]

    def lanes(vector, bits, signed=False):
        mask, sign = (1 << bits) - 1, 1 << (bits - 1)
        result = [int.from_bytes(vector[i:i + bits // 8], "little") for i in range(0, len(vector), bits // 8)]
        return [value - (mask + 1) if signed and value & sign else value for value in result]

    def pack(values, bits):
        return b"".join((value & ((1 << bits) - 1)).to_bytes(bits // 8, "little") for value in values)

    name = case.name
    table = re.fullmatch(r"simd_(tbl|tbx)([1-4])_(64|128)_alias", name)
    if table:
        operation, count, width = table.groups()
        width = int(width)
        lookup = b"".join(vectors[:int(count)])
        output = bytes(lookup[index] if index < len(lookup) else vectors[0][lane] if operation == "tbx" else 0
                       for lane, index in enumerate(vectors[4][:width // 8]))
        return output.ljust(16, b"\0") + b"".join(vectors[1:4])
    regular = re.fullmatch(r"simd_([a-z0-9]+)_(64|128)_e(8|16|32|64)", name)
    if regular:
        operation, width, bits = regular.groups()
        width, bits = int(width), int(bits)
        lhs, rhs = vectors[1][:width // 8], vectors[2][:width // 8]
        old = lanes(vectors[0][:width // 8], bits)
        left, right = lanes(lhs, bits), lanes(rhs, bits)
        signed_left, signed_right = lanes(lhs, bits, True), lanes(rhs, bits, True)
        lane_mask = (1 << bits) - 1
        if operation in ("cmeq", "cmgt", "cmge", "cmhi", "cmhs", "cmtst"):
            predicates = {"cmeq": [a == b for a, b in zip(left, right)],
                          "cmgt": [a > b for a, b in zip(signed_left, signed_right)],
                          "cmge": [a >= b for a, b in zip(signed_left, signed_right)],
                          "cmhi": [a > b for a, b in zip(left, right)],
                          "cmhs": [a >= b for a, b in zip(left, right)],
                          "cmtst": [bool(a & b) for a, b in zip(left, right)]}[operation]
            values = [lane_mask if predicate else 0 for predicate in predicates]
        elif operation in ("smax", "smin", "umax", "umin"):
            a, b = (signed_left, signed_right) if operation[0] == "s" else (left, right)
            reduce = max if operation.endswith("max") else min
            values = [reduce(x, y) for x, y in zip(a, b)]
        elif operation in ("mla", "mls"):
            values = [o + (1 if operation == "mla" else -1) * a * b for o, a, b in zip(old, left, right)]
        elif operation in ("abs", "neg"):
            values = [abs(a) if operation == "abs" else -a for a in signed_left]
        elif operation in ("cmeqz", "cmgtz", "cmgez", "cmltz", "cmlez"):
            predicates = {"cmeqz": [a == 0 for a in signed_left], "cmgtz": [a > 0 for a in signed_left],
                          "cmgez": [a >= 0 for a in signed_left], "cmltz": [a < 0 for a in signed_left],
                          "cmlez": [a <= 0 for a in signed_left]}[operation]
            values = [lane_mask if predicate else 0 for predicate in predicates]
        elif operation in ("uzp1", "uzp2", "zip1", "zip2", "trn1", "trn2"):
            parity = int(operation[-1]) - 1
            if operation.startswith("uzp"):
                values = left[parity::2] + right[parity::2]
            elif operation.startswith("trn"):
                values = [value for pair in zip(left[parity::2], right[parity::2]) for value in pair]
            else:
                half = len(left) // 2
                begin = parity * half
                values = [value for pair in zip(left[begin:begin + half], right[begin:begin + half]) for value in pair]
        elif operation in ("addp", "smaxp", "sminp", "umaxp", "uminp"):
            data = signed_left + signed_right if operation[0] == "s" else left + right
            reducer = (lambda a, b: a + b) if operation == "addp" else max if "max" in operation else min
            values = [reducer(data[index], data[index + 1]) for index in range(0, len(data), 2)]
        elif operation in ("addv", "saddlv", "uaddlv", "smaxv", "sminv", "umaxv", "uminv"):
            data = signed_left if operation[0] == "s" else left
            result = sum(data) if "add" in operation else max(data) if "max" in operation else min(data)
            result_bits = bits * 2 if operation in ("saddlv", "uaddlv") else bits
            return pack([result], result_bits).ljust(16, b"\0") + b"".join(vectors[1:4])
        else:
            raise ValueError(f"no independent math for {name}")
        return pack(values, bits).ljust(16, b"\0") + b"".join(vectors[1:4])
    wide = re.fullmatch(r"simd_([a-z]+)(2?)_e(8|16|32)", name)
    if wide:
        operation, upper, bits = wide.groups()
        bits = int(bits)
        half = 8 if upper else 0
        lhs = vectors[1][half:half + 8]
        rhs = vectors[2][half:half + 8]
        signed = operation[0] == "s"
        left, right = lanes(lhs, bits, signed), lanes(rhs, bits, signed)
        if operation.endswith("w"):
            left = lanes(vectors[1], bits * 2, signed)
        original = lanes(vectors[0], bits * 2)
        if "add" in operation:
            values = [a + b for a, b in zip(left, right)]
        elif "sub" in operation:
            values = [a - b for a, b in zip(left, right)]
        elif operation in ("smull", "umull"):
            values = [a * b for a, b in zip(left, right)]
        elif operation in ("smlal", "umlal", "smlsl", "umlsl"):
            sign = -1 if operation.endswith("sl") else 1
            values = [old + sign * a * b for old, a, b in zip(original, left, right)]
        else:
            raise ValueError(f"no independent wide math for {name}")
        return pack(values, bits * 2) + b"".join(vectors[1:4])
    raise ValueError(f"unrecognized SIMD case: {name}")
