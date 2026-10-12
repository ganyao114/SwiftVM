using MulPackedVector = std::array<u64, 2>;
u64 MulPackedMask(u32 bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
u64 MulPackedLane(MulPackedVector v, u32 bits, u32 lane) { return (v[lane * bits / 64] >> (lane * bits % 64)) & MulPackedMask(bits); }
void MulPackedPut(MulPackedVector& v, u32 bits, u32 lane, u64 value) {
    const u32 index = lane * bits / 64, shift = lane * bits % 64;
    const u64 mask = MulPackedMask(bits);
    v[index] = (v[index] & ~(mask << shift)) | ((value & mask) << shift);
}
MulPackedVector MulPackedIndependent(MulPackedVector a, MulPackedVector b, u32 bits) {
    MulPackedVector result{};
    for (u32 lane = 0; lane < 128 / bits; ++lane)
        MulPackedPut(result, bits, lane, MulPackedLane(a, bits, lane) * MulPackedLane(b, bits, lane));
    return result;
}
ir::Value MulPackedEmit(ir::Assembler& as, ir::Value a, ir::Value b, u32 bits, bool four) {
    return four ? as.Vec4Mul(a, b).SetType(ValueType::V128) : as.VecMul(a, b, ir::Imm{bits}).SetType(ValueType::V128);
}
void MulPackedAllBytesAndDwordBoundaries() {
    constexpr std::array<u32, 16> edges{0, 1, 2, 255, 256, 65535, 65536, 0x7fffffff, 0x80000000,
        0x80000001, 0xfffffffe, 0xffffffff, 0x55555555, 0xaaaaaaaa, 0x00010001, 0xffff0001};
    u64 random = 0x927024783bc11aefULL;
    for (u32 route = 0; route < 3; ++route) for (u32 layout = 0; layout < 4; ++layout) {
        const u32 bits = route == 0 ? 8 : 32; const bool four = route == 2;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12300})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto value = MulPackedEmit(as, a, layout == 3 ? a : b, bits, four);
        const u32 destination = layout < 2 ? layout * 16 : 64;
        as.StoreUniform(ir::Uniform{destination, ValueType::V128}, value);
        as.StoreUniform(ir::Uniform{96, ValueType::V128}, a); as.StoreUniform(ir::Uniform{112, ValueType::V128}, b);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            // Consecutive groups of sixteen cover all 256^2 ordered byte
            // pairs distributed across lane positions, with distinct lanes.
            const u32 directed = bits == 8 ? 4096 : edges.size() * edges.size(), samples = directed + 256;
            for (u32 sample = 0; sample < samples; ++sample) {
                MulPackedVector a{}, b{};
                if (sample < directed) for (u32 lane = 0; lane < 128 / bits; ++lane) {
                    if (bits == 8) {
                        const u32 pair = sample * 16 + lane;
                        MulPackedPut(a, bits, lane, pair >> 8); MulPackedPut(b, bits, lane, pair & 255);
                    } else {
                        MulPackedPut(a, bits, lane, edges[(sample / edges.size() + lane) % edges.size()]);
                        MulPackedPut(b, bits, lane, edges[(sample % edges.size() + lane * 3) % edges.size()]);
                    }
                } else { a = {Random(random), Random(random)}; b = {Random(random), Random(random)}; }
                const auto wanted = MulPackedIndependent(a, layout == 3 ? a : b, bits);
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    std::memcpy(storage->state->uniform_buffer_begin, a.data(), 16);
                    std::memcpy(storage->state->uniform_buffer_begin + 16, b.data(), 16);
                }
                const u64 flags = Random(random); actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                const std::string label = "mulpacked route=" + std::to_string(route) + "/layout=" + std::to_string(layout) + "/sample=" + std::to_string(sample);
                Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, label + " halt");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + destination, wanted.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + destination, wanted.data(), 16), label + " independent modulo product");
                Check(!std::memcmp(actual.state->uniform_buffer_begin + 96, a.data(), 16) &&
                      !std::memcmp(actual.state->uniform_buffer_begin + 112, b.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 96, a.data(), 16) &&
                      !std::memcmp(expected.state->uniform_buffer_begin + 112, b.data(), 16), label + " retained SSA originals");
                Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr,
                      label + " flags and MXCSR");
            }
        }
    }
    std::cout << "PASS packed byte products every pair, dword boundaries, random, aliases and flags\n";
}
void MulPackedForwardingAndPressure() {
    u64 random = 0xbc1881f975a26409ULL;
    for (u32 bits : {8u, 16u, 32u, 64u}) for (bool four : {false, true}) {
        if (four && bits != 32) continue;
        for (u32 chain = 0; chain < 5; ++chain) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12400})}; ir::Assembler as{block.get()};
            std::vector<ir::Value> keep;
            if (chain == 4) for (u32 index = 0; index < 32; ++index)
                keep.push_back(as.LoadUniform(ir::Uniform{256 + index * 16, ValueType::V128}).SetType(ValueType::V128));
            auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
            auto b = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
            auto first = chain == 0 || chain == 2 ? as.VecXor(a, b).SetType(ValueType::V128) : a;
            auto value = MulPackedEmit(as, first, chain == 2 ? first : b, bits, four);
            if (chain == 1) value = MulPackedEmit(as, value, b, bits, four);
            auto final = as.VecXor(value, b).SetType(ValueType::V128);
            as.StoreUniform(ir::Uniform{64, ValueType::V128}, final);
            if (chain >= 3) {
                as.StoreUniform(ir::Uniform{96, ValueType::V128}, value);
                as.StoreUniform(ir::Uniform{112, ValueType::V128}, a);
            }
            if (chain == 4) for (u32 index = 0; index < keep.size(); ++index)
                as.StoreUniform(ir::Uniform{1024 + index * 16, ValueType::V128}, keep[index]);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
                Compiled compiled{block.get(), features};
                for (u32 sample = 0; sample < 128; ++sample) {
                    MulPackedVector a{Random(random), Random(random)}, b{Random(random), Random(random)};
                    MulPackedVector first = chain == 0 || chain == 2 ? MulPackedVector{a[0] ^ b[0], a[1] ^ b[1]} : a;
                    auto product = MulPackedIndependent(first, chain == 2 ? first : b, bits);
                    if (chain == 1) product = MulPackedIndependent(product, b, bits);
                    const MulPackedVector wanted{product[0] ^ b[0], product[1] ^ b[1]}; Storage actual, expected;
                    for (auto* storage : {&actual, &expected}) {
                        for (u32 byte = 0; byte < 2048; ++byte) storage->state->uniform_buffer_begin[byte] = u8(byte * 29 + sample);
                        std::memcpy(storage->state->uniform_buffer_begin, a.data(), 16);
                        std::memcpy(storage->state->uniform_buffer_begin + 16, b.data(), 16);
                    }
                    const u64 flags = Random(random); actual.state->host_cpu_flags = expected.state->host_cpu_flags = flags;
                    const auto mxcsr = _mm_getcsr(); backend::interp::Interpreter reference{*expected.state, block.get()};
                    Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "mulpacked forwarding halt");
                    Check(!std::memcmp(actual.state->uniform_buffer_begin + 64, wanted.data(), 16) &&
                          !std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 2048), "mulpacked independent forward result and pressure snapshot");
                    Check(chain < 3 || (!std::memcmp(actual.state->uniform_buffer_begin + 96, product.data(), 16) &&
                          !std::memcmp(actual.state->uniform_buffer_begin + 112, a.data(), 16)), "mulpacked delayed multiple users");
                    Check(actual.state->host_cpu_flags == flags && expected.state->host_cpu_flags == flags && _mm_getcsr() == mxcsr, "mulpacked flags and MXCSR");
                }
            }
        }
    }
    std::cout << "PASS packed multiply forwarding, delayed consumers, pressure and remaining qword scalar path\n";
}
void MulPackedWidthValidation() {
    for (bool skipped : {false, true}) for (u32 corruption = 0; corruption < 11; ++corruption) {
        constexpr std::array<u64, 9> invalid{0, 1, 7, 65, UINT64_MAX, 0x100000008ULL, 0x100000010ULL, 0x100000020ULL, 0x100000040ULL};
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x12500})}; ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128); ir::Value skip;
        if (skipped) skip = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(ValueType::U8)});
        auto value = as.VecMul(a, a, ir::Imm{corruption < 9 ? invalid[corruption] : u64{32}}).SetType(ValueType::V128);
        if (corruption == 9) value.Def()->SetArg(2, a);
        if (corruption == 10) value.Def()->SetArg(2, ir::Uniform{0, ValueType::U32});
        if (skipped) as.BindLabel(skip);
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool rejected{};
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "packed multiply full immediate and branch-skipped validation");
    }
    std::cout << "PASS vector multiply complete lane immediate validation\n";
}
