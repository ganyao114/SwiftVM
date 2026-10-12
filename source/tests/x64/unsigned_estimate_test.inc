// Exact arithmetic oracle, independent of runtime tables and their binary
// search: reciprocal rational rounding and multiprecision integer sqrt.
u32 UnsignedEstimateIndependent(u32 value, u32 operation) {
    using Big = boost::multiprecision::cpp_int;
    if (!operation) {
        if (value < 0x80000000u) return UINT32_MAX;
        const u64 denominator = 2 * u64(value >> 23) + 1;
        return u32((524288 + denominator) / (2 * denominator)) << 23;
    }
    if (value < 0x40000000u) return UINT32_MAX;
    const bool lower = value < 0x80000000u;
    const u64 denominator = 2 * u64(value >> (lower ? 23 : 24)) + 1;
    const Big numerator = Big{65536} * (lower ? 1024 : 512);
    const Big quotient = numerator / denominator;
    Big estimate = sqrt(quotient);
    if ((2 * estimate + 1) * (2 * estimate + 1) * denominator < 4 * numerator) ++estimate;
    return estimate.convert_to<u32>() << 23;
}
std::array<u32, 4> UnsignedEstimateIndependentVector(std::array<u32, 4> source, u32 operation) {
    for (auto& value : source) value = UnsignedEstimateIndependent(value, operation);
    return source;
}
void UnsignedEstimateBoundariesAndForwarding() {
    u64 random = 0x1830927ceaf23641ULL;
    for (u32 operation = 0; operation < 2; ++operation) for (u32 layout = 0; layout < 5; ++layout) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12900})}; ir::Assembler as{block.get()};
        std::vector<ir::Value> pressure;
        if (layout == 4) for (u32 i = 0; i < 24; ++i)
            pressure.push_back(as.LoadUniform(ir::Uniform{256 + i * 16, ValueType::V128}).SetType(ValueType::V128));
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto pattern = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto source = layout == 2 ? as.VecXor(input, pattern).SetType(ValueType::V128) : input;
        auto result = as.VecUnsignedEstimate(source, ir::Imm{operation}).SetType(ValueType::V128);
        if (layout == 3) result = as.VecUnsignedEstimate(result, ir::Imm{1 - operation}).SetType(ValueType::V128);
        auto final = layout == 2 || layout == 3 ? as.VecXor(result, pattern).SetType(ValueType::V128) : result;
        const u32 destination = layout == 1 ? 0 : 64;
        as.StoreUniform(ir::Uniform{destination, ValueType::V128}, final);
        if (layout < 2 || layout == 4) as.StoreUniform(ir::Uniform{96, ValueType::V128}, input);
        if (layout == 4) {
            as.StoreUniform(ir::Uniform{112, ValueType::V128}, result);
            for (u32 i = 0; i < pressure.size(); ++i) as.StoreUniform(ir::Uniform{1024 + i * 16, ValueType::V128}, pressure[i]);
        }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 1024; ++sample) {
                std::array<u32, 4> input{}, pattern{}, source{};
                for (u32 lane = 0; lane < 4; ++lane) {
                    if (sample < 768) {
                        const u32 index = sample / 3, offset = sample % 3;
                        const u64 boundary = (u64(index) + 256) << 23;
                        const u32 directed = u32(boundary + offset - 1);
                        input[lane] = lane == 0 ? directed : lane == 1 ? directed ^ 0x80000000u :
                                      lane == 2 ? (directed >> 1) : UINT32_MAX - directed;
                    } else input[lane] = u32(Random(random));
                    pattern[lane] = u32(Random(random));
                    source[lane] = layout == 2 ? input[lane] ^ pattern[lane] : input[lane];
                }
                auto wanted = UnsignedEstimateIndependentVector(source, operation);
                if (layout == 3) wanted = UnsignedEstimateIndependentVector(wanted, 1 - operation);
                const auto retained_result = wanted;
                if (layout == 2 || layout == 3) for (u32 lane = 0; lane < 4; ++lane) wanted[lane] ^= pattern[lane];
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    for (u32 byte = 0; byte < 2048; ++byte) storage->state->uniform_buffer_begin[byte] = u8(byte * 17 + sample);
                    std::memcpy(storage->state->uniform_buffer_begin, input.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 16, pattern.data(), 16);
                }
                const u64 flags = Random(random); actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "unsigned estimate halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + destination, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + destination, wanted.data(), 16), "unsigned estimate independent rational/root lanes");
                Check(!std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 2048) &&
                      ((layout == 2 || layout == 3) || !std::memcmp(actual.state->uniform_buffer_begin + 96, input.data(), 16)) &&
                      (layout != 4 || !std::memcmp(actual.state->uniform_buffer_begin + 112, retained_result.data(), 16)), "unsigned estimate aliases, retained SSA and pressure homes");
                Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr, "unsigned estimate preserves guest flags and MXCSR");
            }
        }
    }
    std::cout << "PASS shared unsigned estimate all quantization intervals, boundaries, aliases, forwarding and flags\n";
}
void UnsignedEstimateMalformedIR() {
    for (bool skipped : {false, true}) for (u32 corruption = 0; corruption < 15; ++corruption) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12a00})}; ir::Assembler as{block.get()};
        auto source = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        ir::Value skip;
        if (skipped) skip = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(ValueType::U8)});
        u64 operation = corruption == 0 ? 2 : corruption == 1 ? UINT64_MAX : corruption == 2 ? 0x100000000ULL : corruption == 3 ? 0x100000001ULL : 0;
        if (corruption == 4) source = source.SetCastType(ValueType::V64);
        if (corruption == 5) source = source.SetType(ValueType::V64);
        if (corruption == 6) source = source.SetCastType(ValueType::U64);
        if (corruption == 7) source = source.SetType(ValueType::VOID);
        if (corruption == 8) source = source.SetCastType(ValueType::V256);
        if (corruption == 9) source = source.SetType(ValueType::V256);
        auto result = as.VecUnsignedEstimate(source, ir::Imm{operation}).SetType(ValueType::V128);
        if (corruption == 10) result = result.SetType(ValueType::V64);
        if (corruption == 11) result = result.SetType(ValueType::VOID);
        if (corruption == 12) result.Def()->SetArg(0, ir::Imm{u64{0}});
        if (corruption == 13) result.Def()->SetArg(1, source);
        if (corruption == 14) {
            // SetArg rejects undefined SSA at construction. Inject the
            // malformed slot after unregistering its valid operand instead.
            result.Def()->SetArg(0, ir::Void{nullptr});
            result.Def()->ArgAt(0) = ir::Value{};
        }
        if (skipped) as.BindLabel(skip);
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool native_rejected{}, interp_rejected{};
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { native_rejected = true; }
        Storage state;
        try { backend::interp::Interpreter reference{*state.state, block.get()}; interp_rejected = reference.Run() == HaltReason::IllegalCode; }
        catch (const std::runtime_error&) { interp_rejected = true; }
        if (corruption == 14) result.Def()->ArgAt(0) = ir::Void{nullptr};
        Check(native_rejected && interp_rejected, "unsigned estimate strict full-operation/canonical types including skipped malformed IR");
    }
    std::cout << "PASS shared unsigned estimate whole-block malformed IR validation\n";
}
