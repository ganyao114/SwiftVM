// Included in backend_test.cpp's anonymous namespace to share the IR fixtures.
u64 EncodeFP(double value, u32 bits) {
    return bits == 32 ? std::bit_cast<u32>(static_cast<float>(value)) : std::bit_cast<u64>(value);
}

void RoundIntegralNativeAndBaseline() {
    struct Case { double input; std::array<double, 4> output; };
    const std::array<Case, 15> cases{{
        {0.0, {0.0, 0.0, 0.0, 0.0}}, {-0.0, {-0.0, -0.0, -0.0, -0.0}},
        {0.5, {0.0, 0.0, 1.0, 0.0}}, {-0.5, {-0.0, -1.0, -0.0, -0.0}},
        {1.5, {2.0, 1.0, 2.0, 1.0}}, {-1.5, {-2.0, -2.0, -1.0, -1.0}},
        {2.5, {2.0, 2.0, 3.0, 2.0}}, {-2.5, {-2.0, -3.0, -2.0, -2.0}},
        {0.25, {0.0, 0.0, 1.0, 0.0}}, {-0.25, {-0.0, -1.0, -0.0, -0.0}},
        {0.75, {1.0, 0.0, 1.0, 0.0}}, {-0.75, {-1.0, -1.0, -0.0, -0.0}},
        {8388609.0, {8388609.0, 8388609.0, 8388609.0, 8388609.0}},
        {4503599627370495.5, {4503599627370496.0, 4503599627370495.0, 4503599627370496.0, 4503599627370495.0}},
        {4503599627370496.0, {4503599627370496.0, 4503599627370496.0, 4503599627370496.0, 4503599627370496.0}}
    }};
    u64 seed = 0x574829adafeeULL;
    for (u32 bits : {32u, 64u}) for (u32 mode = 0; mode < 4; ++mode) for (bool scalar : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4800})}; ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto merge = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto result = as.VecFRoundInt(input, merge, ir::Imm{bits}, ir::Imm{mode}, ir::Imm{u32(scalar)}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        const u64 sign = u64{1} << (bits - 1);
        const u64 inf = bits == 32 ? 0x7f800000 : 0x7ff0000000000000ULL;
        const u64 quiet = bits == 32 ? 0x00400000 : 0x0008000000000000ULL;
        for (u32 iteration = 0; iteration < 160; ++iteration) {
            Storage observed, portable, expected;
            for (u32 lane = 0; lane < 128 / bits; ++lane) {
                u64 raw;
                if (iteration < cases.size()) raw = EncodeFP(cases[iteration].input, bits);
                else if (iteration < cases.size() + 10) {
                    const std::array<u64, 10> special{inf, inf | sign, inf | 0x1234, inf | sign | quiet | 0x5678,
                        1, sign | 1, inf - 1, (inf - 1) | sign, (inf - 1) >> 1, sign | ((inf - 1) >> 1)};
                    raw = special[iteration - cases.size()];
                } else raw = Random(seed);
                std::memcpy(observed.state->uniform_buffer_begin + lane * bits / 8, &raw, bits / 8);
            }
            observed.Put(16, Random(seed)); observed.Put(24, Random(seed));
            std::memcpy(portable.state->uniform_buffer_begin, observed.state->uniform_buffer_begin, 32);
            std::memcpy(expected.state->uniform_buffer_begin, observed.state->uniform_buffer_begin, 32);
            backend::interp::Interpreter reference{*expected.state, block.get()}; const auto wanted = reference.Run();
            Check(native.fn(observed.state) == wanted && baseline.fn(portable.state) == wanted, "native/baseline integral round halt");
            Check(observed.Get(32) == expected.Get(32) && observed.Get(40) == expected.Get(40) &&
                  portable.Get(32) == expected.Get(32) && portable.Get(40) == expected.Get(40), "round modes/scalar merge edge/random bits");
            if (iteration < cases.size() && !(bits == 32 && iteration == 13)) {
                const u64 result_bits = observed.Get(32) & (bits == 32 ? UINT32_MAX : UINT64_MAX);
                Check(result_bits == EncodeFP(cases[iteration].output[mode], bits), "independent mathematical round boundary");
            }
        }
    }
    std::cout << "PASS all integral rounding modes, scalar merges, native and SSE2 baseline\n";
}

void FusedMultiplyAddNativeAndBaseline() {
    u64 seed = 0x961894acef23ULL;
    for (u32 bits : {32u, 64u}) for (u32 flags = 0; flags < 4; ++flags) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4900})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto c = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
        auto result = as.VecFMulAdd(a, b, c, ir::Imm{bits}, ir::Imm{flags}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{48, ValueType::V128}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get()}, baseline{block.get(), {}};
        const u64 sign = u64{1} << (bits - 1);
        const u64 inf = bits == 32 ? 0x7f800000 : 0x7ff0000000000000ULL;
        const u64 quiet = bits == 32 ? 0x00400000 : 0x0008000000000000ULL;
        const u64 one = EncodeFP(1.0, bits), two = EncodeFP(2.0, bits);
        const u64 tiny_square = bits == 32 ? 0xa8800000 : 0xb970000000000000ULL;
        struct Case { u64 a, b, c, expected; };
        const auto one_and_half = EncodeFP(1.5, bits);
        const auto far_below_ulp = EncodeFP(bits == 32 ? 0x1p-100 : 0x1p-200, bits);
        const std::array<Case, 18> fixed{{
            {one + 1, one - 2, one | sign, tiny_square}, // (1+eps)(1-eps)-1 = -eps^2
            {inf - 1, two, (inf - 1) | sign, inf - 1}, // unrounded overflowing product cancels
            {1, EncodeFP(0.5, bits), 1, 2}, // 1.5 minimum subnormals ties to even
            {one, one, one | sign, 0},
            {0, one, sign, 0}, {sign, one, sign, sign},
            {inf, one, one, inf}, {inf | sign, one, one, inf | sign},
            {0, inf, one, inf | quiet | sign}, {inf, one, inf | sign, inf | quiet | sign},
            {inf | 0x1234, inf | quiet | 0x5678, inf | sign | 0x9876, inf | quiet | 0x1234},
            {one, inf | sign | 0x1234, inf | quiet | 0x5678, inf | quiet | sign | 0x1234},
            {one, one, inf | 0x1234, inf | quiet | 0x1234},
            {inf | quiet | sign | 0x4321, one, one, inf | quiet | sign | 0x4321},
            {inf - 1, two, 0, inf}, {one, one, 1, one},
            // Tiny addends straddle exact product ties. The f64 correction lies
            // beyond f128 precision, exercising round-to-odd before narrowing.
            {one + 1, one_and_half, far_below_ulp | sign, one_and_half + 1},
            {one + 3, one_and_half, far_below_ulp, one_and_half + 5}
        }};
        for (u32 iteration = 0; iteration < 640; ++iteration) {
            Storage actual, portable, expected;
            const auto nan = [&](u64 raw) { return (raw & inf) == inf && (raw & (quiet * 2 - 1)); };
            for (u32 lane = 0; lane < 128 / bits; ++lane) for (u32 operand = 0; operand < 3; ++operand) {
                u64 raw;
                if (iteration < fixed.size()) {
                    const auto& test = fixed[iteration]; raw = operand == 0 ? test.a : operand == 1 ? test.b : test.c;
                    // Preserve the exact operation across sign modes; NaNs keep
                    // their original sign/payload because sign-mode ignores it.
                    if (!nan(raw) && ((operand == 0 && (flags & 1)) || (operand == 2 && (flags & 2)))) raw ^= sign;
                } else raw = Random(seed);
                std::memcpy(actual.state->uniform_buffer_begin + operand * 16 + lane * bits / 8, &raw, bits / 8);
            }
            std::memcpy(portable.state->uniform_buffer_begin, actual.state->uniform_buffer_begin, 48);
            std::memcpy(expected.state->uniform_buffer_begin, actual.state->uniform_buffer_begin, 48);
            backend::interp::Interpreter reference{*expected.state, block.get()}; const auto wanted = reference.Run();
            Check(native.fn(actual.state) == wanted && baseline.fn(portable.state) == wanted, "native/baseline fused multiply-add halt");
            Check(actual.Get(48) == expected.Get(48) && actual.Get(56) == expected.Get(56) &&
                  portable.Get(48) == expected.Get(48) && portable.Get(56) == expected.Get(56), "all FMA signs/NaN priorities/edge/random bits");
            if (iteration < fixed.size()) {
                for (u32 lane = 0; lane < 128 / bits; ++lane) {
                    u64 raw{}; std::memcpy(&raw, actual.state->uniform_buffer_begin + 48 + lane * bits / 8, bits / 8);
                    Check(raw == fixed[iteration].expected, "independent exact fused/overflow/underflow/NaN boundary");
                }
            }
        }
    }
    std::cout << "PASS fused multiply-add all signs, exact boundaries and baseline fallback\n";
}
