// Independent arbitrary-precision reference; never uses runtime shift helpers.
using ShiftBig = boost::multiprecision::cpp_int;
struct ShiftOracle { std::array<u64, 2> value{}; u8 qc{}; };
u64 ShiftTestMask(u32 bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
ShiftOracle ShiftIndependent(std::array<u64, 2> input, u32 bits, u32 shift, u32 lanes, u32 mode) {
    const bool left = mode < 3, signed_source = mode != 1 && mode != 4 && mode != 7;
    const bool signed_result = mode == 0 || mode == 3 || mode == 6;
    const u32 source_bits = left ? bits : bits * 2;
    const ShiftBig lower = signed_result ? -(ShiftBig(1) << (bits - 1)) : ShiftBig(0);
    const ShiftBig upper = signed_result ? (ShiftBig(1) << (bits - 1)) - 1 : (ShiftBig(1) << bits) - 1;
    ShiftOracle expected;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const u64 raw = (input[lane * source_bits / 64] >> (lane * source_bits % 64)) & ShiftTestMask(source_bits);
        ShiftBig value = raw;
        if (signed_source && (raw >> (source_bits - 1))) value -= ShiftBig(1) << source_bits;
        if (left) value *= ShiftBig(1) << shift;
        else { if (mode >= 6) value += ShiftBig(1) << (shift - 1); value >>= shift; }
        if (value < lower) { value = lower; expected.qc = 1; }
        if (value > upper) { value = upper; expected.qc = 1; }
        value &= (ShiftBig(1) << bits) - 1;
        expected.value[lane * bits / 64] |= value.convert_to<u64>() << (lane * bits % 64);
    }
    return expected;
}
ShiftBig ShiftBoundary(u32 bits, u32 shift, u32 mode, u32 profile) {
    const u32 source_bits = mode < 3 ? bits : bits * 2;
    const ShiftBig one = 1, sign = one << (source_bits - 1), mask = (one << source_bits) - 1;
    const bool signed_result = mode == 0 || mode == 3 || mode == 6;
    const ShiftBig maximum = signed_result ? (one << (bits - 1)) - 1 : (one << bits) - 1;
    const ShiftBig minimum = signed_result ? -(one << (bits - 1)) : ShiftBig(0);
    ShiftBig upper = maximum, lower = minimum;
    if (mode < 3) { upper >>= shift; lower >>= shift; }
    else { upper <<= shift; lower <<= shift; }
    const ShiftBig half = one << (shift ? shift - 1 : 0);
    switch (profile % 20) {
        case 0: return 0; case 1: return 1; case 2: return mask;
        case 3: return sign; case 4: return sign - 1; case 5: return sign + 1;
        case 6: return maximum; case 7: return maximum + 1;
        case 8: return upper; case 9: return upper + 1; case 10: return upper - 1;
        case 11: return lower; case 12: return lower - 1; case 13: return lower + 1;
        case 14: return upper + half; case 15: return upper + half - 1;
        case 16: return lower - half; case 17: return lower - half + 1;
        case 18: return -half; default: return -half - 1;
    }
}
std::array<u64, 2> ShiftInput(u32 bits, u32 shift, u32 mode, u32 sample, u64& random) {
    const u32 source_bits = mode < 3 ? bits : bits * 2;
    std::array<u64, 2> value{};
    for (u32 lane = 0; lane < 128 / source_bits; ++lane) {
        ShiftBig raw = sample < 20 ? ShiftBoundary(bits, shift, mode, sample + lane) : ShiftBig(Random(random));
        raw &= (ShiftBig(1) << source_bits) - 1;
        value[lane * source_bits / 64] |= raw.convert_to<u64>() << (lane * source_bits % 64);
    }
    return value;
}
void SatShiftAllShapesAndBoundaries() {
    u64 random = 0x263087cafedd4713ULL;
    for (u32 mode = 0; mode < 9; ++mode) for (u32 bits : {8u, 16u, 32u, 64u}) for (bool full : {false, true}) {
        if ((mode < 3 && bits == 64 && !full) || (mode >= 3 && (bits == 64 || full))) continue;
        const u32 lanes = (mode >= 3 || !full ? 64 : 128) / bits;
        const u32 first = mode < 3 ? 0 : 1, last = mode < 3 ? bits - 1 : bits;
        for (u32 shift = first; shift <= last; ++shift) for (bool alias : {false, true}) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf000})}; ir::Assembler as{block.get()};
            auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
            auto value = as.VecSatShiftImm(input, ir::Imm{bits}, ir::Imm{shift}, ir::Imm{(u64{lanes} << 4) | mode}).SetType(ValueType::V128);
            auto first_qc = as.VecSatShiftQC(value).SetType(ValueType::U8);
            for (u32 index = 0; index < 8; ++index)
                as.StoreUniform(ir::Uniform{256, ValueType::U64}, as.LoadImm(ir::Imm{u64{index}}).SetType(ValueType::U64));
            auto second_qc = as.VecSatShiftQC(value).SetType(ValueType::U8);
            as.StoreUniform(ir::Uniform{alias ? 0u : 64u, ValueType::V128}, value);
            as.StoreUniform(ir::Uniform{96, ValueType::U8}, first_qc);
            as.StoreUniform(ir::Uniform{97, ValueType::U8}, second_qc);
            as.StoreUniform(ir::Uniform{128, ValueType::V128}, input);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
                Compiled compiled{block.get(), features};
                for (u32 sample = 0; sample < 32; ++sample) {
                    const auto source = ShiftInput(bits, shift, mode, sample, random);
                    const auto wanted = ShiftIndependent(source, bits, shift, lanes, mode);
                    Storage actual, expected;
                    std::memcpy(actual.state->uniform_buffer_begin, source.data(), 16);
                    std::memcpy(expected.state->uniform_buffer_begin, source.data(), 16);
                    const u64 flags = Random(random); actual.state->host_cpu_flags = flags; expected.state->host_cpu_flags = flags;
                    const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                    const std::string label = "satshift mode=" + std::to_string(mode) + "/bits=" + std::to_string(bits) +
                        "/shift=" + std::to_string(shift) + "/sample=" + std::to_string(sample);
                    Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, label + " halt");
                    Check(!std::memcmp(actual.state->uniform_buffer_begin + (alias ? 0 : 64), wanted.value.data(), 16) &&
                          !std::memcmp(expected.state->uniform_buffer_begin + (alias ? 0 : 64), wanted.value.data(), 16), label + " exact bigint clamp/round/inactive bits");
                    Check(actual.state->uniform_buffer_begin[96] == wanted.qc && actual.state->uniform_buffer_begin[97] == wanted.qc &&
                          expected.state->uniform_buffer_begin[96] == wanted.qc && expected.state->uniform_buffer_begin[97] == wanted.qc, label + " normalized repeated QC");
                    Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr &&
                          !std::memcmp(actual.state->uniform_buffer_begin + 128, source.data(), 16) &&
                          !std::memcmp(expected.state->uniform_buffer_begin + 128, source.data(), 16), label + " live original/flags/host FP preserved");
                }
            }
        }
    }
    std::cout << "PASS shared saturating shift every shape/immediate/alias/BigInt clamp+round/QC\n";
}

void SatShiftProducerTimeAndPressure() {
    u64 random = 0x2881473cab091677ULL;
    for (u32 mode = 0; mode < 9; ++mode) for (u32 layout = 0; layout < 3; ++layout) for (u32 pressure : {1u, 24u, 64u}) {
        const u32 bits = mode < 3 ? 8 : 32, shift = mode < 3 ? 7 : 1, lanes = mode < 3 ? 8 : 2;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf100})}; ir::Assembler as{block.get()};
        std::vector<ir::Value> keep;
        for (u32 index = 0; index < pressure; ++index)
            keep.push_back(as.LoadUniform(ir::Uniform{128 + index * 8, ValueType::U64}).SetType(ValueType::U64));
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto value = as.VecSatShiftImm(input, ir::Imm{bits}, ir::Imm{shift}, ir::Imm{(u64{lanes} << 4) | mode}).SetType(ValueType::V128);
        for (u32 index = 0; index < pressure; ++index) as.StoreUniform(ir::Uniform{768 + index * 8, ValueType::U64}, keep[index]);
        auto other = as.VecSatShiftImm(input, ir::Imm{bits}, ir::Imm{shift}, ir::Imm{(u64{lanes} << 4) | ((mode + 1) % 3 + (mode < 3 ? 0 : 3))}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, other);
        ir::Value skip;
        if (layout == 1) skip = as.Goto(ir::BOOL{as.LoadUniform(ir::Uniform{64, ValueType::U8}).SetType(ValueType::U8)});
        auto qc = as.VecSatShiftQC(value).SetType(ValueType::U8), repeated = as.VecSatShiftQC(value).SetType(ValueType::U8);
        if (layout == 1) as.BindLabel(skip);
        as.StoreUniform(ir::Uniform{2048, ValueType::U8}, qc); as.StoreUniform(ir::Uniform{2049, ValueType::U8}, repeated);
        as.StoreUniform(ir::Uniform{2064, ValueType::V128}, value);
        if (layout == 2) {
            auto counter = as.LoadUniform(ir::Uniform{72, ValueType::U64}).SetType(ValueType::U64);
            auto next = as.Sub(counter, ir::Operand{ir::Imm{u64{1}}}).SetType(ValueType::U64);
            as.StoreUniform(ir::Uniform{72, ValueType::U64}, next);
            auto back = as.Goto(ir::BOOL{as.TestNotZero(next).SetType(ValueType::U8)}); as.BindLabel(back);
            auto* label = &block->GetInstList().back(); block->RemoveInst(label); block->InsertBefore(label, qc.Def());
        }
        for (u32 index = 0; index < 128; ++index)
            as.StoreUniform(ir::Uniform{2080, ValueType::U64}, as.LoadImm(ir::Imm{u64{index}}).SetType(ValueType::U64));
        block->SetTerminal(ir::terminal::If{ir::BOOL{qc}, ir::terminal::LinkBlock{ir::Location{0xf200}}, ir::terminal::LinkBlock{ir::Location{0xf300}}});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 24; ++sample) {
                const auto input_value = ShiftInput(bits, shift, mode, sample, random), wanted = ShiftIndependent(input_value, bits, shift, lanes, mode).value;
                const auto wanted_qc = ShiftIndependent(input_value, bits, shift, lanes, mode).qc;
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    std::memcpy(storage->state->uniform_buffer_begin, input_value.data(), 16); storage->Put(64, sample & 1); storage->Put(72, 4);
                    storage->state->host_cpu_flags = 0xdbca2345167890abULL;
                    for (u32 index = 0; index < pressure; ++index) storage->Put(128 + index * 8, 0x1122334400000000ULL + index);
                }
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == reference.Run() && actual.state->current_loc == ir::Location{wanted_qc ? 0xf200u : 0xf300u}, "satshift early QC terminal");
                Check(actual.state->uniform_buffer_begin[2048] == wanted_qc && actual.state->uniform_buffer_begin[2049] == wanted_qc &&
                      expected.state->uniform_buffer_begin[2048] == wanted_qc && expected.state->uniform_buffer_begin[2049] == wanted_qc &&
                      !std::memcmp(actual.state->uniform_buffer_begin + 2064, wanted.data(), 16), "satshift marker skipped/revisited and second producer preserves early QC");
                Check(actual.state->host_cpu_flags == 0xdbca2345167890abULL && expected.state->host_cpu_flags == 0xdbca2345167890abULL &&
                      _mm_getcsr() == mxcsr && actual.Get(2080) == 127 && (layout != 2 || actual.Get(72) == 0), "satshift pressure and host/guest state");
                for (u32 index = 0; index < pressure; ++index)
                    Check(actual.Get(768 + index * 8) == 0x1122334400000000ULL + index && expected.Get(768 + index * 8) == actual.Get(768 + index * 8),
                          "satshift early QC cannot overwrite older live homes");
            }
        }
    }
    std::cout << "PASS shared saturating shift producer QC / skipped+revisited markers / pressure / terminal\n";
}

void SatShiftInvalidIR() {
    const auto reject = [](u64 bits, u64 shift, u64 control, u32 corruption = 0, ValueType bad_type = ValueType::V64) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf400})}; ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        if (corruption == 1) input = input.SetCastType(bad_type);
        if (corruption == 2) input.Def()->SetReturn(bad_type);
        auto value = as.VecSatShiftImm(input, ir::Imm{bits}, ir::Imm{shift}, ir::Imm{control}).SetType(corruption == 3 ? bad_type : ValueType::V128);
        if (corruption >= 4 && corruption <= 6) value.Def()->SetArg(corruption - 3, input);
        if (corruption == 7) value.Def()->SetArg(0, ir::Imm{u64{0}});
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool native_rejected{}, interpreter_rejected{};
        try { Compiled compiled{block.get(), x64::HostFeatures{}}; } catch (const std::runtime_error&) { native_rejected = true; }
        try { Storage state; interpreter_rejected = backend::interp::Interpreter{*state.state, block.get()}.Run() == HaltReason::IllegalCode; }
        catch (const std::runtime_error&) { interpreter_rejected = true; }
        Check(native_rejected && interpreter_rejected, "satshift rejects malformed full immediate/type/argument before publication");
    };
    for (u64 bits : {u64{0}, u64{1}, u64{7}, u64{128}, (u64{1} << 32) + 8, UINT64_MAX}) reject(bits, 0, 256);
    for (u64 shift : {u64{8}, u64{64}, (u64{1} << 32), UINT64_MAX}) reject(8, shift, 256);
    for (u64 control : {u64{0}, u64{16}, u64{48}, u64{128} + 9, u64{256} + 15, (u64{1} << 32) + 256, UINT64_MAX}) reject(8, 0, control);
    reject(64, 0, 16); reject(64, 1, 19);
    for (u64 shift : {u64{0}, u64{9}, (u64{1} << 32) + 1}) reject(8, shift, 131);
    reject(8, 1, 259); reject(8, 1, 19);
    for (u32 corruption : {1u, 2u, 3u}) for (auto type : {ValueType::VOID, ValueType::U8, ValueType::U64, ValueType::V8, ValueType::V64, ValueType::V256})
        if (corruption != 1 || type != ValueType::VOID) reject(8, 0, 256, corruption, type);
    for (u32 corruption = 4; corruption <= 7; ++corruption) reject(8, 0, 256, corruption);
    for (u32 corruption = 0; corruption < 5; ++corruption) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf500})}; ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto producer = corruption == 0 ? ir::Value{input} : ir::Value{as.VecSatShiftImm(input, ir::Imm{u64{8}}, ir::Imm{u64{0}}, ir::Imm{u64{256}}).SetType(ValueType::V128)};
        if (corruption == 1) producer = producer.SetCastType(ValueType::V64);
        auto qc = as.VecSatShiftQC(producer).SetType(corruption == 2 ? ValueType::U32 : corruption == 3 ? ValueType::VOID : ValueType::U8);
        if (corruption == 4) qc.Def()->SetArg(0, ir::Imm{u64{0}});
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool rejected{};
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "satshift QC rejects wrong producer/view/result and malformed argument");
    }
    std::cout << "PASS shared saturating shift strict full-immediate/canonical-types/pseudo validation\n";
}

void SatShiftForwardedVectors() {
    u64 random = 0x487199eade667541ULL;
    for (u32 mode = 0; mode < 9; ++mode) for (u32 bits : {8u, 16u, 32u, 64u}) {
        if (mode >= 3 && bits == 64) continue;
        const u32 shift = mode < 3 ? bits - 1 : 1, lanes = (mode < 3 ? 128 : 64) / bits;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf600})}; ir::Assembler as{block.get()};
        auto first = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto second = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto mixed = as.VecXor(first, second).SetType(ValueType::V128);
        auto shifted = as.VecSatShiftImm(mixed, ir::Imm{bits}, ir::Imm{shift}, ir::Imm{(u64{lanes} << 4) | mode}).SetType(ValueType::V128);
        auto output = as.VecXor(shifted, first).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, output);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get(), x64::HostFeatures{}};
        for (u32 sample = 0; sample < 32; ++sample) {
            std::array<u64, 2> lhs{Random(random), Random(random)}, rhs{Random(random), Random(random)}, combined{lhs[0] ^ rhs[0], lhs[1] ^ rhs[1]};
            auto wanted = ShiftIndependent(combined, bits, shift, lanes, mode).value;
            wanted[0] ^= lhs[0]; wanted[1] ^= lhs[1]; Storage state;
            std::memcpy(state.state->uniform_buffer_begin, lhs.data(), 16); std::memcpy(state.state->uniform_buffer_begin + 16, rhs.data(), 16);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "satshift forwarded chain halt");
            Check(!std::memcmp(state.state->uniform_buffer_begin + 64, wanted.data(), 16), "satshift consumes forwarded XMM2 input and forwards packed result");
        }
    }
    std::cout << "PASS shared saturating shift forwarded XOR inputs and packed results\n";
}
