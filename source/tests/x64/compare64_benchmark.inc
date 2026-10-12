// Scalar IR expansion is a portable reference baseline, not a timing copy of
// the previous direct scalar emitter. Both routes use baseline SSE2 features.
using Compare64BenchmarkVector = std::array<u64, 2>;
Compare64BenchmarkVector Compare64BenchmarkIndependent(Compare64BenchmarkVector a, Compare64BenchmarkVector b, bool greater) {
    Compare64BenchmarkVector result{};
    for (u32 lane = 0; lane < 2; ++lane) {
        const bool sign_a = a[lane] >> 63, sign_b = b[lane] >> 63;
        const bool match = greater ? (sign_a != sign_b ? sign_b : a[lane] > b[lane]) : a[lane] == b[lane];
        result[lane] = match ? UINT64_MAX : 0;
    }
    return result;
}
ir::Value Compare64BenchmarkExpanded(ir::Assembler& as, ir::Value a, ir::Value b, bool greater) {
    using ir::ValueType;
    std::array<ir::Value, 2> output;
    for (u32 lane = 0; lane < 2; ++lane) {
        auto left = as.VecExtract64(a, ir::Imm{lane}).SetType(ValueType::U64);
        auto right = as.VecExtract64(b, ir::Imm{lane}).SetType(ValueType::U64);
        ir::Value predicate;
        if (greater) {
            auto sign_a = as.TestBit(left, ir::Imm{u32{63}}).SetType(ValueType::U8);
            auto sign_b = as.TestBit(right, ir::Imm{u32{63}}).SetType(ValueType::U8);
            auto different_sign = as.Xor(sign_a, ir::Operand{sign_b}).SetType(ValueType::U8);
            auto difference = as.Sub(left, ir::Operand{right}).SetType(ValueType::U64);
            auto nonzero = as.TestNotZero(difference).SetType(ValueType::U8);
            auto negative = as.TestBit(difference, ir::Imm{u32{63}}).SetType(ValueType::U8);
            auto nonnegative = as.Xor(negative, ir::Operand{ir::Imm{u8{1}}}).SetType(ValueType::U8);
            // Equal-sign subtraction cannot overflow a signed 64-bit integer.
            auto same_sign_greater = as.And(nonzero, ir::Operand{nonnegative}).SetType(ValueType::U8);
            predicate = as.Select(ir::BOOL{different_sign}, sign_b, same_sign_greater).SetType(ValueType::U8);
        } else {
            auto difference = as.Xor(left, ir::Operand{right}).SetType(ValueType::U64);
            predicate = as.TestZero(difference).SetType(ValueType::U8);
        }
        auto wide = as.ZeroExtend64(predicate).SetType(ValueType::U64);
        output[lane] = as.Neg(wide).SetType(ValueType::U64);
    }
    auto low = as.VecDup64(output[0]).SetType(ValueType::V128);
    auto high = as.VecDup64(output[1]).SetType(ValueType::V128);
    return as.VecZip(low, high, ir::Imm{u32{64}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
}
IntrusivePtr<ir::Block> Compare64BenchmarkProgram(bool packed, bool greater) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7000})}; ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto pattern = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        value = as.VecXor(value, pattern).SetType(ValueType::V128);
        if (!packed) value = Compare64BenchmarkExpanded(as, value, pattern, greater);
        else if (greater) value = as.VecCmpGt(value, pattern, ir::Imm{u32{64}}).SetType(ValueType::V128);
        else value = as.VecCmpEq(value, pattern, ir::Imm{u32{64}}).SetType(ValueType::V128);
    }
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}
void MeasureCompare64(u32 iterations) {
    std::cout << "compare64,route,ir_instructions,code_bytes,frame_bytes,compile_us,native_ns,speedup\n";
    for (bool greater : {false, true}) {
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        Compare64BenchmarkVector expanded_result{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = Compare64BenchmarkProgram(route != 0, greater);
            std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now(); backend::x64::Translator candidate{block.get(), backend::x64::HostFeatures{}}; candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get(), backend::x64::HostFeatures{}}; translated.Translate();
            const auto native = translated.getCode<HaltReason (*)(backend::State*)>();
            Storage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = 0xdabcdef012345678ULL;
            Compare64BenchmarkVector independent{}, pattern{};
            std::memcpy(independent.data(), actual.state->uniform_buffer_begin, 16);
            std::memcpy(pattern.data(), actual.state->uniform_buffer_begin + 16, 16);
            for (u32 iteration = 0; iteration < 64; ++iteration) {
                independent[0] ^= pattern[0]; independent[1] ^= pattern[1];
                independent = Compare64BenchmarkIndependent(independent, pattern, greater);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) ||
                actual.state->host_cpu_flags != expected.state->host_cpu_flags || actual.state->host_cpu_flags != 0xdabcdef012345678ULL)
                throw std::runtime_error("compare64 benchmark native/interpreter snapshot or flags mismatch");
            if (std::memcmp(actual.state->uniform_buffer_begin + 64, independent.data(), 16))
                throw std::runtime_error("compare64 benchmark independent signed integer result mismatch");
            if (!route) std::memcpy(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 16);
            else if (std::memcmp(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 16))
                throw std::runtime_error("compare64 benchmark scalar/packed mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples); code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << (greater ? "gt64" : "eq64") << ',' << (route ? "packed_sse2" : "scalar_ir_expansion") << ','
                      << instructions[route] << ',' << code_bytes[route] << ',' << frame_bytes[route] << ',' << std::fixed << std::setprecision(2)
                      << compile_us[route] << ',' << execution_ns[route] << ',' << execution_ns[0] / execution_ns[route] << '\n';
    }
}
