// Baseline reproduces the prior two CallHostFPFree scalar-pair kernels per
// full vector. It is a specific prior lowering, not a global optimal bound.
u64 UnsignedEstimateBenchmarkPair(u64 pair, u64 operation) {
    return u64(EvaluateUnsignedEstimateLane(u32(pair), u32(operation))) |
           (u64(EvaluateUnsignedEstimateLane(u32(pair >> 32), u32(operation))) << 32);
}
u32 UnsignedEstimateBenchmarkIndependent(u32 value, u32 operation) {
    if (!operation) {
        if (value < 0x80000000u) return UINT32_MAX;
        const u64 denominator = 2 * u64(value >> 23) + 1;
        return u32((524288 + denominator) / (2 * denominator)) << 23;
    }
    if (value < 0x40000000u) return UINT32_MAX;
    const bool lower = value < 0x80000000u;
    const u64 numerator = u64{65536} * (lower ? 1024 : 512), denominator = 2 * u64(value >> (lower ? 23 : 24)) + 1;
    // Enumerate exact half-integer thresholds, independently of the runtime
    // table generator's binary search and without floating-point arithmetic.
    u32 estimate = 256;
    while (u64(2 * estimate + 1) * (2 * estimate + 1) * denominator < 4 * numerator) ++estimate;
    return estimate << 23;
}
IntrusivePtr<ir::Block> UnsignedEstimateBenchmarkProgram(bool shared, u32 operation) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x9000})}; ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto pattern = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto op = as.LoadImm(ir::Imm{u64(operation)}).SetType(ValueType::U64);
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        value = as.VecXor(value, pattern).SetType(ValueType::V128);
        if (shared) value = as.VecUnsignedEstimate(value, ir::Imm{operation}).SetType(ValueType::V128);
        else {
            auto low = as.VecExtract64(value, ir::Imm{u32{0}}).SetType(ValueType::U64);
            auto high = as.VecExtract64(value, ir::Imm{u32{1}}).SetType(ValueType::U64);
            low = as.CallHostFPFree(&UnsignedEstimateBenchmarkPair, low, op).SetType(ValueType::U64);
            high = as.CallHostFPFree(&UnsignedEstimateBenchmarkPair, high, op).SetType(ValueType::U64);
            auto lo_vector = as.VecDup64(low).SetType(ValueType::V128), hi_vector = as.VecDup64(high).SetType(ValueType::V128);
            value = as.VecZip(lo_vector, hi_vector, ir::Imm{u32{64}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
        }
    }
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}
void MeasureUnsignedEstimate(u32 iterations) {
    std::cout << "unsigned_estimate,route,ir_instructions,code_bytes,frame_bytes,compile_us,native_ns,speedup\n";
    for (u32 operation = 0; operation < 2; ++operation) {
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        std::array<u32, 4> baseline_result{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = UnsignedEstimateBenchmarkProgram(route != 0, operation); std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now(); backend::x64::Translator candidate{block.get(), backend::x64::HostFeatures{}}; candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get(), backend::x64::HostFeatures{}}; translated.Translate();
            const auto native = translated.getCode<HaltReason (*)(backend::State*)>(); Storage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = 0xdabcdef012345678ULL;
            std::array<u32, 4> independent{}, pattern{};
            std::memcpy(independent.data(), actual.state->uniform_buffer_begin, 16); std::memcpy(pattern.data(), actual.state->uniform_buffer_begin + 16, 16);
            for (u32 iteration = 0; iteration < 64; ++iteration) for (u32 lane = 0; lane < 4; ++lane)
                independent[lane] = UnsignedEstimateBenchmarkIndependent(independent[lane] ^ pattern[lane], operation);
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) ||
                actual.state->host_cpu_flags != expected.state->host_cpu_flags || actual.state->host_cpu_flags != 0xdabcdef012345678ULL ||
                std::memcmp(actual.state->uniform_buffer_begin + 64, independent.data(), 16))
                throw std::runtime_error("unsigned estimate benchmark independent result/interpreter snapshot/flags mismatch");
            if (!route) std::memcpy(baseline_result.data(), actual.state->uniform_buffer_begin + 64, 16);
            else if (std::memcmp(baseline_result.data(), actual.state->uniform_buffer_begin + 64, 16))
                throw std::runtime_error("unsigned estimate benchmark prior pair helper/shared mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples); code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << (operation ? "ursqrte" : "urecpe") << ',' << (route ? "shared_inline_lookup" : "prior_two_fp_free_pair_calls") << ','
                      << instructions[route] << ',' << code_bytes[route] << ',' << frame_bytes[route] << ',' << std::fixed << std::setprecision(2)
                      << compile_us[route] << ',' << execution_ns[route] << ',' << execution_ns[0] / execution_ns[route] << '\n';
    }
}
