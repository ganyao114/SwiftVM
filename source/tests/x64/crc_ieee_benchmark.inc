// Include after Median/Measure inside benchmark.cpp's fixture namespace.
u32 BenchmarkIeeeForward(u32 initial, u64 data, u32 width) {
    const auto reverse = [](u32 value) {
        u32 result{};
        for (u32 bit = 0; bit < 32; ++bit) result |= ((value >> bit) & 1u) << (31 - bit);
        return result;
    };
    u32 value = reverse(initial);
    for (u32 bit = 0; bit < width; ++bit) {
        const bool reduce = ((value >> 31) ^ (data >> bit)) & 1;
        value <<= 1;
        if (reduce) value ^= 0x04C11DB7u;
    }
    return reverse(value);
}

IntrusivePtr<ir::Block> IeeeCrcProgram(bool shared, u32 width) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5000})}; ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::U32}).SetType(ValueType::U32);
    const auto data_type = width == 64 ? ValueType::U64 : ValueType::U32;
    auto data = as.LoadUniform(ir::Uniform{8, data_type}).SetType(data_type);
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        if (shared) { value = as.Crc32(value, data, ir::Imm{width}).SetType(ValueType::U32); continue; }
        // Preserve the former A64 frontend expansion, including its zero
        // materialization, so code/frame/compile comparisons measure the
        // actual representation that the shared opcode replaces.
        for (u32 byte = 0; byte < width / 8; ++byte) {
            auto next = width == 64 ? as.LsrImm(data, ir::Imm{byte * 8}).SetType(ValueType::U64) :
                                     as.LsrImm(data, ir::Imm{byte * 8}).SetType(ValueType::U32);
            next = as.And(next, ir::Operand{ir::Imm{u32{255}}}).SetType(ValueType::U32);
            value = as.Xor(value, ir::Operand{next}).SetType(ValueType::U32);
            for (u32 bit = 0; bit < 8; ++bit) {
                auto low = as.And(value, ir::Operand{ir::Imm{u32{1}}}).SetType(ValueType::U32);
                auto zero = as.LoadImm(ir::Imm{u32{0}}).SetType(ValueType::U32);
                auto mask = as.Sub(zero, ir::Operand{low}).SetType(ValueType::U32);
                auto reduction = as.And(mask, ir::Operand{ir::Imm{u32{0xEDB88320}}}).SetType(ValueType::U32);
                auto shifted = as.LsrImm(value, ir::Imm{u32{1}}).SetType(ValueType::U32);
                value = as.Xor(shifted, ir::Operand{reduction}).SetType(ValueType::U32);
            }
        }
    }
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, as.ZeroExtend32To64(value).SetType(ValueType::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void MeasureIeeeCrc(u32 iterations) {
    std::cout << "crc_workload,route,ir_instructions,code_bytes,frame_bytes,compile_us,execution_ns,expanded_over_route\n";
    for (u32 width : {8u, 16u, 32u, 64u}) {
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        u32 expanded_result{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = IeeeCrcProgram(route != 0, width);
            std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now(); backend::x64::Translator candidate{block.get()}; candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get()}; translated.Translate();
            const auto native = translated.getCode<HaltReason (*)(backend::State*)>();
            Storage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = 0xdabcdef012345678ULL;
            u32 independent{}; u64 input{};
            std::memcpy(&independent, actual.state->uniform_buffer_begin, 4);
            std::memcpy(&input, actual.state->uniform_buffer_begin + 8, width == 64 ? 8 : 4);
            for (u32 iteration = 0; iteration < 64; ++iteration) independent = BenchmarkIeeeForward(independent, input, width);
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) ||
                actual.state->host_cpu_flags != expected.state->host_cpu_flags || actual.state->host_cpu_flags != 0xdabcdef012345678ULL)
                throw std::runtime_error("IEEE CRC benchmark native/interpreter or flags mismatch");
            u64 actual_result{}; std::memcpy(&actual_result, actual.state->uniform_buffer_begin + 16, 8);
            if (actual_result != independent) throw std::runtime_error("IEEE CRC benchmark independent polynomial/zero extension mismatch");
            if (route == 0) expanded_result = u32(actual_result);
            else if (actual_result != expanded_result) throw std::runtime_error("IEEE CRC benchmark shared/expanded mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples); code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << "ieee_" << width << ',' << (route ? "shared_table" : "expanded_scalar") << ',' << instructions[route] << ','
                      << code_bytes[route] << ',' << frame_bytes[route] << ',' << std::fixed << std::setprecision(2) << compile_us[route] << ','
                      << execution_ns[route] << ',' << execution_ns[0] / execution_ns[route] << '\n';
    }
}
