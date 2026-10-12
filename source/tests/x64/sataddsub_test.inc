using SatAddSubVector = std::array<u64, 2>;
u64 SatAddSubMask(u32 bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
u64 SatAddSubLane(SatAddSubVector v, u32 bits, u32 lane) {
    return (v[lane * bits / 64] >> (lane * bits % 64)) & SatAddSubMask(bits);
}
void SatAddSubPut(SatAddSubVector& v, u32 bits, u32 lane, u64 value) {
    const u32 word = lane * bits / 64, shift = lane * bits % 64;
    const auto mask = SatAddSubMask(bits);
    v[word] = (v[word] & ~(mask << shift)) | ((value & mask) << shift);
}
SatAddSubVector SatAddSubBigInt(SatAddSubVector a, SatAddSubVector b, u32 bits, bool sign, bool subtract) {
    using Big = boost::multiprecision::cpp_int;
    const Big modulus = Big{1} << bits;
    Big minimum = 0, maximum = modulus - 1;
    if (sign) { minimum = -(Big{1} << (bits - 1)); maximum = (Big{1} << (bits - 1)) - 1; }
    SatAddSubVector result{};
    for (u32 lane = 0; lane < 128 / bits; ++lane) {
        Big left = SatAddSubLane(a, bits, lane), right = SatAddSubLane(b, bits, lane);
        if (sign && left > maximum) left -= modulus;
        if (sign && right > maximum) right -= modulus;
        Big value = left;
        if (subtract) value -= right; else value += right;
        if (value < minimum) value = minimum;
        if (value > maximum) value = maximum;
        if (value < 0) value += modulus;
        SatAddSubPut(result, bits, lane, value.convert_to<u64>());
    }
    return result;
}
ir::Value SatAddSubEmit(ir::Assembler& as, ir::Value a, ir::Value b, u32 bits, bool sign, bool subtract) {
    return (subtract ? as.VecSatSub(a, b, ir::Imm{bits}, ir::Imm{u32(sign)}) :
                       as.VecSatAdd(a, b, ir::Imm{bits}, ir::Imm{u32(sign)})).SetType(ValueType::V128);
}
void SatAddSubBigIntBoundariesForwardingAndPressure() {
    u64 random = 0xac741139e86c570aULL;
    for (u32 bits : {8u, 16u, 32u, 64u}) for (bool sign : {false, true})
    for (bool subtract : {false, true}) for (u32 layout = 0; layout < 5; ++layout) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12b00})}; ir::Assembler as{block.get()};
        std::vector<ir::Value> live;
        if (layout == 4) for (u32 i = 0; i < 24; ++i)
            live.push_back(as.LoadUniform(ir::Uniform{256 + i * 16, ValueType::V128}).SetType(ValueType::V128));
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto pattern = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
        auto left = layout == 3 ? as.VecXor(a, pattern).SetType(ValueType::V128) : a;
        auto value = SatAddSubEmit(as, left, layout == 2 ? left : b, bits, sign, subtract);
        if (layout == 3) value = as.VecXor(value, pattern).SetType(ValueType::V128);
        const u32 destination = layout == 1 ? 0 : 64;
        as.StoreUniform(ir::Uniform{destination, ValueType::V128}, value);
        if (layout != 3) {
            as.StoreUniform(ir::Uniform{96, ValueType::V128}, a);
            as.StoreUniform(ir::Uniform{112, ValueType::V128}, b);
        }
        for (u32 i = 0; i < live.size(); ++i) as.StoreUniform(ir::Uniform{1024 + i * 16, ValueType::V128}, live[i]);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const u64 mask = SatAddSubMask(bits), sign_bit = u64{1} << (bits - 1);
        const std::array<u64, 12> boundaries{0, 1, 2, sign_bit - 2, sign_bit - 1, sign_bit,
                                           sign_bit + 1, sign_bit + 2, mask - 2, mask - 1, mask, mask / 3};
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 512; ++sample) {
                SatAddSubVector a{}, b{}, pattern{Random(random), Random(random)};
                for (u32 lane = 0; lane < 128 / bits; ++lane) {
                    const auto left = sample < 144 ? boundaries[(sample / 12 + lane) % 12] : Random(random);
                    const auto right = sample < 144 ? boundaries[(sample % 12 + lane * 5) % 12] : Random(random);
                    SatAddSubPut(a, bits, lane, left); SatAddSubPut(b, bits, lane, right);
                }
                auto left = a;
                if (layout == 3) for (u32 part = 0; part < 2; ++part) left[part] ^= pattern[part];
                auto wanted = SatAddSubBigInt(left, layout == 2 ? left : b, bits, sign, subtract);
                if (layout == 3) for (u32 part = 0; part < 2; ++part) wanted[part] ^= pattern[part];
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    for (u32 byte = 0; byte < 2048; ++byte) storage->state->uniform_buffer_begin[byte] = u8(byte * 11 + sample);
                    std::memcpy(storage->state->uniform_buffer_begin, a.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 16, b.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 32, pattern.data(), 16);
                }
                const auto flags = Random(random); actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "saturating add/sub halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + destination, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + destination, wanted.data(), 16), "saturating add/sub independent BigInt lanes");
                Check(!std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 2048) &&
                      (layout == 3 || (!std::memcmp(actual.state->uniform_buffer_begin + 96, a.data(), 16) &&
                                      !std::memcmp(actual.state->uniform_buffer_begin + 112, b.data(), 16))), "saturating add/sub aliases and pressure snapshot");
                Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr,
                      "saturating add/sub preserves guest flags and MXCSR");
            }
        }
    }
    std::cout << "PASS saturating add/sub BigInt boundaries, aliases, forwarding and pressure\n";
}
void SatAddSubAllBytePairs() {
    for (bool sign : {false, true}) for (bool subtract : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12c00})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, SatAddSubEmit(as, a, b, 8, sign, subtract));
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get(), x64::HostFeatures{}};
        for (u32 left = 0; left < 256; ++left) for (u32 first_right = 0; first_right < 256; first_right += 16) {
            SatAddSubVector a{}, b{};
            for (u32 lane = 0; lane < 16; ++lane) { SatAddSubPut(a, 8, lane, left); SatAddSubPut(b, 8, lane, first_right + lane); }
            const auto wanted = SatAddSubBigInt(a, b, 8, sign, subtract); Storage state;
            std::memcpy(state.state->uniform_buffer_begin, a.data(), 16);
            std::memcpy(state.state->uniform_buffer_begin + 16, b.data(), 16);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "saturating add/sub exhaustive byte halt");
            Check(!std::memcmp(state.state->uniform_buffer_begin + 64, wanted.data(), 16), "all byte pairs saturating add/sub BigInt");
        }
    }
}
void SatAddSubMalformedWholeBlock() {
    for (bool subtract : {false, true}) for (bool skipped : {false, true}) for (u32 fault = 0; fault < 17; ++fault) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12d00})}; ir::Assembler as{block.get()};
        auto source = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128); ir::Value skip;
        if (skipped) skip = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(ValueType::U8)});
        auto result = SatAddSubEmit(as, source, source, 32, false, subtract);
        if (fault < 5) result.Def()->SetArg(2, ir::Imm{u64(fault == 0 ? 0 : fault == 1 ? 24 : fault == 2 ? UINT64_MAX :
                                                        fault == 3 ? 0x100000020ULL : 0x100000040ULL)});
        if (fault >= 5 && fault < 9) result.Def()->SetArg(3, ir::Imm{u64(fault == 5 ? 2 : fault == 6 ? UINT64_MAX :
                                                                      fault == 7 ? 0x100000000ULL : 0x100000001ULL)});
        if (fault == 9) result.Def()->SetArg(2, source);
        if (fault == 10) result.Def()->SetArg(3, source);
        if (fault == 11) {
            result.Def()->SetArg(0, ir::Void{nullptr});
            result.Def()->ArgAt(0) = ir::Value{};
        }
        if (fault == 12) result.Def()->SetArg(1, ir::Imm{0u});
        if (fault == 13) result.Def()->SetArg(0, source.SetCastType(ValueType::V256));
        if (fault == 14) source.SetType(ValueType::VOID);
        if (fault == 15) source.SetType(ValueType::V256);
        if (fault == 16) result.SetType(ValueType::VOID);
        if (skipped) as.BindLabel(skip);
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool native_rejected{}, interpreter_rejected{};
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { native_rejected = true; }
        Storage state;
        try { backend::interp::Interpreter reference{*state.state, block.get()}; interpreter_rejected = reference.Run() == HaltReason::IllegalCode; }
        catch (const std::runtime_error&) { interpreter_rejected = true; }
        if (fault == 11) result.Def()->ArgAt(0) = ir::Void{nullptr};
        Check(native_rejected && interpreter_rejected, "saturating add/sub whole-block full immediate/type validation");
    }
}

void SatAddSubRawContainerViews() {
    // Every annotation/view/result combination still computes all four
    // dwords or both qwords, including bytes beyond a narrow annotation.
    u64 random = 0x742fec159d130ecaULL;
    for (auto actual_type : ContainerTypes) for (auto view_type : ContainerTypes) for (auto result_type : ContainerTypes)
    for (u32 bits : {32u, 64u}) for (bool sign : {false, true}) for (bool subtract : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12d80})}; ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto other = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto producer = as.VecXor(input, other).SetType(actual_type);
        auto value = SatAddSubEmit(as, producer.SetCastType(view_type), other.SetCastType(view_type), bits, sign, subtract).SetType(result_type);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, value.SetCastType(ValueType::V128));
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get(), x64::HostFeatures{}};
        for (u32 sample = 0; sample < 2; ++sample) {
            SatAddSubVector input{Random(random), Random(random)}, other{Random(random), Random(random)}, source{};
            for (u32 part = 0; part < 2; ++part) source[part] = input[part] ^ other[part];
            const auto wanted = SatAddSubBigInt(source, other, bits, sign, subtract);
            Storage actual, expected;
            for (auto* storage : {&actual, &expected}) {
                std::memcpy(storage->state->uniform_buffer_begin, input.data(), 16);
                std::memcpy(storage->state->uniform_buffer_begin + 16, other.data(), 16);
            }
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "saturating add/sub raw view halt");
            Check(!std::memcmp(actual.state->uniform_buffer_begin + 64, wanted.data(), 16) &&
                  !std::memcmp(expected.state->uniform_buffer_begin + 64, wanted.data(), 16), "saturating add/sub narrow actual/view/result retain complete container");
        }
    }
    for (u32 bits : {32u, 64u}) for (bool sign : {false, true}) for (bool subtract : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12da0})}; ir::Assembler as{block.get()};
        auto scalar = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto other = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto value = SatAddSubEmit(as, scalar.SetCastType(ValueType::V128), other, bits, sign, subtract);
        as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get(), x64::HostFeatures{}};
        Storage actual, expected; const SatAddSubVector source{UINT64_MAX, 0}, right{1, UINT64_MAX};
        for (auto* storage : {&actual, &expected}) {
            std::memcpy(storage->state->uniform_buffer_begin, source.data(), 16);
            std::memcpy(storage->state->uniform_buffer_begin + 16, right.data(), 16);
        }
        const auto wanted = SatAddSubBigInt(source, right, bits, sign, subtract);
        backend::interp::Interpreter reference{*expected.state, block.get()};
        Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "saturating add/sub scalar home halt");
        Check(!std::memcmp(actual.state->uniform_buffer_begin + 64, wanted.data(), 16) &&
              !std::memcmp(expected.state->uniform_buffer_begin + 64, wanted.data(), 16), "saturating add/sub initialized scalar high container");
    }
}
