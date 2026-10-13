"""Exact scalar fixed-point conversion cases and a rational mathematical oracle."""

from fractions import Fraction
import struct


def decode(bits, width):
    exponent_bits, fraction_bits, bias = (8, 23, 127) if width == 32 else (11, 52, 1023)
    sign = bits >> (width - 1)
    exponent = (bits >> fraction_bits) & ((1 << exponent_bits) - 1)
    fraction = bits & ((1 << fraction_bits) - 1)
    if exponent == (1 << exponent_bits) - 1:
        return None, "nan" if fraction else "inf", sign
    significand = fraction if exponent == 0 else fraction | (1 << fraction_bits)
    scale = (1 if exponent == 0 else exponent) - bias - fraction_bits
    value = Fraction(significand) * (Fraction(1 << scale) if scale >= 0 else Fraction(1, 1 << -scale))
    return -value if sign else value, "subnormal" if exponent == 0 and fraction else "finite", sign


def encode(value, width, rounding):
    if value == 0:
        return 0, 0
    negative = value < 0
    value = abs(value)
    fraction_bits, bias = (23, 127) if width == 32 else (52, 1023)
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if value < (Fraction(1 << exponent) if exponent >= 0 else Fraction(1, 1 << -exponent)):
        exponent -= 1
    shift = fraction_bits - exponent
    scaled = value * (Fraction(1 << shift) if shift >= 0 else Fraction(1, 1 << -shift))
    quotient, remainder = divmod(scaled.numerator, scaled.denominator)
    inexact = remainder != 0
    increment = ((2 * remainder > scaled.denominator or
                  (2 * remainder == scaled.denominator and quotient & 1)) if rounding == 0 else
                 inexact and (not negative if rounding == 1 else negative if rounding == 2 else False))
    quotient += int(increment)
    if quotient == 1 << (fraction_bits + 1):
        quotient >>= 1
        exponent += 1
    result = (int(negative) << (width - 1)) | ((exponent + bias) << fraction_bits) | (quotient & ((1 << fraction_bits) - 1))
    return result, 16 if inexact else 0


def expected(case):
    _, direction, integer_width, float_width, scale, _ = case.name.split("_", 5)
    integer_width, float_width, scale = int(integer_width[1:]), int(float_width[1:]), int(scale[1:])
    operand = case.seeds[1]
    fpcr = case.seeds[27]
    if direction in ("scvtf", "ucvtf"):
        value = operand & ((1 << integer_width) - 1)
        if direction == "scvtf" and value & (1 << (integer_width - 1)):
            value -= 1 << integer_width
        result, flags = encode(Fraction(value, 1 << scale), float_width, (fpcr >> 22) & 3)
        return {"memory": result.to_bytes(float_width // 8, "little").ljust(16, b"\0"), "fpsr": flags, "fpcr": fpcr}
    value, kind, sign = decode(operand, float_width)
    unsigned = direction == "fcvtzu"
    minimum = 0 if unsigned else -(1 << (integer_width - 1))
    maximum = (1 << integer_width) - 1 if unsigned else (1 << (integer_width - 1)) - 1
    flags = 0
    if kind == "nan":
        result, flags = 0, 1
    elif kind == "inf":
        result, flags = minimum if sign else maximum, 1
    else:
        if kind == "subnormal" and fpcr & (1 << 24):
            value, flags = Fraction(0), 128
        scaled = value * (1 << scale)
        result = int(scaled)  # Fraction truncates toward zero.
        if result < minimum or result > maximum:
            result, flags = min(max(result, minimum), maximum), flags | 1
        elif scaled != result:
            flags |= 16
    return {"x0": result & ((1 << integer_width) - 1), "fpsr": flags, "fpcr": fpcr}


def corpus(case_type):
    result = []
    for direction, opcode in (("scvtf", 2), ("ucvtf", 3), ("fcvtzs", 24), ("fcvtzu", 25)):
        for integer_width in (32, 64):
            for float_width in (32, 64):
                for scale in (1, integer_width // 2, integer_width):
                    word = 0x1E000020 | ((integer_width == 64) << 31) | ((float_width == 64) << 22)
                    word |= opcode << 16 | (64 - scale) << 10
                    if direction in ("scvtf", "ucvtf"):
                        operands = [("power", 1 << (integer_width - 2), 0),
                                    ("minimum", 1 << (integer_width - 1), 0),
                                    ("max", (1 << integer_width) - 1, 0),
                                    ("roundup", (1 << integer_width) - 1, 1 << 22)]
                    else:
                        f = lambda value: int.from_bytes(struct.pack("<f" if float_width == 32 else "<d", value), "little")
                        operands = [("fraction", f(1.25), 0), ("negative", f(-0.75), 0),
                                    ("nan", 0x7FC00123 if float_width == 32 else 0x7FF8000000000123, 0),
                                    ("inf", f(float("inf")), 0),
                                    ("neginf", f(float("-inf")), 0),
                                    ("subnormal", 1, 0),
                                    ("subnormalfz", 1, 1 << 24)]
                    for label, operand, fpcr in operands:
                        initial = (0xD51B441B, 0xD51B443C)
                        if direction in ("fcvtzs", "fcvtzu"):
                            initial += ((0x1E270021 if float_width == 32 else 0x9E670021),)
                        capture = (0xD53B443B, 0xD53B441C)
                        if direction in ("scvtf", "ucvtf"):
                            capture += (0x3D8003A0,)
                        name = f"fixed_{direction}_i{integer_width}_f{float_width}_s{scale}_{label}"
                        result.append(case_type(name, initial + (word,) + capture, name,
                                                {1: operand, 27: fpcr, 28: 0}, 0xB0000000, "fp_fixed"))
    return result
