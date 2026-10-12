// Registering translation unit must include <xmmintrin.h>.
using SatBenchmarkUnsignedWide = unsigned __int128;
using SatBenchmarkSignedWide = __int128;
// Baseline is explicit scalar IR expansion; the old
// x64 emitter had no 32/64-bit saturation route to benchmark.
ir::Value SatAddSubBenchmarkExpanded(ir::Assembler& as, ir::Value a, ir::Value b,
                                     u32 bits, bool sign, bool subtract) {
    using ir::ValueType;
    const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    std::array<ir::Value, 2> output;
    for (u32 part = 0; part < 2; ++part) {
        auto left_part = as.VecExtract64(a, ir::Imm{part}).SetType(ValueType::U64);
        auto right_part = as.VecExtract64(b, ir::Imm{part}).SetType(ValueType::U64);
        output[part] = as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64);
        for (u32 offset = 0; offset < 64; offset += bits) {
            auto left = left_part, right = right_part;
            if (offset) {
                left = as.LsrImm(left, ir::Imm{offset}).SetType(ValueType::U64);
                right = as.LsrImm(right, ir::Imm{offset}).SetType(ValueType::U64);
            }
            if (bits < 64) {
                left = as.And(left, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
                right = as.And(right, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
            }
            auto wrapped = (subtract ? as.Sub(left, ir::Operand{right}) : as.Add(left, ir::Operand{right})).SetType(ValueType::U64);
            if (bits < 64) wrapped = as.And(wrapped, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
            auto differing = as.Xor(left, ir::Operand{right}).SetType(ValueType::U64);
            ir::Value overflow, saturated;
            if (sign) {
                auto changed = as.Xor(left, ir::Operand{wrapped}).SetType(ValueType::U64);
                if (!subtract) differing = as.Xor(differing, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
                overflow = as.And(changed, ir::Operand{differing}).SetType(ValueType::U64);
                auto negative = as.TestBit(left, ir::Imm{bits - 1}).SetType(ValueType::U8);
                auto minimum = as.LoadImm(ir::Imm{u64{1} << (bits - 1)}).SetType(ValueType::U64);
                auto maximum = as.LoadImm(ir::Imm{(u64{1} << (bits - 1)) - 1}).SetType(ValueType::U64);
                saturated = as.Select(ir::BOOL{negative}, minimum, maximum).SetType(ValueType::U64);
            } else {
                auto inverted_left = as.Xor(left, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
                auto first = as.And(subtract ? inverted_left : left, ir::Operand{right}).SetType(ValueType::U64);
                ir::Value second;
                if (subtract) {
                    auto same = as.Xor(differing, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
                    second = as.And(same, ir::Operand{wrapped}).SetType(ValueType::U64);
                } else {
                    auto joined = as.Or(left, ir::Operand{right}).SetType(ValueType::U64);
                    auto inverted_result = as.Xor(wrapped, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
                    second = as.And(joined, ir::Operand{inverted_result}).SetType(ValueType::U64);
                }
                overflow = as.Or(first, ir::Operand{second}).SetType(ValueType::U64);
                saturated = as.LoadImm(ir::Imm{subtract ? u64{0} : mask}).SetType(ValueType::U64);
            }
            auto predicate = as.TestBit(overflow, ir::Imm{bits - 1}).SetType(ValueType::U8);
            auto value = as.Select(ir::BOOL{predicate}, saturated, wrapped).SetType(ValueType::U64);
            if (offset) value = as.LslImm(value, ir::Imm{offset}).SetType(ValueType::U64);
            output[part] = as.Or(output[part], ir::Operand{value}).SetType(ValueType::U64);
        }
    }
    auto low = as.VecDup64(output[0]).SetType(ValueType::V128), high = as.VecDup64(output[1]).SetType(ValueType::V128);
    return as.VecZip(low, high, ir::Imm{u32{64}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
}
IntrusivePtr<ir::Block> SatAddSubBenchmarkProgram(bool packed, u32 bits, bool sign, bool subtract) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7400})}; ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto operand = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    for (u32 repeat = 0; repeat < 64; ++repeat) {
        value = as.VecXor(value, operand).SetType(ValueType::V128);
        if (!packed) value = SatAddSubBenchmarkExpanded(as, value, operand, bits, sign, subtract);
        else value = (subtract ? as.VecSatSub(value, operand, ir::Imm{bits}, ir::Imm{u32(sign)}) :
                                 as.VecSatAdd(value, operand, ir::Imm{bits}, ir::Imm{u32(sign)})).SetType(ValueType::V128);
    }
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}
std::array<u64, 2> SatAddSubBenchmarkIndependent(std::array<u64, 2> a, std::array<u64, 2> b,
                                               u32 bits, bool sign, bool subtract) {
    const SatBenchmarkUnsignedWide modulus = SatBenchmarkUnsignedWide{1} << bits, mask = modulus - 1;
    const SatBenchmarkSignedWide minimum = sign ? -SatBenchmarkSignedWide(modulus / 2) : 0;
    const SatBenchmarkSignedWide maximum = sign ? SatBenchmarkSignedWide(modulus / 2 - 1) : SatBenchmarkSignedWide(mask);
    std::array<u64, 2> result{};
    for (u32 part = 0; part < 2; ++part) for (u32 offset = 0; offset < 64; offset += bits) {
        SatBenchmarkSignedWide left = (SatBenchmarkUnsignedWide(a[part]) >> offset) & mask, right = (SatBenchmarkUnsignedWide(b[part]) >> offset) & mask;
        if (sign && left > maximum) left -= SatBenchmarkSignedWide(modulus);
        if (sign && right > maximum) right -= SatBenchmarkSignedWide(modulus);
        SatBenchmarkSignedWide value = subtract ? left - right : left + right;
        if (value < minimum) value = minimum;
        if (value > maximum) value = maximum;
        result[part] |= u64(SatBenchmarkUnsignedWide(value) & mask) << offset;
    }
    return result;
}
void MeasureSatAddSub(u32 iterations) {
    std::cout << "sataddsub,route,ir_instructions,code_bytes,frame_bytes,compile_us,native_ns,speedup\n";
    for (u32 bits : {32u, 64u}) for (bool sign : {false, true}) for (bool subtract : {false, true}) {
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        std::array<u64, 2> baseline{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = SatAddSubBenchmarkProgram(route, bits, sign, subtract); std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now(); backend::x64::Translator candidate{block.get(), backend::x64::HostFeatures{}}; candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get(), backend::x64::HostFeatures{}}; translated.Translate();
            auto native = translated.getCode<HaltReason (*)(backend::State*)>(); Storage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = 0xeabc591389ae7214ULL;
            std::array<u64, 2> wanted{}, operand{};
            std::memcpy(wanted.data(), actual.state->uniform_buffer_begin, 16);
            std::memcpy(operand.data(), actual.state->uniform_buffer_begin + 16, 16);
            for (u32 repeat = 0; repeat < 64; ++repeat) {
                for (u32 part = 0; part < 2; ++part) wanted[part] ^= operand[part];
                wanted = SatAddSubBenchmarkIndependent(wanted, operand, bits, sign, subtract);
            }
            const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) ||
                std::memcmp(actual.state->uniform_buffer_begin + 64, wanted.data(), 16) ||
                actual.state->host_cpu_flags != expected.state->host_cpu_flags || actual.state->host_cpu_flags != 0xeabc591389ae7214ULL ||
                _mm_getcsr() != mxcsr) throw std::runtime_error("sat add/sub independent/interpreter/flags/MXCSR mismatch");
            if (!route) std::memcpy(baseline.data(), actual.state->uniform_buffer_begin + 64, 16);
            else if (std::memcmp(baseline.data(), actual.state->uniform_buffer_begin + 64, 16)) throw std::runtime_error("sat add/sub scalar/packed route mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples); code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << (sign ? 's' : 'u') << (subtract ? "qsub_" : "qadd_") << bits << ','
                      << (route ? "packed_sse2" : "scalar_ir_expansion") << ',' << instructions[route] << ',' << code_bytes[route] << ','
                      << frame_bytes[route] << ',' << std::fixed << std::setprecision(2) << compile_us[route] << ',' << execution_ns[route] << ','
                      << execution_ns[0] / execution_ns[route] << '\n';
    }
}
