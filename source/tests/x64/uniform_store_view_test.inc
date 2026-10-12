// Included inside backend_test.cpp's fixture namespace. The descriptor
// contributes an offset; the value view determines bytes written.
u32 UniformStoreOracleSize(ValueType type) {
    switch (type) {
        case ValueType::U8: case ValueType::S8: case ValueType::V8: return 1;
        case ValueType::U16: case ValueType::S16: case ValueType::V16: return 2;
        case ValueType::U32: case ValueType::S32: case ValueType::V32: return 4;
        case ValueType::U64: case ValueType::S64: case ValueType::V64: return 8;
        case ValueType::V128: return 16;
        default: throw std::runtime_error("uniform-store oracle requires a supported value width");
    }
}
void SeedUniformStoreRegion(Storage& storage, std::array<u8, 64>& oracle) {
    for (u32 byte = 0; byte < oracle.size(); ++byte)
        storage.state->uniform_buffer_begin[128 + byte] = oracle[byte] = u8(0xa5 ^ (byte * 17));
    storage.state->host_cpu_flags = 0xdbad1234567809abULL;
}

void UniformStoreScalarViewsAndNeighbors() {
    constexpr std::array<ValueType, 8> scalar{ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                             ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64};
    constexpr std::array<ValueType, 9> descriptors{ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64,
                                                  ValueType::V8, ValueType::V16, ValueType::V32, ValueType::V64, ValueType::V128};
    for (auto producer_type : scalar) for (auto view_type : scalar) for (auto descriptor : descriptors) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe600})}; ir::Assembler as{block.get()};
        auto producer = as.LoadUniform(ir::Uniform{0, producer_type}).SetType(producer_type);
        as.StoreUniform(ir::Uniform{133, descriptor}, ir::Value{producer}.SetCastType(view_type));
        // Keep the original definition live after the store, so input-view
        // truncation must not change its canonical producer bits.
        as.StoreUniform(ir::Uniform{224, ValueType::U64}, as.ZeroExtend64(producer).SetType(ValueType::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()};
        const u32 producer_bytes = UniformStoreOracleSize(producer_type), view_bytes = UniformStoreOracleSize(view_type);
        u64 seed = 0x672104bfceaa8719ULL;
        for (u32 sample = 0; sample < 32; ++sample) {
            const u64 input = sample == 0 ? UINT64_MAX : sample == 1 ? 0x8000000000000000ULL : Random(seed);
            Storage actual, expected; std::array<u8, 64> oracle{}, ignored{};
            SeedUniformStoreRegion(actual, oracle); SeedUniformStoreRegion(expected, ignored);
            actual.Put(0, input); expected.Put(0, input);
            for (u32 byte = 0; byte < view_bytes; ++byte)
                oracle[5 + byte] = byte < producer_bytes ? u8(input >> (byte * 8)) : 0;
            const u64 canonical = producer_bytes == 8 ? input : input & ((u64{1} << (producer_bytes * 8)) - 1);
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "scalar StoreUniform view/descriptor halt");
            Check(!std::memcmp(actual.state->uniform_buffer_begin + 128, oracle.data(), oracle.size()) &&
                  !std::memcmp(expected.state->uniform_buffer_begin + 128, oracle.data(), oracle.size()),
                  "scalar StoreUniform writes only value-view bytes producer=" + std::to_string(u32(producer_type)) +
                  "/view=" + std::to_string(u32(view_type)) + "/descriptor=" + std::to_string(u32(descriptor)));
            Check(actual.Get(224) == canonical && expected.Get(224) == canonical &&
                  actual.state->host_cpu_flags == 0xdbad1234567809abULL && expected.state->host_cpu_flags == 0xdbad1234567809abULL,
                  "scalar StoreUniform preserves producer bits, guest flags and adjacent bytes");
        }
    }
    std::cout << "PASS scalar StoreUniform descriptor/view mismatches / signed bits / widening+narrowing / neighbors\n";
}

void UniformStoreVectorViewsAndNeighbors() {
    constexpr std::array<ValueType, 5> vector{ValueType::V8, ValueType::V16, ValueType::V32, ValueType::V64, ValueType::V128};
    constexpr std::array<ValueType, 9> descriptors{ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64,
                                                  ValueType::V8, ValueType::V16, ValueType::V32, ValueType::V64, ValueType::V128};
    for (auto producer_type : vector) for (auto view_type : vector) for (auto descriptor : descriptors) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe700})}; ir::Assembler as{block.get()};
        auto producer = as.LoadUniform(ir::Uniform{0, producer_type}).SetType(producer_type);
        as.StoreUniform(ir::Uniform{131, descriptor}, ir::Value{producer}.SetCastType(view_type));
        as.StoreUniform(ir::Uniform{224, producer_type}, producer);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()};
        const u32 producer_bytes = UniformStoreOracleSize(producer_type), view_bytes = UniformStoreOracleSize(view_type);
        u64 seed = 0x8154653b901ffeacULL;
        for (u32 sample = 0; sample < 16; ++sample) {
            std::array<u8, 16> input{};
            for (u32 byte = 0; byte < input.size(); ++byte) input[byte] = sample == 0 ? 0xff : u8(Random(seed));
            Storage actual, expected; std::array<u8, 64> oracle{}, ignored{};
            SeedUniformStoreRegion(actual, oracle); SeedUniformStoreRegion(expected, ignored);
            std::memcpy(actual.state->uniform_buffer_begin, input.data(), input.size());
            std::memcpy(expected.state->uniform_buffer_begin, input.data(), input.size());
            for (u32 byte = 0; byte < view_bytes; ++byte) oracle[3 + byte] = byte < producer_bytes ? input[byte] : 0;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "vector StoreUniform view/descriptor halt");
            Check(!std::memcmp(actual.state->uniform_buffer_begin + 128, oracle.data(), oracle.size()) &&
                  !std::memcmp(expected.state->uniform_buffer_begin + 128, oracle.data(), oracle.size()),
                  "vector StoreUniform writes only value-view bytes producer=" + std::to_string(u32(producer_type)) +
                  "/view=" + std::to_string(u32(view_type)) + "/descriptor=" + std::to_string(u32(descriptor)));
            Check(!std::memcmp(actual.state->uniform_buffer_begin + 224, input.data(), producer_bytes) &&
                  !std::memcmp(expected.state->uniform_buffer_begin + 224, input.data(), producer_bytes) &&
                  actual.state->host_cpu_flags == 0xdbad1234567809abULL && expected.state->host_cpu_flags == 0xdbad1234567809abULL,
                  "vector StoreUniform preserves live producer bits and guest flags");
        }
    }
    std::cout << "PASS vector StoreUniform descriptor/view mismatches / widening+narrowing / neighbors\n";
}

void UniformStoreSetTypeRetainsInputView() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe800})}; ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto typed = as.ZeroExtend32(input).SetType(ValueType::U64);
    Check(typed.Def()->ReturnType() == ValueType::U64 && typed.Type() == ValueType::U32, "SetType retains a typed wrapper's existing cast");
    as.StoreUniform(ir::Uniform{133, ValueType::U64}, typed);
    auto wide = as.ZeroExtend32To64(input).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{149, ValueType::U8}, wide);
    block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled compiled{block.get()};
    u64 seed = 0x16275baaff640912ULL;
    for (u32 sample = 0; sample < 64; ++sample) {
        Storage actual, expected; std::array<u8, 64> oracle{}, ignored{};
        SeedUniformStoreRegion(actual, oracle); SeedUniformStoreRegion(expected, ignored);
        const u64 value = sample == 0 ? UINT64_MAX : Random(seed); actual.Put(0, value); expected.Put(0, value);
        for (u32 byte = 0; byte < 4; ++byte) oracle[5 + byte] = u8(value >> (byte * 8));
        for (u32 byte = 0; byte < 8; ++byte) oracle[21 + byte] = byte < 4 ? u8(value >> (byte * 8)) : 0;
        backend::interp::Interpreter reference{*expected.state, block.get()};
        Check(compiled.fn(actual.state) == HaltReason::CallHost && reference.Run() == HaltReason::CallHost, "typed-wrapper StoreUniform halt");
        Check(!std::memcmp(actual.state->uniform_buffer_begin + 128, oracle.data(), oracle.size()) &&
              !std::memcmp(expected.state->uniform_buffer_begin + 128, oracle.data(), oracle.size()),
              "SetType(U64) alone preserves U32 store view; explicit U64 extension stores eight bytes");
        Check(actual.state->host_cpu_flags == 0xdbad1234567809abULL && expected.state->host_cpu_flags == 0xdbad1234567809abULL,
              "typed-wrapper StoreUniform preserves guest flags");
    }
    std::cout << "PASS StoreUniform retained TypedValue cast / explicit U64 extension / poison neighbors\n";
}
