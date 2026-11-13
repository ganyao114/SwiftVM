// Numerical operations of the direct IR consumer; no Program or instruction traversal.
template <OpCode op>
Interpreter::Value Interpreter::HandleVector(Type type, std::span<const Operand> a) {
    const auto value = [&](unsigned i) { return Raw(a[i]); };
    const auto imm = [&](unsigned i) { return static_cast<unsigned>(a[i].payload); };
    const auto lane = [](u128 v, unsigned i, unsigned width) {
        return static_cast<u64>(v >> (i * width)) & Mask(width);
    };
    const auto put = [](u128& v, unsigned i, unsigned width, u64 bits) {
        const auto mask = u128{Mask(width)} << (i * width);
        v = (v & ~mask) | ((u128{bits} << (i * width)) & mask);
    };
    u128 result{};
    if constexpr (op == OpCode::VecLoadConst)
        return Number(u128{a[0].payload} | (u128{a[1].payload} << 64), Type::V128);
    else if constexpr (op == OpCode::VecSharedZero)
        return Number(0, Type::V128);
    else if constexpr (op == OpCode::VecDup64)
        return Number(u128{static_cast<u64>(value(0))} * ((u128{1} << 64) | 1), Type::V128);
    else if constexpr (op == OpCode::VecScalar)
        return Number(u128{static_cast<u64>(Read(a[0]))}, Type::V128);
    else if constexpr (op == OpCode::VecExtract64)
        return Number(value(0) >> (imm(1) * 64), type);
    else if constexpr (op == OpCode::VecExtract16)
        return Number(lane(value(0), imm(1), 16), type);
    else if constexpr (op == OpCode::VecInsert16) {
        result = value(0);
        put(result, imm(2), 16, static_cast<u64>(Read(a[1])));
    } else if constexpr (op == OpCode::VecTableLookup8) {
        for (unsigned i = 0; i < 16; ++i) {
            const auto index = lane(value(1), i, 8);
            put(result, i, 8, index & 128 ? 0 : lane(value(0), index & 15, 8));
        }
    } else if constexpr (op == OpCode::VecDupPairs32) {
        for (unsigned i = 0; i < 4; ++i) put(result, i, 32, lane(value(0), (i & 2) + imm(1), 32));
    } else if constexpr (op == OpCode::VecXor || op == OpCode::VecAnd || op == OpCode::VecOr ||
                         op == OpCode::VecAndNot) {
        if constexpr (op == OpCode::VecXor)
            result = value(0) ^ value(1);
        else if constexpr (op == OpCode::VecAnd)
            result = value(0) & value(1);
        else if constexpr (op == OpCode::VecOr)
            result = value(0) | value(1);
        else
            result = value(0) & ~value(1);
    } else if constexpr (op == OpCode::VecAdd || op == OpCode::VecSub || op == OpCode::VecMul ||
                         op == OpCode::VecCmpEq || op == OpCode::VecCmpGt || op == OpCode::VecMin ||
                         op == OpCode::VecMax || op == OpCode::VecAvg) {
        const auto width = imm(2);
        for (unsigned i = 0; i < 128 / width; ++i) {
            const auto left = lane(value(0), i, width), right = lane(value(1), i, width);
            u64 out{};
            if constexpr (op == OpCode::VecAdd)
                out = left + right;
            else if constexpr (op == OpCode::VecSub)
                out = left - right;
            else if constexpr (op == OpCode::VecMul)
                out = left * right;
            else if constexpr (op == OpCode::VecCmpEq)
                out = left == right ? Mask(width) : 0;
            else if constexpr (op == OpCode::VecCmpGt)
                out = Signed(left, width) > Signed(right, width) ? Mask(width) : 0;
            else if constexpr (op == OpCode::VecAvg)
                out = static_cast<u64>((u128{left} + right + 1) >> 1);
            else if constexpr (op == OpCode::VecMin)
                out = imm(3) ? (Signed(left, width) < Signed(right, width) ? left : right)
                             : std::min(left, right);
            else
                out = imm(3) ? (Signed(left, width) > Signed(right, width) ? left : right)
                             : std::max(left, right);
            put(result, i, width, out);
        }
    } else if constexpr (op == OpCode::VecMulWiden || op == OpCode::VecSignedMulWidenLow) {
        const auto width = imm(2), output = width * 2;
        for (unsigned i = 0; i < 128 / output; ++i) {
            const auto index = op == OpCode::VecMulWiden ? i * 2 : i;
            const auto left = lane(value(0), index, width), right = lane(value(1), index, width);
            const bool sign = op == OpCode::VecSignedMulWidenLow || imm(3);
            const auto product = sign ? static_cast<u64>(Signed(left, width) * Signed(right, width))
                                      : left * right;
            put(result, i, output, product);
        }
    } else if constexpr (op == OpCode::VecMadd16) {
        for (unsigned i = 0; i < 4; ++i) {
            const auto first =
                    Signed(lane(value(0), i * 2, 16), 16) * Signed(lane(value(1), i * 2, 16), 16);
            const auto second = Signed(lane(value(0), i * 2 + 1, 16), 16) *
                                Signed(lane(value(1), i * 2 + 1, 16), 16);
            put(result, i, 32, static_cast<u64>(first + second));
        }
    } else if constexpr (op == OpCode::VecSatAdd || op == OpCode::VecSatSub) {
        const auto width = imm(2);
        for (unsigned i = 0; i < 128 / width; ++i) {
            const auto left = lane(value(0), i, width), right = lane(value(1), i, width);
            u64 out;
            if (imm(3)) {
                const s128 sum = op == OpCode::VecSatAdd
                                         ? s128{Signed(left, width)} + Signed(right, width)
                                         : s128{Signed(left, width)} - Signed(right, width);
                const s128 minimum = -(s128{1} << (width - 1)),
                           maximum = (s128{1} << (width - 1)) - 1;
                out = static_cast<u64>(std::clamp(sum, minimum, maximum));
            } else if constexpr (op == OpCode::VecSatSub)
                out = left < right ? 0 : left - right;
            else
                out = static_cast<u64>(std::min(u128{left} + right, u128{Mask(width)}));
            put(result, i, width, out);
        }
    } else if constexpr (op == OpCode::VecMulHigh16) {
        for (unsigned i = 0; i < 8; ++i) {
            const auto l = lane(value(0), i, 16), r = lane(value(1), i, 16);
            const auto product = imm(2) ? static_cast<u64>(Signed(l, 16) * Signed(r, 16)) : l * r;
            put(result, i, 16, product >> 16);
        }
    } else if constexpr (op == OpCode::VecAbsDiffSum8) {
        for (unsigned half = 0; half < 2; ++half) {
            unsigned sum{};
            for (unsigned i = 0; i < 8; ++i) {
                const auto l = lane(value(0), half * 8 + i, 8), r = lane(value(1), half * 8 + i, 8);
                sum += static_cast<unsigned>(l > r ? l - r : r - l);
            }
            put(result, half, 64, sum);
        }
    } else if constexpr (op == OpCode::VecPack) {
        const auto source = imm(2), width = source / 2, lanes = 128 / source;
        const auto minimum = imm(3) ? 0 : -(std::int64_t{1} << (width - 1));
        const auto maximum = imm(3) ? static_cast<std::int64_t>(Mask(width))
                                    : (std::int64_t{1} << (width - 1)) - 1;
        for (unsigned i = 0; i < lanes * 2; ++i)
            put(result,
                i,
                width,
                static_cast<u64>(
                        std::clamp(Signed(lane(value(i / lanes), i % lanes, source), source),
                                   minimum,
                                   maximum)));
    } else if constexpr (op == OpCode::VecExtend) {
        const auto source = imm(1), output = imm(2);
        for (unsigned i = 0; i < 128 / output; ++i) {
            const auto v = lane(value(0), i, source);
            put(result, i, output, imm(3) ? static_cast<u64>(Signed(v, source)) : v);
        }
    } else if constexpr (op == OpCode::VecShiftLeftImm || op == OpCode::VecShiftRightImm ||
                         op == OpCode::VecShiftRightArithmeticImm || op == OpCode::VecShiftLeft ||
                         op == OpCode::VecShiftRight || op == OpCode::VecShiftRightArithmetic) {
        const auto width = imm(2);
        for (unsigned i = 0; i < 128 / width; ++i) {
            const auto count = View(a[1]) == Type::V128 ? lane(value(1), i, width)
                                                        : static_cast<u64>(Read(a[1]));
            const auto v = lane(value(0), i, width);
            u64 out{};
            if constexpr (op == OpCode::VecShiftRightArithmeticImm ||
                          op == OpCode::VecShiftRightArithmetic)
                out = static_cast<u64>(Signed(v, width) >> std::min(count, u64{width - 1}));
            else if (count < width)
                out = (op == OpCode::VecShiftLeftImm || op == OpCode::VecShiftLeft) ? v << count
                                                                                    : v >> count;
            put(result, i, width, out);
        }
    } else if constexpr (op == OpCode::VecByteShift) {
        const auto shift = imm(2) * 8;
        if (!shift)
            result = value(0);
        else if (shift < 128)
            result = imm(3) ? (value(0) << shift) | (value(1) >> (128 - shift))
                            : (value(0) >> shift) | (value(1) << (128 - shift));
    } else if constexpr (op == OpCode::VecExtractBytes) {
        const auto shift = imm(2) * 8;
        result = !shift ? value(0) : (value(0) >> shift) | (value(1) << (128 - shift));
    } else if constexpr (op == OpCode::VecShuffle32 || op == OpCode::VecShuffle32TwoSrc) {
        const auto control = imm(op == OpCode::VecShuffle32 ? 1 : 2);
        for (unsigned i = 0; i < 4; ++i)
            put(result,
                i,
                32,
                lane(value(op == OpCode::VecShuffle32TwoSrc && i >= 2 ? 1 : 0),
                     (control >> (2 * i)) & 3,
                     32));
    } else if constexpr (op == OpCode::VecShuffle16) {
        result = value(0);
        const auto base = imm(2) ? 4u : 0u;
        for (unsigned i = 0; i < 4; ++i)
            put(result, i + base, 16, lane(value(0), base + ((imm(1) >> (2 * i)) & 3), 16));
    } else if constexpr (op == OpCode::VecZip || op == OpCode::VecUnzip) {
        const auto width = imm(2), lanes = 128 / width;
        if constexpr (op == OpCode::VecZip) {
            const auto base = imm(3) ? lanes / 2 : 0;
            for (unsigned i = 0; i < lanes / 2; ++i) {
                put(result, i * 2, width, lane(value(0), base + i, width));
                put(result, i * 2 + 1, width, lane(value(1), base + i, width));
            }
        } else
            for (unsigned i = 0; i < lanes; ++i) {
                const auto index = i * 2 + imm(3);
                put(result, i, width, lane(value(index / lanes), index % lanes, width));
            }
    } else if constexpr (op == OpCode::VecMovMask) {
        const auto width = imm(1);
        for (unsigned i = 0; i < 128 / width; ++i)
            result |= u128{lane(value(0), i, width) >> (width - 1)} << i;
        return Number(result, Type::U32);
    } else
        return HandleFloating<op>(type, a);
    return Number(result, type);
}
