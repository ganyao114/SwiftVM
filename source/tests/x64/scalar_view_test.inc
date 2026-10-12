// Included in backend_test.cpp's fixture namespace.
// A view exposes raw low bits; signed arithmetic uses explicit SignExtend.
u32 ScalarViewOracleBits(ValueType type) {
    switch (type) {
        case ValueType::U8: case ValueType::S8: return 8;
        case ValueType::U16: case ValueType::S16: return 16;
        case ValueType::U32: case ValueType::S32: return 32;
        case ValueType::U64: case ValueType::S64: return 64;
        default: throw std::runtime_error("scalar-view oracle requires an integer type");
    }
}
u64 ScalarViewOracleMask(u64 value, u32 bits) {
    return bits == 64 ? value : value & ((u64{1} << bits) - 1);
}
u64 ScalarViewOracle(u64 value, ValueType producer, ValueType view) {
    return ScalarViewOracleMask(value, std::min(ScalarViewOracleBits(producer), ScalarViewOracleBits(view)));
}
u64 ScalarViewOracleSignExtend(u64 value, u32 bits) {
    value = ScalarViewOracleMask(value, bits);
    return bits == 64 || !(value & (u64{1} << (bits - 1))) ? value : value | (UINT64_MAX << bits);
}

void ScalarInputViewsArithmeticAndPredicates() {
    constexpr std::array<ValueType, 8> types{ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                            ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64};
    constexpr std::array<u64, 10> edges{0, 1, 0x80, 0xff, 0x100, 0x10000, 0x80000000,
                                      0x100000000ULL, 0x8000000000000000ULL, UINT64_MAX};
    for (auto producer_type : types) for (auto view_type : types) for (bool alias : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe200})}; ir::Assembler as{block.get()};
        auto original = as.LoadUniform(ir::Uniform{0, producer_type}).SetType(producer_type);
        auto other_original = alias ? original : as.LoadUniform(ir::Uniform{8, producer_type}).SetType(producer_type);
        auto lhs = ir::Value{original}.SetCastType(view_type), rhs = ir::Value{other_original}.SetCastType(view_type);
        const auto store = [&](u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, ValueType::U64}, value); };
        store(64, as.ZeroExtend64(lhs).SetType(ValueType::U64));
        store(72, as.Add(lhs, ir::Operand{rhs}).SetType(ValueType::U64));
        store(80, as.Sub(lhs, ir::Operand{rhs}).SetType(ValueType::U64));
        store(88, as.Mul(lhs, ir::Operand{rhs}).SetType(ValueType::U64));
        store(96, as.And(lhs, ir::Operand{rhs}).SetType(ValueType::U64));
        store(104, as.Or(lhs, ir::Operand{rhs}).SetType(ValueType::U64));
        store(112, as.Xor(lhs, ir::Operand{rhs}).SetType(ValueType::U64));
        store(120, as.ZeroExtend64(as.TestZero(lhs).SetType(ValueType::U8)).SetType(ValueType::U64));
        store(128, as.ZeroExtend64(as.TestNotZero(lhs).SetType(ValueType::U8)).SetType(ValueType::U64));
        store(136, as.SignExtend(lhs).SetType(ValueType::U64));
        store(144, as.BitExtract(lhs, ir::Imm{u32{8}}, ir::Imm{u32{16}}).SetType(ValueType::U64));
        auto true_value = as.LoadImm(ir::Imm{u64{0x1111111111111111ULL}}).SetType(ValueType::U64);
        auto false_value = as.LoadImm(ir::Imm{u64{0x2222222222222222ULL}}).SetType(ValueType::U64);
        store(152, as.Select(ir::BOOL{lhs}, true_value, false_value).SetType(ValueType::U64));
        store(160, as.SelectZero(lhs, true_value, false_value).SetType(ValueType::U64));
        store(168, as.ZeroExtend64(original).SetType(ValueType::U64));
        store(176, as.ZeroExtend64(other_original).SetType(ValueType::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        u64 seed = 0x01674328fecda395ULL;
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 32; ++sample) {
                const u64 input = sample < edges.size() ? edges[sample] : Random(seed);
                const u64 other = sample < edges.size() ? edges[(sample + 3) % edges.size()] : Random(seed);
                const u64 a = ScalarViewOracle(input, producer_type, view_type), b = ScalarViewOracle(alias ? input : other, producer_type, view_type);
                const std::array<u64, 15> independent{a, a + b, a - b, a * b, a & b, a | b, a ^ b, u64(a == 0), u64(a != 0),
                    ScalarViewOracleSignExtend(a, ScalarViewOracleBits(view_type)), (a >> 8) & 0xffff,
                    u8(a) ? 0x1111111111111111ULL : 0x2222222222222222ULL,
                    !a ? 0x1111111111111111ULL : 0x2222222222222222ULL,
                    ScalarViewOracleMask(input, ScalarViewOracleBits(producer_type)),
                    ScalarViewOracleMask(alias ? input : other, ScalarViewOracleBits(producer_type))};
                Storage actual, expected;
                for (auto* storage : {&actual, &expected}) {
                    storage->Put(0, input); storage->Put(8, other); storage->state->host_cpu_flags = 0xd500123456789abcULL;
                }
                backend::interp::Interpreter reference{*expected.state, block.get()};
                Check(compiled.fn(actual.state) == reference.Run(), "scalar view arithmetic/predicate halt");
                for (u32 index = 0; index < independent.size(); ++index)
                    Check(actual.Get(64 + index * 8) == independent[index] && expected.Get(64 + index * 8) == independent[index],
                          "scalar view arithmetic/predicate independent bits producer=" + std::to_string(u32(producer_type)) +
                          "/view=" + std::to_string(u32(view_type)) + "/sample=" + std::to_string(sample) + "/consumer=" + std::to_string(index));
                Check(actual.state->host_cpu_flags == 0xd500123456789abcULL && expected.state->host_cpu_flags == 0xd500123456789abcULL,
                      "scalar view reads preserve guest flags");
            }
        }
    }
    std::cout << "PASS scalar input views / raw signed bits / explicit sign extension / arithmetic+predicate consumers\n";
}

void ScalarInputViewsLocalJumpsAndTerminals() {
    constexpr std::array<ValueType, 8> types{ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                            ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64};
    constexpr std::array<u64, 10> inputs{0, 1, 0xff, 0x100, 0x10000, 0x80000000,
                                       0x100000000ULL, 0x8000000000000000ULL, UINT64_MAX, 0x1234567800ULL};
    for (auto producer_type : types) for (auto view_type : types) for (bool invert : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xe300})}; ir::Assembler as{block.get()};
        auto producer = as.LoadUniform(ir::Uniform{0, producer_type}).SetType(producer_type);
        auto value = ir::Value{producer}.SetCastType(view_type);
        auto jump = invert ? as.NotGoto(ir::BOOL{value}) : as.Goto(ir::BOOL{value});
        as.StoreUniform(ir::Uniform{16, ValueType::U64}, as.LoadImm(ir::Imm{u64{0xabcdef}}).SetType(ValueType::U64));
        as.BindLabel(jump);
        block->SetTerminal(ir::terminal::If{ir::BOOL{value}, ir::terminal::LinkBlock{ir::Location{0xe400}}, ir::terminal::LinkBlock{ir::Location{0xe500}}});
        Compiled compiled{block.get()};
        for (u64 input : inputs) {
            Storage actual, expected;
            for (auto* storage : {&actual, &expected}) { storage->Put(0, input); storage->Put(16, 0); }
            const bool condition = u8(ScalarViewOracle(input, producer_type, view_type)) != 0;
            const bool taken = invert ? !condition : condition;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(compiled.fn(actual.state) == reference.Run(), "scalar view local branch/terminal halt");
            Check(actual.Get(16) == (taken ? 0 : 0xabcdef) && expected.Get(16) == actual.Get(16), "BOOL input view local Goto/NotGoto exposes low byte");
            const auto location = ir::Location{condition ? 0xe400u : 0xe500u};
            Check(actual.state->current_loc == location && expected.state->current_loc == location, "BOOL terminal input view exposes low byte");
        }
    }
    std::cout << "PASS scalar input views / Goto+NotGoto / BOOL terminal consumers\n";
}
