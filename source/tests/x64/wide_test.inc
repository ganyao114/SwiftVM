// Included inside backend_test.cpp's fixture namespace.
HostPairResult CpuidPairProbe(u64 leaf, u64 subleaf, u64 options) {
    return {(leaf * 0x1020304050607081ULL) ^ options,
            std::rotl(subleaf + 0xFEDCBA9876543210ULL, 19) ^ leaf};
}

void CpuidPairAndDelayedUpper() {
    for (bool delayed : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5800})}; ir::Assembler as{block.get()};
        auto leaf = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto subleaf = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        constexpr u64 options = 0xFEDCBADC87654321;
        auto first = as.Cpuid(leaf, subleaf, ir::Imm{options}, ir::Imm{reinterpret_cast<u64>(&CpuidPairProbe)})
                          .SetType(ValueType::U64);
        if (delayed) for (u32 i = 0; i < 40; ++i) {
            auto temporary = as.Add<ir::U64>(leaf, ir::Operand{ir::Imm{u64{i}}}).SetType(ValueType::U64);
            as.StoreUniform(ir::Uniform{40, ValueType::U64}, temporary);
        }
        auto second = as.CpuidUpper(first).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{16, ValueType::U64}, first);
        as.StoreUniform(ir::Uniform{24, ValueType::U64}, second);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        u64 seed = 0x521641013CADULL;
        for (u32 i = 0; i < 256; ++i) {
            const u64 a = Random(seed), b = Random(seed);
            Storage actual, expected;
            for (auto* storage : {&actual, &expected}) {
                storage->Put(0, a); storage->Put(8, b); storage->state->host_cpu_flags = 0xD0001234;
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == reference.Run(), "CPUID pair SysV helper halt");
            const u64 independent_first = (a * 0x1020304050607081ULL) ^ options;
            const u64 sum = b + 0xFEDCBA9876543210ULL;
            const u64 independent_second = ((sum << 19) | (sum >> 45)) ^ a;
            Check(actual.Get(16) == independent_first && actual.Get(24) == independent_second,
                  "CPUID helper parameters and independent two-register return");
            Check(actual.Get(16) == expected.Get(16) && actual.Get(24) == expected.Get(24), "CPUID interpreter comparison");
            Check(actual.state->host_cpu_flags == 0xD0001234, "CPUID pair preserves guest flags");
            if (delayed) Check(actual.Get(40) == a + 39, "CPUID delayed upper survives recycled stack homes");
        }
    }
    std::cout << "PASS CPUID helper pair ABI and delayed upper result\n";
}

std::array<u64, 2> IndependentWideDivision(u64 high, u64 low, u64 divisor, bool sign) {
    if (!divisor || (sign && divisor == UINT64_MAX && high == (u64{1} << 63) && low == 0)) return {};
    unsigned __int128 numerator = (static_cast<unsigned __int128>(high) << 64) | low;
    const bool negative_numerator = sign && (high >> 63);
    const bool negative_divisor = sign && (divisor >> 63);
    if (negative_numerator) numerator = ~numerator + 1;
    const u64 denominator = negative_divisor ? u64{0} - divisor : divisor;
    u64 quotient = static_cast<u64>(numerator / denominator);
    u64 remainder = static_cast<u64>(numerator % denominator);
    if (negative_numerator != negative_divisor) quotient = u64{0} - quotient;
    if (negative_numerator) remainder = u64{0} - remainder;
    return {quotient, remainder};
}

void WideDivisionNativeAndFallback() {
    for (bool sign : {false, true}) for (bool delayed : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5900})}; ir::Assembler as{block.get()};
        auto high = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto low = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        auto divisor = as.LoadUniform(ir::Uniform{16, ValueType::U64}).SetType(ValueType::U64);
        auto quotient = as.Div128(high, low, divisor, ir::Imm{sign}).SetType(ValueType::U64);
        if (delayed) {
            for (u32 i = 0; i < 40; ++i) {
                auto scratch = as.Add<ir::U64>(low, ir::Operand{ir::Imm{u64{i}}}).SetType(ValueType::U64);
                as.StoreUniform(ir::Uniform{48, ValueType::U64}, scratch);
            }
        }
        auto remainder = as.Div128Remainder(quotient).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{24, ValueType::U64}, quotient);
        as.StoreUniform(ir::Uniform{32, ValueType::U64}, remainder);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        std::vector<std::array<u64, 3>> inputs;
        const std::array<u64, 7> edges{0, 1, 2, (u64{1} << 63) - 1, u64{1} << 63, UINT64_MAX - 1, UINT64_MAX};
        for (u64 h : edges) for (u64 l : edges) for (u64 d : edges) inputs.push_back({h, l, d});
        u64 seed = 0x784136BA7851ULL;
        for (u32 i = 0; i < 1024; ++i) {
            const u64 l = Random(seed), d = Random(seed);
            const u64 h = i % 3 == 0 ? u64{0} - (l >> 63) : i % 3 == 1 && d ? Random(seed) % d : Random(seed);
            inputs.push_back({h, l, d});
        }
        for (const auto& input : inputs) {
            Storage actual, expected;
            for (u32 i = 0; i < 3; ++i) { actual.Put(i * 8, input[i]); expected.Put(i * 8, input[i]); }
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = 0xD0012345;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == reference.Run(), "wide division halt and trap guards");
            const auto independent = IndependentWideDivision(input[0], input[1], input[2], sign);
            Check(actual.Get(24) == independent[0] && actual.Get(32) == independent[1],
                  "independent wide quotient and remainder sign=" + std::to_string(sign) +
                  " delayed=" + std::to_string(delayed) + " high=" + std::to_string(input[0]) +
                  " low=" + std::to_string(input[1]) + " divisor=" + std::to_string(input[2]) +
                  " actual=" + std::to_string(actual.Get(24)) + "/" + std::to_string(actual.Get(32)) +
                  " expected=" + std::to_string(independent[0]) + "/" + std::to_string(independent[1]));
            Check(actual.Get(24) == expected.Get(24) && actual.Get(32) == expected.Get(32), "wide division interpreter equality");
            Check(actual.state->host_cpu_flags == expected.state->host_cpu_flags, "wide division preserves guest flags");
            if (delayed) Check(actual.Get(48) == input[1] + 39, "delayed remainder survives recycled homes");
        }
    }
    std::cout << "PASS guarded native 128-bit division, truncating fallback and delayed remainder\n";
}
