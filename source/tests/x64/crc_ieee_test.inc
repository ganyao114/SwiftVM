// Included in backend_test.cpp's fixture namespace after the CRC32C tests.
// Forward polynomial division with explicit reflection is independent of
// the runtime's reflected byte table and ARM CRC instructions.
u32 ReferenceIeeeForward(u32 initial, u64 data, u32 width) {
    const auto reverse = [](u32 value) {
        u32 result{};
        for (u32 bit = 0; bit < 32; ++bit) result |= ((value >> bit) & 1u) << (31 - bit);
        return result;
    };
    u32 value = reverse(initial);
    for (u32 bit = 0; bit < width; ++bit) {
        const bool top = ((value >> 31) ^ (data >> bit)) & 1;
        value <<= 1;
        if (top) value ^= 0x04C11DB7u;
    }
    return reverse(value);
}

void IeeeCrcEveryWidthAndAliases() {
    for (u32 width : {8u, 16u, 32u, 64u}) for (bool alias : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xde00})}; ir::Assembler as{block.get()};
        auto initial = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto data = alias ? initial : as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        auto value = as.Crc32(initial, data, ir::Imm{width}).SetType(ValueType::U32);
        as.StoreUniform(ir::Uniform{16, ValueType::U64}, as.ZeroExtend32To64(value).SetType(ValueType::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled automatic{block.get()}, baseline{block.get(), x64::HostFeatures{}};
        u64 seed = 0xbfee021097563824ULL;
        for (u32 sample = 0; sample < 512; ++sample) {
            const u64 initial_value = sample < 4 ? 0xDEADBEEF00000000ULL | (sample & 1 ? UINT32_MAX : 0) : Random(seed);
            const u64 data_value = sample < 256 ? sample * 0x0101010101010101ULL : Random(seed);
            Storage actual, portable, expected;
            for (auto* storage : {&actual, &portable, &expected}) {
                storage->Put(0, initial_value); storage->Put(8, data_value);
                storage->Put(16, UINT64_MAX);
                storage->state->host_cpu_flags = 0xdabcdef012345678ULL;
            }
            backend::interp::Interpreter reference{*expected.state, block.get()}; const auto halt = reference.Run();
            const auto mxcsr = _mm_getcsr();
            Check(automatic.fn(actual.state) == halt && baseline.fn(portable.state) == halt, "IEEE CRC both feature settings halt");
            const u64 independent = ReferenceIeeeForward(u32(initial_value), alias ? initial_value : data_value, width);
            Check(actual.Get(16) == independent && portable.Get(16) == independent, "IEEE CRC independent forward polynomial and upper zeroing");
            Check(actual.Get(16) == expected.Get(16), "IEEE CRC interpreter equality width=" + std::to_string(width) +
                  "/alias=" + std::to_string(alias) + "/sample=" + std::to_string(sample) +
                  "/native=" + std::to_string(actual.Get(16)) + "/interpreter=" + std::to_string(expected.Get(16)));
            Check(actual.state->host_cpu_flags == 0xdabcdef012345678ULL && portable.state->host_cpu_flags == 0xdabcdef012345678ULL &&
                  _mm_getcsr() == mxcsr, "IEEE CRC preserves guest flags and host FP state");
        }
    }
    u32 known = UINT32_MAX;
    for (u8 byte : std::array<u8, 9>{'1','2','3','4','5','6','7','8','9'}) known = ReferenceIeeeForward(known, byte, 8);
    Check((known ^ UINT32_MAX) == 0xCBF43926u, "IEEE CRC standard check value");
    std::cout << "PASS IEEE CRC every width / high-bit ignoring / aliases / table feature settings\n";
}

void IeeeCrcByteOrderAndKnownValue() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xdf00})}; ir::Assembler as{block.get()};
    auto initial = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto data = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    auto wide = as.Crc32(initial, data, ir::Imm{u32{64}}).SetType(ValueType::U32);
    ir::Value narrow = initial;
    for (u32 byte = 0; byte < 8; ++byte) {
        auto next = as.LsrImm(data, ir::Imm{byte * 8}).SetType(ValueType::U64);
        narrow = as.Crc32(narrow, next, ir::Imm{u32{8}}).SetType(ValueType::U32);
    }
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, as.ZeroExtend32To64(wide).SetType(ValueType::U64));
    as.StoreUniform(ir::Uniform{24, ValueType::U64}, as.ZeroExtend32To64(narrow).SetType(ValueType::U64));
    auto known = as.Crc32(as.LoadImm(ir::Imm{u32{UINT32_MAX}}).SetType(ValueType::U32),
                         as.LoadImm(ir::Imm{u64{0x3837363534333231ULL}}).SetType(ValueType::U64), ir::Imm{u32{64}}).SetType(ValueType::U32);
    known = as.Crc32(known, as.LoadImm(ir::Imm{u32{'9'}}).SetType(ValueType::U32), ir::Imm{u32{8}}).SetType(ValueType::U32);
    as.StoreUniform(ir::Uniform{32, ValueType::U64}, as.ZeroExtend32To64(known).SetType(ValueType::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()};
    u64 seed = 0xf081ace784436592ULL;
    for (u32 sample = 0; sample < 512; ++sample) {
        Storage actual; const u64 accumulator = Random(seed), value = Random(seed);
        actual.Put(0, accumulator); actual.Put(8, value);
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "IEEE CRC wide/byte chain halt");
        Check(actual.Get(16) == actual.Get(24) && actual.Get(16) == ReferenceIeeeForward(u32(accumulator), value, 64),
              "IEEE CRC64 consumes bytes from least significant to most significant");
        Check((actual.Get(32) ^ UINT32_MAX) == 0xCBF43926u, "IEEE CRC emitted standard check value");
    }
    std::cout << "PASS IEEE CRC64 / eight CRC8 chain / emitted standard check value\n";
}

void IeeeCrcScalarViews() {
    constexpr std::array<ValueType, 8> views{ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                            ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64};
    for (u32 width : {8u, 16u, 32u, 64u}) for (u32 view = 0; view < views.size(); ++view)
    for (bool narrow_producer : {false, true}) for (bool alias : {false, true}) {
        const auto acc_type = narrow_producer ? ValueType::U16 : ValueType::U64;
        const auto data_type = narrow_producer ? ValueType::U8 : ValueType::U64;
        const auto acc_view = views[view], data_view = views[(view + 3) % views.size()];
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe100})}; ir::Assembler as{block.get()};
        auto acc = as.LoadUniform(ir::Uniform{0, acc_type}).SetType(acc_type);
        auto data = alias ? acc : as.LoadUniform(ir::Uniform{8, data_type}).SetType(data_type);
        auto value = as.Crc32(ir::Value{acc}.SetCastType(acc_view), ir::Value{data}.SetCastType(data_view), ir::Imm{width}).SetType(ValueType::U32);
        as.StoreUniform(ir::Uniform{16, ValueType::U64}, as.ZeroExtend32To64(value).SetType(ValueType::U64));
        // Keep the uncast definitions live to exercise independent input views.
        as.StoreUniform(ir::Uniform{24, ValueType::U64}, as.ZeroExtend64(acc).SetType(ValueType::U64));
        as.StoreUniform(ir::Uniform{32, ValueType::U64}, as.ZeroExtend64(data).SetType(ValueType::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto truncate = [](u64 value, ValueType producer, ValueType view_type) {
            const u32 bits = std::min(ir::GetValueSizeByte(producer), ir::GetValueSizeByte(view_type)) * 8;
            return value & (bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1);
        };
        u64 seed = 0xc0826e734912fd77ULL;
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 32; ++sample) {
                const u64 a = sample == 0 ? UINT64_MAX : Random(seed), b = sample == 0 ? UINT64_MAX : Random(seed);
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    storage->Put(0, a); storage->Put(8, b);
                    storage->state->host_cpu_flags = 0x9a5ef010ba781234ULL;
                }
                backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == reference.Run(), "IEEE CRC scalar narrowing/widening view halt");
                Check(actual.Get(16) == ReferenceIeeeForward(u32(truncate(a, acc_type, acc_view)),
                      truncate(alias ? a : b, alias ? acc_type : data_type, data_view), width) && actual.Get(16) == expected.Get(16),
                      "IEEE CRC signed and unsigned views expose min(view,producer) raw bits");
                Check(actual.Get(24) == truncate(a, acc_type, acc_type) &&
                      actual.Get(32) == truncate(alias ? a : b, alias ? acc_type : data_type, alias ? acc_type : data_type),
                      "IEEE CRC input view masking preserves live producer values");
                Check(actual.state->host_cpu_flags == 0x9a5ef010ba781234ULL, "IEEE CRC narrowed inputs preserve guest flags");
            }
        }
    }
    std::cout << "PASS IEEE CRC signed/unsigned scalar views / narrow producers / aliases\n";
}

void IeeeCrcInvalidIR() {
    const auto reject = [](u64 width, ValueType accumulator_type, ValueType data_type, ValueType result_type, u32 mismatched_producer) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe000})}; ir::Assembler as{block.get()};
        auto accumulator = as.LoadImm(ir::Imm{u64{1}}).SetType(accumulator_type);
        auto data = as.LoadImm(ir::Imm{u64{2}}).SetType(data_type);
        if (mismatched_producer == 1) { accumulator.Def()->SetReturn(ValueType::V128); accumulator = accumulator.SetCastType(ValueType::U64); }
        if (mismatched_producer == 2) { data.Def()->SetReturn(ValueType::VOID); data = data.SetCastType(ValueType::U64); }
        auto value = as.Crc32(accumulator, data, ir::Imm{width}).SetType(result_type);
        if (mismatched_producer == 3) value.Def()->SetArg(2, accumulator);
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool rejected{};
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "IEEE CRC rejects malformed width/input/producer/result before native publication");
    };
    for (u64 width : {u64{0}, u64{1}, u64{7}, u64{24}, u64{128}, UINT64_MAX, (u64{1} << 32) + 8, (u64{1} << 32) + 64})
        reject(width, ValueType::U64, ValueType::U64, ValueType::U32, 0);
    for (auto type : {ValueType::VOID, ValueType::V8, ValueType::V16, ValueType::V32, ValueType::V64, ValueType::V128, ValueType::V256}) {
        reject(8, type, ValueType::U64, ValueType::U32, 0);
        reject(8, ValueType::U64, type, ValueType::U32, 0);
    }
    for (auto type : {ValueType::VOID, ValueType::U8, ValueType::U16, ValueType::U64, ValueType::S32, ValueType::V32, ValueType::V128})
        reject(8, ValueType::U64, ValueType::U64, type, 0);
    for (u32 mismatch = 1; mismatch <= 3; ++mismatch)
        reject(8, ValueType::U64, ValueType::U64, ValueType::U32, mismatch);
    std::cout << "PASS IEEE CRC complete immediate / scalar types / defined producers / malformed arguments\n";
}
