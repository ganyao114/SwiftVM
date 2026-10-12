using Compare64Vector = std::array<u64, 2>;
u64 Compare64IndependentLane(u64 a, u64 b, bool greater) {
    // Order the mathematical signed values without converting out-of-range
    // unsigned values to signed integers or reproducing the packed emitter.
    const bool sign_a = a >> 63, sign_b = b >> 63;
    const bool match = greater ? (sign_a != sign_b ? sign_b : a > b) : a == b;
    return match ? UINT64_MAX : 0;
}
Compare64Vector Compare64Independent(Compare64Vector a, Compare64Vector b, bool greater) {
    return {Compare64IndependentLane(a[0], b[0], greater), Compare64IndependentLane(a[1], b[1], greater)};
}
ir::Value Compare64Emit(ir::Assembler& as, ir::Value a, ir::Value b, bool greater) {
    return greater ? as.VecCmpGt(a, b, ir::Imm{u32{64}}).SetType(ValueType::V128) :
                     as.VecCmpEq(a, b, ir::Imm{u32{64}}).SetType(ValueType::V128);
}

void Compare64BoundariesAndAliases() {
    constexpr std::array<u64, 20> boundaries{0, 1, UINT64_MAX, 0x8000000000000000ULL,
        0x7fffffffffffffffULL, 0x8000000000000001ULL, 0xffffffff00000000ULL,
        0xffffffff7fffffffULL, 0xffffffff80000000ULL, 0xffffffffffffffffULL,
        0x000000007fffffffULL, 0x0000000080000000ULL, 0x00000000ffffffffULL,
        0x0000000100000000ULL, 0x000000017fffffffULL, 0x0000000180000000ULL,
        0x00000001ffffffffULL, 0x7fffffff00000000ULL, 0x80000000ffffffffULL,
        0x800000007fffffffULL};
    u64 random = 0xc30967d435821abeULL;
    for (bool greater : {false, true}) for (u32 layout = 0; layout < 4; ++layout) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12000})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto value = Compare64Emit(as, a, layout == 3 ? a : b, greater);
        const u32 destination = layout < 2 ? layout * 16 : 64;
        as.StoreUniform(ir::Uniform{destination, ValueType::V128}, value);
        // Retain the original SSA values after a store that may overwrite their
        // uniforms. This also exercises the non-forwarded producer path.
        as.StoreUniform(ir::Uniform{96, ValueType::V128}, a);
        as.StoreUniform(ir::Uniform{112, ValueType::V128}, b);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < boundaries.size() * boundaries.size() + 256; ++sample) {
                Compare64Vector a, b;
                if (sample < boundaries.size() * boundaries.size()) {
                    const u64 left = boundaries[sample / boundaries.size()], right = boundaries[sample % boundaries.size()];
                    a = {left, right}; b = {right, left};
                } else {
                    a = {Random(random), Random(random)}; b = {Random(random), Random(random)};
                    if (sample % 4 == 0) b[0] = a[0];
                    if (sample % 4 == 1) b[0] = (a[0] & 0xffffffff00000000ULL) | (b[0] & 0xffffffffULL);
                    if (sample % 4 == 2) b[1] = (a[1] & 0xffffffffULL) | (b[1] & 0xffffffff00000000ULL);
                }
                const auto wanted = Compare64Independent(a, layout == 3 ? a : b, greater);
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    std::memcpy(storage->state->uniform_buffer_begin, a.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 16, b.data(), 16);
                }
                const u64 flags = Random(random); actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                const std::string label = "compare64 op=" + std::to_string(greater) + "/layout=" + std::to_string(layout) + "/sample=" + std::to_string(sample);
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, label + " halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + destination, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + destination, wanted.data(), 16), label + " independent lane masks");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + 96, a.data(), 16) &&
                      !std::memcmp(actual.state->uniform_buffer_begin + 112, b.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 96, a.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 112, b.data(), 16), label + " retained SSA originals");
                Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr,
                      label + " guest flags and host FP environment");
            }
        }
    }
    std::cout << "PASS compare64 signed order/equality boundaries, random halves, aliases, retained SSA and flags\n";
}

void Compare64ForwardedChains() {
    u64 random = 0x815970bfc30aed24ULL;
    for (bool greater : {false, true}) for (u32 chain = 0; chain < 4; ++chain) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12100})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        ir::Value value;
        if (chain == 0 || chain == 2) {
            auto mixed = as.VecXor(a, b).SetType(ValueType::V128);
            value = Compare64Emit(as, mixed, chain == 2 ? mixed : b, greater);
        } else {
            value = Compare64Emit(as, a, b, greater);
            if (chain == 1) value = Compare64Emit(as, value, b, greater);
        }
        if (chain != 1) value = as.VecXor(value, b).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 256; ++sample) {
                Compare64Vector a{Random(random), Random(random)}, b{Random(random), Random(random)};
                if (sample % 4 == 0) b[0] = a[0];
                if (sample % 4 == 1) b[1] = 0x8000000000000000ULL;
                Compare64Vector wanted;
                if (chain == 0 || chain == 2) {
                    const Compare64Vector mixed{a[0] ^ b[0], a[1] ^ b[1]};
                    wanted = Compare64Independent(mixed, chain == 2 ? mixed : b, greater);
                } else {
                    wanted = Compare64Independent(a, b, greater);
                    if (chain == 1) wanted = Compare64Independent(wanted, b, greater);
                }
                if (chain != 1) { wanted[0] ^= b[0]; wanted[1] ^= b[1]; }
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    std::memcpy(storage->state->uniform_buffer_begin, a.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 16, b.data(), 16);
                }
                const u64 flags = Random(random); actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                const std::string label = "compare64 forwarding op=" + std::to_string(greater) + "/chain=" + std::to_string(chain);
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, label + " halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + 64, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 64, wanted.data(), 16), label + " independent result");
                Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr,
                      label + " guest flags and host FP environment");
            }
        }
    }
    std::cout << "PASS compare64 XOR/compare/repeated operand XMM2 forwarding chains\n";
}

void CompareWidthWholeBlockValidation() {
    constexpr std::array<u64, 16> invalid_widths{0, 1, 7, 9, 15, 17, 31, 33, 63, 65,
        0x100000008ULL, 0x100000010ULL, 0x100000020ULL, 0x100000040ULL,
        UINT64_MAX, 0x8000000000000040ULL};
    for (bool greater : {false, true}) for (bool skipped : {false, true}) {
        const auto make = [&](u64 width, u32 corruption) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12200})}; ir::Assembler as{block.get()};
            auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
            auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
            ir::Value skip;
            if (skipped) skip = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(ValueType::U8)});
            auto value = greater ? as.VecCmpGt(a, b, ir::Imm{width}).SetType(ValueType::V128) :
                                   as.VecCmpEq(a, b, ir::Imm{width}).SetType(ValueType::V128);
            if (corruption == 1) value.Def()->SetArg(2, a);
            if (corruption == 2) value.Def()->SetArg(2, ir::Uniform{0, ValueType::U32});
            if (skipped) as.BindLabel(skip);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            return block;
        };
        for (u64 width : invalid_widths) {
            auto block = make(width, 0); bool rejected{};
            try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { rejected = true; }
            Check(rejected, "compare rejects entire u64 lane width, including branch-skipped IR");
        }
        for (u32 corruption : {1u, 2u}) {
            auto block = make(64, corruption); bool rejected{};
            try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { rejected = true; }
            Check(rejected, "compare rejects value/uniform operands in immediate width slot before emission");
        }
        for (u64 width : {8u, 16u, 32u, 64u}) {
            auto block = make(width, 0); bool accepted{true};
            try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { accepted = false; }
            Check(accepted, "compare accepts each exact legal lane width in whole-block validation");
        }
    }
    std::cout << "PASS vector compare complete-immediate and branch-skipped width validation\n";
}
