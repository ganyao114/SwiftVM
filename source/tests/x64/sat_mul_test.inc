using SatBytes = std::array<u8, 16>;
struct SatReference { SatBytes bytes{}; u8 qc{}; };

void SatPutLane(SatBytes& bytes, u32 lane, u32 width, u32 value) {
    for (u32 byte = 0; byte < width / 8; ++byte) bytes[lane * (width / 8) + byte] = u8(value >> (byte * 8));
}

s64 SatReadLane(const SatBytes& bytes, u32 lane, u32 width) {
    u64 value{};
    for (u32 byte = 0; byte < width / 8; ++byte) value |= u64(bytes[lane * (width / 8) + byte]) << (byte * 8);
    return value & (u64{1} << (width - 1)) ? s64(value) - (s64{1} << width) : s64(value);
}

SatReference SatIndependent(const SatBytes& lhs, const SatBytes& rhs, const SatBytes& accumulator,
                            u32 width, u32 active, u32 mode) {
    using boost::multiprecision::cpp_int;
    SatReference result;
    const cpp_int divisor = cpp_int{1} << width;
    const cpp_int minimum = -(cpp_int{1} << (width - 1));
    const cpp_int maximum = (cpp_int{1} << (width - 1)) - 1;
    for (u32 lane = 0; lane < active; ++lane) {
        // Deliberately use the Arm full doubled-product equation, arbitrary
        // precision and explicit floor division, not backend's shift recipe.
        cpp_int combined = cpp_int{2} * SatReadLane(lhs, lane, width) * SatReadLane(rhs, lane, width);
        if (mode == 3) combined = -combined;
        if (mode >= 2) combined += cpp_int{SatReadLane(accumulator, lane, width)} * divisor;
        if (mode) combined += divisor / 2;
        cpp_int value = combined / divisor;
        if (combined < 0 && combined % divisor != 0) --value;
        if (value < minimum) { value = minimum; result.qc = 1; }
        if (value > maximum) { value = maximum; result.qc = 1; }
        const cpp_int bits = value < 0 ? value + divisor : value;
        SatPutLane(result.bytes, lane, width, bits.convert_to<u32>());
    }
    return result;
}

IntrusivePtr<ir::Block> SatProgram(u32 width, u32 active, u32 mode) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xeb00})}; ir::Assembler as{block.get()};
    auto lhs = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto rhs = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto acc = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
    auto value = as.VecSatMulHigh(lhs, rhs, acc, ir::Imm{u64(width)}, ir::Imm{u64((active << 2) | mode)}).SetType(ValueType::V128);
    auto qc = as.VecSatMulQC(value).SetType(ValueType::U8);
    as.StoreUniform(ir::Uniform{48, ValueType::V128}, value);
    as.StoreUniform(ir::Uniform{64, ValueType::U8}, qc);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}

void SatRunCase(Compiled& compiled, const SatBytes& lhs, const SatBytes& rhs, const SatBytes& accumulator,
                u32 width, u32 active, u32 mode) {
    Storage actual; std::array<u8, 4096> expected{};
    for (u32 byte = 0; byte < expected.size(); ++byte) expected[byte] = u8(byte * 39 + 83);
    std::memcpy(expected.data(), lhs.data(), 16); std::memcpy(expected.data() + 16, rhs.data(), 16);
    std::memcpy(expected.data() + 32, accumulator.data(), 16);
    std::memcpy(actual.state->uniform_buffer_begin, expected.data(), expected.size());
    const auto wanted = SatIndependent(lhs, rhs, accumulator, width, active, mode);
    std::memcpy(expected.data() + 48, wanted.bytes.data(), 16); expected[64] = wanted.qc;
    actual.state->host_cpu_flags = 0xdeadbeefabcdef01ULL;
    const auto original = _mm_getcsr(); const auto changed = (original & ~0x6000u) | 0x4000u | 0x3fu;
    _mm_setcsr(changed); const auto halt = compiled.fn(actual.state); const auto observed = _mm_getcsr(); _mm_setcsr(original);
    const auto detail = std::to_string(width) + "/" + std::to_string(active) + "/" + std::to_string(mode);
    Check(halt == HaltReason::CallHost, "VecSatMulHigh halt " + detail);
    Check(std::memcmp(actual.state->uniform_buffer_begin, expected.data(), expected.size()) == 0,
          "VecSatMulHigh cpp_int result/QC/all uniform preservation " + detail);
    Check(actual.state->host_cpu_flags == 0xdeadbeefabcdef01ULL, "VecSatMulHigh/QC preserve guest flags");
    Check(observed == changed, "VecSatMulHigh/QC preserve host FP status");
}

void SatMulDirectedAndRandom() {
    u64 seed = 0x343276108faebb75ULL;
    for (bool baseline : {false, true}) for (u32 width : {16u, 32u}) {
        const u32 sign = u32{1} << (width - 1), mask = width == 32 ? UINT32_MAX : 65535;
        const std::array<u32, 11> edges{0, 1, 2, mask, mask - 1, sign, sign + 1,
                                       sign - 1, sign - 2, sign / 2, sign / 2 + 1};
        const std::array<u32, 3> actives{1, width == 16 ? 4u : 2u, 128 / width};
        for (u32 active : actives) for (u32 mode = 0; mode < 4; ++mode) {
            auto block = SatProgram(width, active, mode);
            Compiled compiled{block.get(), baseline ? x64::HostFeatures{} : x64::HostFeatures::Detect()};
            for (u32 left : edges) for (u32 right : edges) for (u32 acc : edges) {
                SatBytes lhs{}, rhs{}, accumulator{};
                for (u32 lane = 0; lane < 128 / width; ++lane) {
                    // Keep lane0 exact edge Cartesian coverage; other lanes
                    // differ so reduction/packing order is independently seen.
                    SatPutLane(lhs, lane, width, (left + lane * 131) & mask);
                    SatPutLane(rhs, lane, width, (right + lane * 67) & mask);
                    SatPutLane(accumulator, lane, width, (acc + lane * 197) & mask);
                }
                SatRunCase(compiled, lhs, rhs, accumulator, width, active, mode);
            }
            for (u32 iteration = 0; iteration < 512; ++iteration) {
                SatBytes lhs{}, rhs{}, accumulator{};
                for (u32 lane = 0; lane < 128 / width; ++lane) {
                    SatPutLane(lhs, lane, width, Random(seed)); SatPutLane(rhs, lane, width, Random(seed));
                    SatPutLane(accumulator, lane, width, Random(seed));
                }
                SatRunCase(compiled, lhs, rhs, accumulator, width, active, mode);
            }
        }
    }
    // The mathematical oracle itself is pinned to key architectural results:
    // final-only saturation, negative ties rounded toward positive infinity.
    for (u32 width : {16u, 32u}) {
        const u32 sign = u32{1} << (width - 1);
        SatBytes min{}, zero{}, one{}, half{};
        SatPutLane(min, 0, width, sign); SatPutLane(one, 0, width, 1); SatPutLane(half, 0, width, sign / 2);
        auto add = SatIndependent(min, min, min, width, 1, 2);
        Check(SatReadLane(add.bytes, 0, width) == 0 && add.qc == 0, "MIN*MIN + MIN cancels before final saturation");
        auto sub = SatIndependent(min, min, zero, width, 1, 3);
        Check(SatReadLane(sub.bytes, 0, width) == -s64(sign) && sub.qc == 0, "MIN*MIN subtract yields exact MIN without QC");
        auto positive_tie = SatIndependent(one, half, zero, width, 1, 2);
        auto negative_tie = SatIndependent(one, half, zero, width, 1, 3);
        Check(SatReadLane(positive_tie.bytes, 0, width) == 1 && SatReadLane(negative_tie.bytes, 0, width) == 0,
              "opposite half ties both round toward positive infinity");
    }
    std::cout << "PASS VecSatMulHigh all modes/active sizes directed and random cpp_int oracle\n";
}

void SatMulInactiveAndAliases() {
    u64 seed = 0x698175635434eccdULL;
    for (u32 width : {16u, 32u}) for (u32 active : {1u, 64 / width, 128 / width}) for (u32 mode = 0; mode < 4; ++mode) {
        auto block = SatProgram(width, active, mode); Compiled compiled{block.get(), x64::HostFeatures{}};
        SatBytes lhs{}, rhs{}, accumulator{};
        for (u32 lane = 0; lane < 128 / width; ++lane) {
            const u32 sign = u32{1} << (width - 1);
            SatPutLane(lhs, lane, width, lane < active ? 0 : sign);
            SatPutLane(rhs, lane, width, lane < active ? 0 : sign);
            SatPutLane(accumulator, lane, width, lane < active ? 0 : sign - 1);
        }
        SatRunCase(compiled, lhs, rhs, accumulator, width, active, mode);
        // Saturation in any active lane must survive reduction, including
        // the last lane in a half/full vector.
        for (u32 lane = 0; lane < active; ++lane) {
            SatBytes active_lhs{}, active_rhs{}, active_acc{};
            SatPutLane(active_lhs, lane, width, u32{1} << (width - 1));
            SatPutLane(active_rhs, lane, width, u32{1} << (width - 1));
            if (mode == 3) SatPutLane(active_acc, lane, width, u32{1} << (width - 1));
            SatRunCase(compiled, active_lhs, active_rhs, active_acc, width, active, mode);
        }
    }
    // The producer has no remaining payload consumer: only the late QC
    // pseudo keeps its metadata alive while unrelated scalar homes recycle.
    for (u32 width : {16u, 32u}) for (bool saturated : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xec80})}; ir::Assembler as{block.get()};
        auto lhs = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto rhs = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto acc = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
        auto producer = as.VecSatMulHigh(lhs, rhs, acc, ir::Imm{u64(width)}, ir::Imm{u64{5}}).SetType(ValueType::V128);
        auto value = as.LoadImm(ir::Imm{u64{1}}).SetType(ValueType::U64);
        for (u32 step = 0; step < 80; ++step)
            value = as.Add(value, ir::Operand{ir::Imm{u64{17}}}).SetType(ValueType::U64);
        auto qc = as.VecSatMulQC(producer).SetType(ValueType::U8);
        as.StoreUniform(ir::Uniform{64, ValueType::U8}, qc); as.StoreUniform(ir::Uniform{72, ValueType::U64}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get(), x64::HostFeatures{}};
        Storage actual; const u32 input = saturated ? u32{1} << (width - 1) : 0;
        SatBytes lhs_bytes{}, rhs_bytes{}; SatPutLane(lhs_bytes, 0, width, input); SatPutLane(rhs_bytes, 0, width, input);
        std::memcpy(actual.state->uniform_buffer_begin, lhs_bytes.data(), 16);
        std::memcpy(actual.state->uniform_buffer_begin + 16, rhs_bytes.data(), 16);
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "QC-only late consumer halt");
        Check(actual.Get(64) == u64(saturated) && actual.Get(72) == 1361, "QC-only use retains producer metadata across recycled scalar homes");
    }
    for (bool baseline : {false, true}) for (u32 width : {16u, 32u}) for (u32 mode = 0; mode < 4; ++mode)
        for (u32 alias = 0; alias < 5; ++alias) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xec00})}; ir::Assembler as{block.get()};
            auto lhs = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
            auto rhs = alias == 1 || alias == 4 ? lhs : as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
            auto acc = alias == 2 || alias == 4 ? lhs : alias == 3 ? rhs :
                       as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
            const u32 active = 128 / width;
            auto first = as.VecSatMulHigh(lhs, rhs, acc, ir::Imm{u64(width)}, ir::Imm{u64((active << 2) | mode)}).SetType(ValueType::V128);
            auto second = as.VecSatMulHigh(rhs, lhs, first, ir::Imm{u64(width)}, ir::Imm{u64((active << 2) | ((mode + 1) % 4))}).SetType(ValueType::V128);
            auto chained = first;
            for (u32 step = 0; step < 40; ++step) {
                chained = as.VecXor(chained, lhs).SetType(ValueType::V128);
                chained = as.VecAdd(chained, rhs, ir::Imm{u32{32}}).SetType(ValueType::V128);
            }
            auto first_qc = as.VecSatMulQC(first).SetType(ValueType::U8);
            auto second_qc = as.VecSatMulQC(second).SetType(ValueType::U8);
            auto repeated_qc = as.VecSatMulQC(first).SetType(ValueType::U8);
            as.StoreUniform(ir::Uniform{48, ValueType::V128}, first); as.StoreUniform(ir::Uniform{64, ValueType::V128}, second);
            as.StoreUniform(ir::Uniform{80, ValueType::U8}, first_qc); as.StoreUniform(ir::Uniform{81, ValueType::U8}, second_qc);
            as.StoreUniform(ir::Uniform{82, ValueType::U8}, repeated_qc);
            as.StoreUniform(ir::Uniform{96, ValueType::V128}, chained);
            as.StoreUniform(ir::Uniform{112, ValueType::V128}, lhs); as.StoreUniform(ir::Uniform{128, ValueType::V128}, rhs);
            as.StoreUniform(ir::Uniform{144, ValueType::V128}, acc);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled compiled{block.get(), baseline ? x64::HostFeatures{} : x64::HostFeatures::Detect()};
            Check(compiled.translator.FrameSize() <= 512, "saturation metadata last-use permits other home recycling");
            for (u32 iteration = 0; iteration < 64; ++iteration) {
                Storage actual; std::array<u8, 4096> expected{};
                for (u32 byte = 0; byte < expected.size(); byte += 8) {
                    const u64 bits = Random(seed); std::memcpy(expected.data() + byte, &bits, 8);
                }
                std::memcpy(actual.state->uniform_buffer_begin, expected.data(), expected.size());
                SatBytes left{}, right{}, accumulator{};
                std::memcpy(left.data(), expected.data(), 16);
                std::memcpy(right.data(), expected.data() + (alias == 1 || alias == 4 ? 0 : 16), 16);
                std::memcpy(accumulator.data(), expected.data() + (alias == 2 || alias == 4 ? 0 : alias == 3 ? 16 : 32), 16);
                const auto wanted_first = SatIndependent(left, right, accumulator, width, active, mode);
                const auto wanted_second = SatIndependent(right, left, wanted_first.bytes, width, active, (mode + 1) % 4);
                std::array<u32, 4> chain{}, left_words{}, right_words{};
                std::memcpy(chain.data(), wanted_first.bytes.data(), 16);
                std::memcpy(left_words.data(), left.data(), 16); std::memcpy(right_words.data(), right.data(), 16);
                for (u32 step = 0; step < 40; ++step)
                    for (u32 lane = 0; lane < 4; ++lane) chain[lane] = (chain[lane] ^ left_words[lane]) + right_words[lane];
                std::memcpy(expected.data() + 48, wanted_first.bytes.data(), 16); std::memcpy(expected.data() + 64, wanted_second.bytes.data(), 16);
                expected[80] = wanted_first.qc; expected[81] = wanted_second.qc; expected[82] = wanted_first.qc;
                std::memcpy(expected.data() + 96, chain.data(), 16); std::memcpy(expected.data() + 112, left.data(), 16);
                std::memcpy(expected.data() + 128, right.data(), 16); std::memcpy(expected.data() + 144, accumulator.data(), 16);
                actual.state->host_cpu_flags = Random(seed); const auto flags = actual.state->host_cpu_flags;
                Check(compiled.fn(actual.state) == HaltReason::CallHost, "saturation aliased input and delayed QC halt");
                Check(std::memcmp(actual.state->uniform_buffer_begin, expected.data(), expected.size()) == 0,
                      "saturation old SSA/aliases/delayed QC remain independent of second producer and temporaries");
                Check(actual.state->host_cpu_flags == flags, "saturation delayed QC preserves flags");
            }
        }
    std::cout << "PASS saturation inactive-lane QC exclusion, aliases and delayed pseudo metadata\n";
}

void SatMulInvalidIR() {
    const auto reject = [](u64 width, u64 control, s32 bad_input = -1, ValueType bad_type = ValueType::V64,
                           bool fake_cast = false, ValueType result_type = ValueType::V128) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xed00})}; ir::Assembler as{block.get()};
        std::array<ir::Value, 3> input;
        for (u32 index = 0; index < 3; ++index) {
            const auto type = s32(index) == bad_input ? bad_type : ValueType::V128;
            input[index] = type == ValueType::VOID ? as.LoadImm(ir::Imm{u64{0}}).SetType(type) :
                                                    as.LoadUniform(ir::Uniform{index * 16, type}).SetType(type);
            if (s32(index) == bad_input && fake_cast) input[index] = input[index].SetCastType(ValueType::V128);
        }
        auto value = as.VecSatMulHigh(input[0], input[1], input[2], ir::Imm{width}, ir::Imm{control}).SetType(result_type);
        as.StoreUniform(ir::Uniform{64, result_type}, value); block->SetTerminal(ir::terminal::ReturnToHost{});
        bool rejected{};
        try { Compiled compiled{block.get(), x64::HostFeatures{}}; } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "saturation strict full immediate/type contract rejected");
    };
    for (u64 width : {u64{0}, u64{8}, u64{64}, u64{16} + (u64{1} << 32), UINT64_MAX}) reject(width, 4);
    for (u64 control : {u64{0}, u64{8}, u64{12}, u64{20}, u64{36}, u64{4} + (u64{1} << 32), UINT64_MAX}) reject(16, control);
    for (u64 control : {u64{0}, u64{12}, u64{32}, u64{4} + (u64{1} << 32), UINT64_MAX}) reject(32, control);
    for (u32 input = 0; input < 3; ++input) for (auto type : {ValueType::VOID, ValueType::U32, ValueType::V64, ValueType::V256}) {
        reject(16, 4, input, type); reject(32, 4, input, type, true);
    }
    for (auto type : {ValueType::VOID, ValueType::U8, ValueType::V64, ValueType::V256}) reject(16, 4, -1, ValueType::V64, false, type);
    for (u32 malformed = 0; malformed < 4; ++malformed) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xed80})}; ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto producer = malformed == 0 ? input :
            as.VecSatMulHigh(input, input, input, ir::Imm{u64{16}}, ir::Imm{u64{4}}).SetType(ValueType::V128);
        if (malformed == 1) producer = producer.SetCastType(ValueType::V64);
        auto qc = as.VecSatMulQC(producer).SetType(malformed == 2 ? ValueType::U32 : malformed == 3 ? ValueType::VOID : ValueType::U8);
        as.StoreUniform(ir::Uniform{64, ValueType::U8}, qc); block->SetTerminal(ir::terminal::ReturnToHost{});
        bool rejected{};
        try { Compiled compiled{block.get(), x64::HostFeatures{}}; } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "QC pseudo rejects unrelated/cast producer and invalid return type");
    }
    std::cout << "PASS saturation producer and QC pseudo invalid IR rejection\n";
}
