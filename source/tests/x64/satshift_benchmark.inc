// Include after Median/Measure inside benchmark.cpp's fixture namespace.
using ShiftBenchmarkVector = std::array<u64, 2>;
struct ShiftBenchmarkResult { ShiftBenchmarkVector value{}; bool qc{}; };

ShiftBenchmarkResult IndependentShiftBenchmark(ShiftBenchmarkVector input, u32 bits, u32 shift, u32 mode, u32 lanes) {
    const bool left = mode < 3, sign = mode != 1 && mode != 4 && mode != 7;
    const bool signed_output = mode == 0 || mode == 3 || mode == 6;
    const u32 source_bits = left ? bits : 2 * bits;
    const u64 source_mask = source_bits == 64 ? UINT64_MAX : (u64{1} << source_bits) - 1;
    const u64 output_mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    using Exact = __int128;
    const Exact lower = signed_output ? -(Exact{1} << (bits - 1)) : 0;
    const Exact upper = signed_output ? (Exact{1} << (bits - 1)) - 1 : (Exact{1} << bits) - 1;
    ShiftBenchmarkResult result;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const u64 raw = (input[lane * source_bits / 64] >> (lane * source_bits % 64)) & source_mask;
        Exact value = raw;
        if (sign && (raw >> (source_bits - 1))) value -= Exact{1} << source_bits;
        if (left) value *= Exact{1} << shift;
        else {
            if (mode >= 6) value += Exact{1} << (shift - 1);
            const Exact divisor = Exact{1} << shift;
            const Exact remainder = value % divisor;
            value /= divisor;
            if (remainder < 0) --value;
        }
        if (value < lower) { value = lower; result.qc = true; }
        if (value > upper) { value = upper; result.qc = true; }
        result.value[lane * bits / 64] |= (u64(value) & output_mask) << (lane * bits % 64);
    }
    return result;
}

std::pair<ir::Value, ir::Value> ExpandedShiftBenchmark(ir::Assembler& as, ir::Value source,
    u32 bits, u32 shift, u32 mode, u32 lanes) {
    using ir::ValueType;
    const auto imm = [&](u64 value) { return as.LoadImm(ir::Imm{value}).SetType(ValueType::U64); };
    const auto sign_extend = [&](ir::Value value, u32 width) -> ir::Value {
        if (width == 64) return value;
        auto upper = as.LslImm(value, ir::Imm{64 - width}).SetType(ValueType::U64);
        return as.AsrImm(upper, ir::Imm{64 - width}).SetType(ValueType::U64);
    };
    const bool left = mode < 3, signed_source = mode != 1 && mode != 4 && mode != 7;
    const bool signed_result = mode == 0 || mode == 3 || mode == 6;
    const u32 source_bits = left ? bits : bits * 2;
    const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    const u64 source_mask = source_bits == 64 ? UINT64_MAX : (u64{1} << source_bits) - 1;
    const std::array<ir::Value, 2> input{as.VecExtract64(source, ir::Imm{u32{0}}).SetType(ValueType::U64),
                                      as.VecExtract64(source, ir::Imm{u32{1}}).SetType(ValueType::U64)};
    std::array<ir::Value, 2> output{imm(0), imm(0)};
    auto qc = as.LoadImm(ir::Imm{u8{0}}).SetType(ValueType::U8);
    for (u32 lane = 0; lane < lanes; ++lane) {
        auto raw = as.LsrImm(input[lane * source_bits / 64], ir::Imm{lane * source_bits % 64}).SetType(ValueType::U64);
        raw = as.And(raw, ir::Operand{ir::Imm{source_mask}}).SetType(ValueType::U64);
        ir::Value value, overflow, negative;
        if (left) {
            value = as.LslImm(raw, ir::Imm{shift}).SetType(ValueType::U64);
            value = as.And(value, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
            auto original = signed_result ? sign_extend(raw, bits) : raw;
            auto truncated = signed_result ? sign_extend(value, bits) : value;
            auto recovered = signed_result ? as.AsrImm(truncated, ir::Imm{shift}).SetType(ValueType::U64) :
                                             as.LsrImm(truncated, ir::Imm{shift}).SetType(ValueType::U64);
            auto changed = as.Xor(original, ir::Operand{recovered}).SetType(ValueType::U64);
            overflow = as.TestNotZero(changed).SetType(ValueType::U8);
            negative = as.TestBit(raw, ir::Imm{bits - 1}).SetType(ValueType::U8);
            if (mode == 2) overflow = as.Or(overflow, ir::Operand{negative}).SetType(ValueType::U8);
        } else {
            auto extended = signed_source ? sign_extend(raw, source_bits) : raw;
            value = signed_source ? as.AsrImm(extended, ir::Imm{shift}).SetType(ValueType::U64) :
                                    as.LsrImm(extended, ir::Imm{shift}).SetType(ValueType::U64);
            if (mode >= 6) {
                auto round = as.LsrImm(raw, ir::Imm{shift - 1}).SetType(ValueType::U64);
                round = as.And(round, ir::Operand{ir::Imm{u64{1}}}).SetType(ValueType::U64);
                value = as.Add(value, ir::Operand{round}).SetType(ValueType::U64);
            }
            auto trimmed = as.And(value, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
            if (signed_result) trimmed = sign_extend(trimmed, bits);
            auto changed = as.Xor(value, ir::Operand{trimmed}).SetType(ValueType::U64);
            overflow = as.TestNotZero(changed).SetType(ValueType::U8);
            negative = as.TestBit(value, ir::Imm{u32{63}}).SetType(ValueType::U8);
        }
        ir::Value limit = imm(mask);
        if (signed_source) limit = as.Select(ir::BOOL{negative},
            imm(signed_result ? u64{1} << (bits - 1) : 0),
            imm(signed_result ? (u64{1} << (bits - 1)) - 1 : mask)).SetType(ValueType::U64);
        value = as.Select(ir::BOOL{overflow}, limit, value).SetType(ValueType::U64);
        qc = as.Or(qc, ir::Operand{overflow}).SetType(ValueType::U8);
        value = as.And(value, ir::Operand{ir::Imm{mask}}).SetType(ValueType::U64);
        value = as.LslImm(value, ir::Imm{lane * bits % 64}).SetType(ValueType::U64);
        output[lane * bits / 64] = as.Or(output[lane * bits / 64], ir::Operand{value}).SetType(ValueType::U64);
    }
    auto low = as.VecDup64(output[0]).SetType(ValueType::V128);
    auto high = as.VecDup64(output[1]).SetType(ValueType::V128);
    return {as.VecZip(low, high, ir::Imm{u32{64}}, ir::Imm{u32{0}}).SetType(ValueType::V128), qc};
}

IntrusivePtr<ir::Block> SaturatingShiftProgram(bool shared, u32 bits, u32 shift, u32 mode, u32 lanes) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x6000})}; ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto pattern = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto qc = as.LoadImm(ir::Imm{u8{0}}).SetType(ValueType::U8);
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        value = as.VecXor(value, pattern).SetType(ValueType::V128);
        ir::Value overflow;
        if (shared) {
            value = as.VecSatShiftImm(value, ir::Imm{bits}, ir::Imm{shift}, ir::Imm{(lanes << 4) | mode}).SetType(ValueType::V128);
            overflow = as.VecSatShiftQC(value).SetType(ValueType::U8);
        } else {
            const auto result = ExpandedShiftBenchmark(as, value, bits, shift, mode, lanes);
            value = result.first; overflow = result.second;
        }
        qc = as.Or(qc, ir::Operand{overflow}).SetType(ValueType::U8);
    }
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
    as.StoreUniform(ir::Uniform{80, ValueType::U64}, as.ZeroExtend64(qc).SetType(ValueType::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void MeasureSaturatingShift(u32 iterations) {
    std::cout << "satshift_workload,route,ir_instructions,code_bytes,frame_bytes,compile_us,execution_ns,expanded_over_route\n";
    // Every mode, every destination width, both Q0/Q1 left routes and
    // signed/unsigned 64-bit narrowing are represented; fixed 64-op blocks.
    const std::array<std::array<u32, 4>, 15> workloads{{
        {8, 3, 0, 16}, {8, 3, 1, 8}, {8, 3, 2, 16},
        {16, 5, 0, 8}, {16, 5, 1, 4}, {32, 9, 0, 4}, {32, 9, 2, 2}, {64, 17, 0, 2}, {64, 17, 1, 2},
        {8, 1, 3, 8}, {8, 1, 7, 8}, {16, 1, 5, 4}, {16, 1, 4, 4}, {32, 1, 6, 2}, {32, 1, 8, 2}}};
    for (const auto shape : workloads) {
        const auto bits = shape[0], shift = shape[1], mode = shape[2], lanes = shape[3];
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        std::array<u8, 24> expanded_result{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = SaturatingShiftProgram(route != 0, bits, shift, mode, lanes);
            std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now(); backend::x64::Translator candidate{block.get(), backend::x64::HostFeatures{}}; candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get(), backend::x64::HostFeatures{}}; translated.Translate();
            const auto native = translated.getCode<HaltReason (*)(backend::State*)>();
            Storage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = 0xdabcdef012345678ULL;
            ShiftBenchmarkVector independent{}, pattern{}; bool qc = false;
            std::memcpy(independent.data(), actual.state->uniform_buffer_begin, 16);
            std::memcpy(pattern.data(), actual.state->uniform_buffer_begin + 16, 16);
            for (u32 iteration = 0; iteration < 64; ++iteration) {
                independent[0] ^= pattern[0]; independent[1] ^= pattern[1];
                const auto result = IndependentShiftBenchmark(independent, bits, shift, mode, lanes);
                independent = result.value; qc |= result.qc;
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) ||
                actual.state->host_cpu_flags != expected.state->host_cpu_flags || actual.state->host_cpu_flags != 0xdabcdef012345678ULL)
                throw std::runtime_error("saturating shift benchmark native/interpreter or flags mismatch");
            u64 actual_qc{}; std::memcpy(&actual_qc, actual.state->uniform_buffer_begin + 80, 8);
            if (std::memcmp(actual.state->uniform_buffer_begin + 64, independent.data(), 16) || actual_qc != u64(qc))
                throw std::runtime_error("saturating shift benchmark independent exact-integer or QC mismatch");
            if (!route) std::memcpy(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 24);
            else if (std::memcmp(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 24))
                throw std::runtime_error("saturating shift benchmark shared/expanded mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples); code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << "m" << mode << "_e" << bits << "_s" << shift << "_lanes" << lanes << ',' << (route ? "shared_sse2" : "expanded_scalar") << ','
                      << instructions[route] << ',' << code_bytes[route] << ',' << frame_bytes[route] << ',' << std::fixed << std::setprecision(2) << compile_us[route] << ','
                      << execution_ns[route] << ',' << execution_ns[0] / execution_ns[route] << '\n';
    }
}
