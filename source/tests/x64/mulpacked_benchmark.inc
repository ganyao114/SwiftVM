// Portable scalar IR expansion, not a replica of the previous direct emitter.
using MulPackedBenchmarkVector = std::array<u64, 2>;
MulPackedBenchmarkVector MulPackedBenchmarkIndependent(MulPackedBenchmarkVector a, MulPackedBenchmarkVector b, u32 bits) {
    MulPackedBenchmarkVector result{}; const u64 mask = (u64{1} << bits) - 1;
    for (u32 lane = 0; lane < 128 / bits; ++lane) {
        const u32 half = lane * bits / 64, shift = lane * bits % 64;
        const u64 product = ((a[half] >> shift) & mask) * ((b[half] >> shift) & mask);
        result[half] |= (product & mask) << shift;
    }
    return result;
}
ir::Value MulPackedBenchmarkExpanded(ir::Assembler& as, ir::Value a, ir::Value b, u32 bits) {
    using ir::ValueType;
    const auto imm = [&](u64 value) { return as.LoadImm(ir::Imm{value}).SetType(ValueType::U64); };
    const u64 mask = (u64{1} << bits) - 1;
    std::array<ir::Value, 2> output{imm(0), imm(0)};
    for (u32 half = 0; half < 2; ++half) {
        auto left = as.VecExtract64(a, ir::Imm{half}).SetType(ValueType::U64);
        auto right = as.VecExtract64(b, ir::Imm{half}).SetType(ValueType::U64);
        for (u32 shift = 0; shift < 64; shift += bits) {
            auto x = as.LsrImm(left, ir::Imm{shift}).SetType(ValueType::U64);
            auto y = as.LsrImm(right, ir::Imm{shift}).SetType(ValueType::U64);
            x = as.And(x, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
            y = as.And(y, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
            auto product = as.Mul(x, ir::Operand{y}).SetType(ValueType::U64);
            product = as.And(product, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
            product = as.LslImm(product, ir::Imm{shift}).SetType(ValueType::U64);
            output[half] = as.Or(output[half], ir::Operand{product}).SetType(ValueType::U64);
        }
    }
    auto low = as.VecDup64(output[0]).SetType(ValueType::V128);
    auto high = as.VecDup64(output[1]).SetType(ValueType::V128);
    return as.VecZip(low, high, ir::Imm{u32{64}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
}
IntrusivePtr<ir::Block> MulPackedBenchmarkProgram(bool packed, u32 bits, bool four) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8000})}; ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto pattern = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        value = as.VecXor(value, pattern).SetType(ValueType::V128);
        if (!packed) value = MulPackedBenchmarkExpanded(as, value, pattern, bits);
        else if (four) value = as.Vec4Mul(value, pattern).SetType(ValueType::V128);
        else value = as.VecMul(value, pattern, ir::Imm{bits}).SetType(ValueType::V128);
    }
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}
void MeasureMulPacked(u32 iterations) {
    std::cout << "mulpacked,route,ir_instructions,code_bytes,frame_bytes,compile_us,native_ns,speedup\n";
    for (u32 shape = 0; shape < 3; ++shape) {
        const u32 bits = shape == 0 ? 8 : 32; const bool four = shape == 2;
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        MulPackedBenchmarkVector expanded_result{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = MulPackedBenchmarkProgram(route != 0, bits, four);
            std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now(); backend::x64::Translator candidate{block.get(), backend::x64::HostFeatures{}}; candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get(), backend::x64::HostFeatures{}}; translated.Translate();
            const auto native = translated.getCode<HaltReason (*)(backend::State*)>();
            Storage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = 0xdabcdef012345678ULL;
            MulPackedBenchmarkVector independent{}, pattern{};
            std::memcpy(independent.data(), actual.state->uniform_buffer_begin, 16);
            std::memcpy(pattern.data(), actual.state->uniform_buffer_begin + 16, 16);
            for (u32 iteration = 0; iteration < 64; ++iteration) {
                independent[0] ^= pattern[0]; independent[1] ^= pattern[1];
                independent = MulPackedBenchmarkIndependent(independent, pattern, bits);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) ||
                actual.state->host_cpu_flags != expected.state->host_cpu_flags || actual.state->host_cpu_flags != 0xdabcdef012345678ULL)
                throw std::runtime_error("mulpacked benchmark native/interpreter snapshot or flags mismatch");
            if (std::memcmp(actual.state->uniform_buffer_begin + 64, independent.data(), 16))
                throw std::runtime_error("mulpacked benchmark independent modulo product mismatch");
            if (!route) std::memcpy(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 16);
            else if (std::memcmp(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 16))
                throw std::runtime_error("mulpacked benchmark scalar/packed mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples); code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << (four ? "vec4mul32" : bits == 8 ? "mul8" : "mul32") << ',' << (route ? "packed_sse2" : "scalar_ir_expansion") << ','
                      << instructions[route] << ',' << code_bytes[route] << ',' << frame_bytes[route] << ',' << std::fixed << std::setprecision(2)
                      << compile_us[route] << ',' << execution_ns[route] << ',' << execution_ns[0] / execution_ns[route] << '\n';
    }
}
