using DotBytes = std::array<u8, 16>;
using DotWords = std::array<u32, 4>;

DotWords DotIndependent(const DotBytes& lhs, const DotBytes& rhs, const DotWords& accumulator, bool sign) {
    DotWords result = accumulator;
    for (u32 lane = 0; lane < 4; ++lane) {
        s64 sum{};
        for (u32 term = 0; term < 4; ++term) {
            const u32 index = lane * 4 + term;
            const s32 left = sign && lhs[index] >= 128 ? s32(lhs[index]) - 256 : s32(lhs[index]);
            const s32 right = sign && rhs[index] >= 128 ? s32(rhs[index]) - 256 : s32(rhs[index]);
            sum += s64(left) * right;
        }
        // Reduce after the complete mathematical dot; no saturating partials.
        result[lane] = u32(u64(accumulator[lane]) + u64(sum));
    }
    return result;
}

IntrusivePtr<ir::Block> DotProgram(u64 sign) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe800})}; ir::Assembler as{block.get()};
    auto lhs = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto rhs = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto accumulator = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
    auto result = as.VecDot(lhs, rhs, accumulator, ir::Imm{sign}).SetType(ValueType::V128);
    as.StoreUniform(ir::Uniform{48, ValueType::V128}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}

void DotBytePairsNativeAndBaseline() {
    for (bool baseline : {false, true}) for (bool sign : {false, true}) {
        auto block = DotProgram(sign);
        Compiled compiled{block.get(), baseline ? x64::HostFeatures{} : x64::HostFeatures::Detect()};
        Storage actual; std::array<u8, 4096> expected{};
        for (u32 byte = 0; byte < expected.size(); ++byte) expected[byte] = u8(byte * 37 + 19);
        std::memcpy(actual.state->uniform_buffer_begin, expected.data(), expected.size());
        const auto original_mxcsr = _mm_getcsr();
        const auto changed_mxcsr = (original_mxcsr & ~0x6000u) | 0x6000u | 0x3fu;
        _mm_setcsr(changed_mxcsr);
        for (u32 left = 0; left < 256; ++left) for (u32 right = 0; right < 256; ++right) {
            DotBytes lhs{}, rhs{};
            for (u32 term = 0; term < 4; ++term) {
                lhs[term] = u8(left); rhs[term] = u8(right);
                lhs[4 + term] = u8(left ^ 0x80); rhs[4 + term] = u8(right);
                lhs[8 + term] = u8(left + term * 43); rhs[8 + term] = u8(right + term * 71);
                lhs[12 + term] = u8(left + 17 + term * 29); rhs[12 + term] = u8(right + 29 + term * 13);
            }
            const DotWords accumulator{0, UINT32_MAX, 0x7fffffff, 0x80000000};
            const auto result = DotIndependent(lhs, rhs, accumulator, sign);
            std::memcpy(expected.data(), lhs.data(), 16); std::memcpy(expected.data() + 16, rhs.data(), 16);
            std::memcpy(expected.data() + 32, accumulator.data(), 16); std::memcpy(expected.data() + 48, result.data(), 16);
            std::memcpy(actual.state->uniform_buffer_begin, expected.data(), 48);
            actual.state->host_cpu_flags = 0xabcdfedc12345678ULL;
            const auto reason = compiled.fn(actual.state);
            const auto after_mxcsr = _mm_getcsr();
            Check(reason == HaltReason::CallHost, "VecDot exhaustive byte pairs halt");
            Check(std::memcmp(actual.state->uniform_buffer_begin, expected.data(), expected.size()) == 0,
                  "VecDot exhaustive byte pairs/math/canonical FP-status preservation: " +
                  std::to_string(sign) + "/" + std::to_string(left) + "/" + std::to_string(right));
            Check(actual.state->host_cpu_flags == 0xabcdfedc12345678ULL, "VecDot preserves all guest flags");
            Check(after_mxcsr == changed_mxcsr, "VecDot preserves host FP rounding and exception status");
        }
        _mm_setcsr(original_mxcsr);
    }
    std::cout << "PASS VecDot all 256x256 byte pairs / signed+unsigned / detected+SSE2 baseline\n";
}

void DotWrapAndAliases() {
    const std::array<u32, 6> edges{0, 1, 0x7fffffff, 0x80000000, 0xffff0000, UINT32_MAX};
    for (bool baseline : {false, true}) for (bool sign : {false, true}) {
        auto block = DotProgram(sign); Compiled compiled{block.get(), baseline ? x64::HostFeatures{} : x64::HostFeatures::Detect()};
        for (u32 pattern = 0; pattern < 5; ++pattern) for (u32 edge = 0; edge < edges.size(); ++edge) {
            DotBytes lhs{}, rhs{};
            for (u32 byte = 0; byte < 16; ++byte) {
                lhs[byte] = pattern == 0 ? 255 : pattern == 1 ? 128 : pattern == 2 ? 127 :
                            pattern == 3 ? u8((byte & 1) ? 128 : 127) : u8(byte * 23 + 9);
                rhs[byte] = pattern == 0 ? 255 : pattern == 1 ? 128 : pattern == 2 ? 128 :
                            pattern == 3 ? u8((byte & 1) ? 127 : 128) : u8(byte * 37 + 11);
            }
            const DotWords accumulator{edges[edge], edges[(edge + 1) % edges.size()],
                                       edges[(edge + 2) % edges.size()], edges[(edge + 3) % edges.size()]};
            const auto expected = DotIndependent(lhs, rhs, accumulator, sign);
            Storage actual; std::memcpy(actual.state->uniform_buffer_begin, lhs.data(), 16);
            std::memcpy(actual.state->uniform_buffer_begin + 16, rhs.data(), 16);
            std::memcpy(actual.state->uniform_buffer_begin + 32, accumulator.data(), 16);
            Check(compiled.fn(actual.state) == HaltReason::CallHost, "VecDot saturation and U32 wrap halt");
            Check(std::memcmp(actual.state->uniform_buffer_begin + 48, expected.data(), 16) == 0,
                  "VecDot exact byte products, signed negatives and U32 accumulator wrap");
        }
    }

    u64 seed = 0x8d723741bcdef901ULL;
    for (bool baseline : {false, true}) for (bool sign : {false, true}) for (u32 alias = 0; alias < 5; ++alias) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe900})}; ir::Assembler as{block.get()};
        auto lhs = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto rhs = alias == 1 || alias == 4 ? lhs : as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto accumulator = alias == 2 || alias == 4 ? lhs : alias == 3 ? rhs :
                           as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
        auto result = as.VecDot(lhs, rhs, accumulator, ir::Imm{u32(sign)}).SetType(ValueType::V128);
        auto value = result;
        for (u32 step = 0; step < 32; ++step) {
            value = as.VecXor(value, lhs).SetType(ValueType::V128);
            value = as.VecAdd(value, rhs, ir::Imm{u32{32}}).SetType(ValueType::V128);
        }
        as.StoreUniform(ir::Uniform{48, ValueType::V128}, result);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
        as.StoreUniform(ir::Uniform{80, ValueType::V128}, lhs);
        as.StoreUniform(ir::Uniform{96, ValueType::V128}, rhs);
        as.StoreUniform(ir::Uniform{112, ValueType::V128}, accumulator);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get(), baseline ? x64::HostFeatures{} : x64::HostFeatures::Detect()};
        Check(compiled.translator.FrameSize() <= 512, "VecDot delayed inputs/result retain homes while temporary chain recycles");
        for (u32 iteration = 0; iteration < 64; ++iteration) {
            Storage actual; std::array<u8, 4096> expected{};
            for (u32 byte = 0; byte < expected.size(); byte += 8) {
                const u64 bits = Random(seed); std::memcpy(expected.data() + byte, &bits, 8);
            }
            std::memcpy(actual.state->uniform_buffer_begin, expected.data(), expected.size());
            DotBytes lhs_bytes{}, rhs_bytes{}; DotWords accumulator_words{}, lhs_words{}, rhs_words{};
            std::memcpy(lhs_bytes.data(), expected.data(), 16);
            std::memcpy(rhs_bytes.data(), expected.data() + (alias == 1 || alias == 4 ? 0 : 16), 16);
            const u32 acc_offset = alias == 2 || alias == 4 ? 0 : alias == 3 ? 16 : 32;
            std::memcpy(accumulator_words.data(), expected.data() + acc_offset, 16);
            std::memcpy(lhs_words.data(), lhs_bytes.data(), 16); std::memcpy(rhs_words.data(), rhs_bytes.data(), 16);
            const auto result_words = DotIndependent(lhs_bytes, rhs_bytes, accumulator_words, sign);
            auto chain = result_words;
            for (u32 step = 0; step < 32; ++step)
                for (u32 lane = 0; lane < 4; ++lane) chain[lane] = (chain[lane] ^ lhs_words[lane]) + rhs_words[lane];
            std::memcpy(expected.data() + 48, result_words.data(), 16); std::memcpy(expected.data() + 64, chain.data(), 16);
            std::memcpy(expected.data() + 80, lhs_bytes.data(), 16); std::memcpy(expected.data() + 96, rhs_bytes.data(), 16);
            std::memcpy(expected.data() + 112, accumulator_words.data(), 16);
            actual.state->host_cpu_flags = Random(seed);
            const auto flags = actual.state->host_cpu_flags;
            Check(compiled.fn(actual.state) == HaltReason::CallHost, "VecDot aliased SSA and delayed consumers halt");
            Check(std::memcmp(actual.state->uniform_buffer_begin, expected.data(), expected.size()) == 0,
                  "VecDot all input aliases/delayed SSA homes/forwarded consumer chain");
            Check(actual.state->host_cpu_flags == flags, "VecDot aliases preserve guest flags");
        }
    }
    std::cout << "PASS VecDot directed wrap, aliases and delayed live homes\n";
}

void DotInvalidIR() {
    const auto reject = [](u64 selector, s32 narrow_index, bool narrow_return, bool cast_narrow) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xea00})}; ir::Assembler as{block.get()};
        std::array<ir::Value, 3> input{};
        for (u32 index = 0; index < input.size(); ++index) {
            const auto type = s32(index) == narrow_index ? ValueType::V64 : ValueType::V128;
            input[index] = as.LoadUniform(ir::Uniform{index * 16, type}).SetType(type);
            if (cast_narrow && s32(index) == narrow_index) input[index] = input[index].SetCastType(ValueType::V128);
        }
        auto result = as.VecDot(input[0], input[1], input[2], ir::Imm{selector}).SetType(narrow_return ? ValueType::V64 : ValueType::V128);
        as.StoreUniform(ir::Uniform{48, result.Type()}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        bool rejected{};
        try { Compiled compiled{block.get(), x64::HostFeatures{}}; }
        catch (const std::runtime_error& error) { rejected = std::string{error.what()}.find("VecDot") != std::string::npos; }
        Check(rejected, "VecDot invalid selector/type rejected before publication");
    };
    for (u64 sign : {u64{2}, u64{256}, u64{1} << 32, UINT64_MAX}) reject(sign, -1, false, false);
    reject(0, -1, true, false);
    for (u32 index = 0; index < 3; ++index) {
        reject(0, index, false, false); reject(1, index, false, true);
    }
    std::cout << "PASS VecDot strict V128/full immediate validation\n";
}
