// Numerical operations of the one DirectSink. No Program or second IR stream.
u64 FlushFloat(u64 bits, unsigned width, bool flush) {
    const auto exponent = width == 64 ? UINT64_C(0x7ff0000000000000) : UINT64_C(0x7f800000);
    return flush && !(bits & exponent) ? bits & (u64{1} << (width - 1)) : bits & Mask(width);
}

u64 ConvertFloatInteger(
        u64 bits, unsigned float_width, unsigned integer_width, bool truncate, bool daz) {
    bits = FlushFloat(bits, float_width, !truncate && daz);
    const double input = float_width == 64 ? std::bit_cast<double>(bits)
                                           : std::bit_cast<float>(static_cast<u32>(bits));
    const double integral = truncate ? std::trunc(input) : std::nearbyint(input);
    const double limit = std::ldexp(1.0, static_cast<int>(integer_width - 1));
    if (!std::isfinite(integral) || integral < -limit || integral >= limit)
        return u64{1} << (integer_width - 1);
    return static_cast<u64>(static_cast<std::int64_t>(integral)) & Mask(integer_width);
}

template <OpCode op> DirectSink::Value DirectSink::Floating(Type type, std::span<const Operand> a) {
    const auto raw = [&](unsigned i) { return Raw(a[i]); };
    const auto imm = [&](unsigned i) { return static_cast<unsigned>(a[i].payload); };
    const auto lane = [&](unsigned operand, unsigned index, unsigned width) {
        return static_cast<u64>(raw(operand) >> (index * width)) & Mask(width);
    };
    const auto put = [](u128& out, unsigned index, unsigned width, u64 bits) {
        const auto mask = u128{Mask(width)} << (index * width);
        out = (out & ~mask) | ((u128{bits} << (index * width)) & mask);
    };
    runtime::platform::HostFloatScope rounding(
            static_cast<runtime::platform::FloatRounding>((state.mxcsr >> 13) & 3));
    const bool daz = (state.mxcsr & (1u << 6)) != 0, ftz = (state.mxcsr & (1u << 15)) != 0;
    if constexpr (op == OpCode::VecFAdd || op == OpCode::VecFSub || op == OpCode::VecFMul ||
                  op == OpCode::VecFDiv || op == OpCode::VecFAddScalar32 ||
                  op == OpCode::VecFSubScalar32 || op == OpCode::VecFMulScalar32 ||
                  op == OpCode::VecFDivScalar32 || op == OpCode::VecFAddScalar64 ||
                  op == OpCode::VecFSubScalar64 || op == OpCode::VecFMulScalar64 ||
                  op == OpCode::VecFDivScalar64) {
        constexpr bool single = op == OpCode::VecFAddScalar32 || op == OpCode::VecFSubScalar32 ||
                                op == OpCode::VecFMulScalar32 || op == OpCode::VecFDivScalar32;
        constexpr bool scalar = single || op == OpCode::VecFAddScalar64 ||
                                op == OpCode::VecFSubScalar64 || op == OpCode::VecFMulScalar64 ||
                                op == OpCode::VecFDivScalar64;
        const auto width = scalar ? (single ? 32u : 64u) : imm(2);
        u128 out = scalar ? raw(0) : 0;
        for (unsigned i = 0; i < (scalar ? 1u : 128 / width); ++i) {
            const auto left = FlushFloat(lane(0, i, width), width, daz),
                       right = FlushFloat(lane(1, i, width), width, daz);
            u64 bits;
            if (width == 64) {
                volatile double l = std::bit_cast<double>(left), r = std::bit_cast<double>(right);
                double result;
                if constexpr (op == OpCode::VecFAdd || op == OpCode::VecFAddScalar64)
                    result = l + r;
                else if constexpr (op == OpCode::VecFSub || op == OpCode::VecFSubScalar64)
                    result = l - r;
                else if constexpr (op == OpCode::VecFMul || op == OpCode::VecFMulScalar64)
                    result = l * r;
                else
                    result = l / r;
                bits = std::bit_cast<u64>(result);
            } else {
                volatile float l = std::bit_cast<float>(static_cast<u32>(left)),
                               r = std::bit_cast<float>(static_cast<u32>(right));
                float result;
                if constexpr (op == OpCode::VecFAdd || op == OpCode::VecFAddScalar32)
                    result = l + r;
                else if constexpr (op == OpCode::VecFSub || op == OpCode::VecFSubScalar32)
                    result = l - r;
                else if constexpr (op == OpCode::VecFMul || op == OpCode::VecFMulScalar32)
                    result = l * r;
                else
                    result = l / r;
                bits = std::bit_cast<u32>(result);
            }
            put(out, i, width, FlushFloat(bits, width, ftz));
        }
        return Number(out, Type::V128);
    } else if constexpr (op == OpCode::VecFMinMax) {
        const auto width = imm(2), count = imm(4) ? 1 : 128 / width;
        u128 out = imm(4) ? raw(0) : 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto left = FlushFloat(lane(0, i, width), width, daz),
                       right = FlushFloat(lane(1, i, width), width, daz);
            const double l = width == 64 ? std::bit_cast<double>(left)
                                         : std::bit_cast<float>(static_cast<u32>(left));
            const double r = width == 64 ? std::bit_cast<double>(right)
                                         : std::bit_cast<float>(static_cast<u32>(right));
            put(out, i, width, (imm(3) ? l > r : l < r) ? left : right);
        }
        return Number(out, Type::V128);
    } else if constexpr (op == OpCode::VecFUnary || op == OpCode::VecFRoundInt) {
        const auto width = imm(2), kind = imm(3), count = imm(4) ? 1 : 128 / width;
        u128 out = imm(4) ? raw(0) : 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto input = FlushFloat(
                    lane(1, i, width), width, op == OpCode::VecFUnary && kind ? true : daz);
            u64 bits;
            if (width == 64) {
                volatile double v = std::bit_cast<double>(input);
                double result;
                if constexpr (op == OpCode::VecFRoundInt) {
                    runtime::platform::HostFloatScope mode(
                            static_cast<runtime::platform::FloatRounding>(kind));
                    result = std::nearbyint(v);
                } else
                    result = kind == 0 ? std::sqrt(v) : kind == 1 ? 1.0 / v : 1.0 / std::sqrt(v);
                bits = std::bit_cast<u64>(result);
            } else {
                volatile float v = std::bit_cast<float>(static_cast<u32>(input));
                float result;
                if constexpr (op == OpCode::VecFRoundInt) {
                    runtime::platform::HostFloatScope mode(
                            static_cast<runtime::platform::FloatRounding>(kind));
                    result = std::nearbyint(v);
                } else
                    result = kind == 0 ? std::sqrt(v) : kind == 1 ? 1.0f / v : 1.0f / std::sqrt(v);
                bits = std::bit_cast<u32>(result);
            }
            put(out,
                i,
                width,
                FlushFloat(bits, width, op == OpCode::VecFUnary && kind ? true : ftz));
        }
        return Number(out, Type::V128);
    } else if constexpr (op == OpCode::VecFCmp || op == OpCode::VecFCmpMask) {
        const auto width = imm(2);
        const bool scalar = op == OpCode::VecFCmp || imm(4);
        u128 out = op == OpCode::VecFCmpMask && scalar ? raw(0) : 0;
        for (unsigned i = 0; i < (scalar ? 1u : 128 / width); ++i) {
            const auto left = FlushFloat(lane(0, i, width), width, daz),
                       right = FlushFloat(lane(1, i, width), width, daz);
            const double l = width == 64 ? std::bit_cast<double>(left)
                                         : std::bit_cast<float>(static_cast<u32>(left));
            const double r = width == 64 ? std::bit_cast<double>(right)
                                         : std::bit_cast<float>(static_cast<u32>(right));
            const bool unordered = std::isnan(l) || std::isnan(r);
            if constexpr (op == OpCode::VecFCmp)
                return Number(unordered ? 7 : l < r ? 1 : l == r ? 4 : 0, Type::U32);
            else {
                const auto relation = unordered ? 8u : l < r ? 1u : l == r ? 2u : 4u;
                put(out, i, width, imm(3) & relation ? Mask(width) : 0);
            }
        }
        return Number(out, Type::V128);
    } else if constexpr (op == OpCode::VecFCvtIntToFloat) {
        volatile std::int64_t value = Signed(static_cast<u64>(raw(0)), imm(1));
        if (imm(2) == 32) return Number(std::bit_cast<u32>(static_cast<float>(value)), type);
        return Number(std::bit_cast<u64>(static_cast<double>(value)), type);
    } else if constexpr (op == OpCode::VecFCvtFloatToInt) {
        return Number(
                ConvertFloatInteger(static_cast<u64>(raw(0)), imm(1), imm(2), imm(3) != 0, daz),
                type);
    } else if constexpr (op == OpCode::VecFCvtScalar) {
        const auto source = imm(1);
        const auto input = FlushFloat(static_cast<u64>(raw(0)), source, daz);
        if (source == 32) {
            volatile float value = std::bit_cast<float>(static_cast<u32>(input));
            return Number(FlushFloat(std::bit_cast<u64>(static_cast<double>(value)), 64, ftz),
                          type);
        }
        volatile double value = std::bit_cast<double>(input);
        return Number(FlushFloat(std::bit_cast<u32>(static_cast<float>(value)), 32, ftz), type);
    } else if constexpr (op == OpCode::VecFCvtPacked) {
        const auto kind = imm(1);
        u128 out{};
        if (kind == 0 || kind == 1) {
            const auto width = kind == 0 ? 32u : 64u;
            for (unsigned i = 0; i < 128 / width; ++i) {
                volatile std::int32_t value = static_cast<std::int32_t>(lane(0, i, 32));
                put(out,
                    i,
                    width,
                    kind == 0 ? std::bit_cast<u32>(static_cast<float>(value))
                              : std::bit_cast<u64>(static_cast<double>(value)));
            }
        } else if (kind <= 5) {
            const auto width = kind <= 3 ? 32u : 64u;
            for (unsigned i = 0; i < 128 / width; ++i)
                put(out,
                    i,
                    32,
                    ConvertFloatInteger(lane(0, i, width), width, 32, kind == 3 || kind == 5, daz));
        } else if (kind == 6) {
            for (unsigned i = 0; i < 2; ++i) {
                volatile float value =
                        std::bit_cast<float>(static_cast<u32>(FlushFloat(lane(0, i, 32), 32, daz)));
                put(out, i, 64, std::bit_cast<u64>(static_cast<double>(value)));
            }
        } else if (kind == 7) {
            for (unsigned i = 0; i < 2; ++i) {
                volatile double value = std::bit_cast<double>(FlushFloat(lane(0, i, 64), 64, daz));
                put(out, i, 32, FlushFloat(std::bit_cast<u32>(static_cast<float>(value)), 32, ftz));
            }
        } else
            throw detail::Rejected{BuildError::Unsupported};
        return Number(out, Type::V128);
    } else if constexpr (op == OpCode::VecFMulAdd) {
        const auto width = imm(3), flags = imm(4), count = flags & 4 ? 1 : 128 / width;
        u128 out = flags & 4 ? raw(0) : 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto x = FlushFloat(lane(0, i, width), width, daz),
                       y = FlushFloat(lane(1, i, width), width, daz);
            const auto z = FlushFloat(lane(2, i, width), width, daz);
            const bool negative_add =
                    bool(flags & 2) ^ (bool(flags & 8) && ((i & 1) == ((flags >> 4) & 1)));
            const auto exponent = width == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
            const auto fraction = width == 32 ? UINT64_C(0x7fffff) : UINT64_C(0xfffffffffffff);
            u64 bits{};
            const auto nan = [&](u64 v) { return (v & exponent) == exponent && (v & fraction); };
            if (nan(x) || nan(y) || nan(z))
                bits = (nan(x) ? x : nan(y) ? y : z) | (u64{1} << (width == 32 ? 22 : 51));
            else if (width == 32) {
                volatile float first = std::bit_cast<float>(static_cast<u32>(x)),
                               second = std::bit_cast<float>(static_cast<u32>(y));
                volatile float addend = std::bit_cast<float>(static_cast<u32>(z));
                bits = std::bit_cast<u32>(std::fma(
                        flags & 1 ? -first : first, second, negative_add ? -addend : addend));
            } else {
                volatile double first = std::bit_cast<double>(x), second = std::bit_cast<double>(y),
                                addend = std::bit_cast<double>(z);
                bits = std::bit_cast<u64>(std::fma(
                        flags & 1 ? -first : first, second, negative_add ? -addend : addend));
            }
            put(out, i, width, FlushFloat(bits, width, ftz));
        }
        return Number(out, Type::V128);
    } else
        return Crypto<op>(type, a);
}
