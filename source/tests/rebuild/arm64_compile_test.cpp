#include "runtime/exit.h"
#include "tests/support/arm64_fixture.h"

namespace {
using namespace swift::tests::arm64_fixture;

void SharedContextScalarRegister() {
    using swift::runtime::x86::GuestState;
    for (const auto scope : {Scope::Block, Scope::Function}) {
        Builder builder{scope};
        Need(builder.BeginBlock(Create(builder)));
        std::array<Value, 7> inputs;
        for (unsigned i = 0; i < inputs.size(); ++i)
            inputs[i] = Load(builder, offsetof(GuestState, ymm_high) + i * 8);
        const auto first = Need(builder.Emit<OpCode::Add>(
                Type::U64, {Operand::ValueOf(inputs[6]), Operand::ValueOf(inputs[0])}));
        auto sum = first;
        for (unsigned i = 1; i < 6; ++i)
            sum = Need(builder.Emit<OpCode::Add>(
                    Type::U64, {Operand::ValueOf(sum), Operand::ValueOf(inputs[i])}));
        Need(builder.EndBlock(Terminal::Return(sum)));
        auto program = Submit(builder);
        arm64::CompileOptions options;
        options.pinned_uniform_offset = offsetof(GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.pc_offset = offsetof(GuestState, pc);
        options.flags_offset = offsetof(GuestState, packed_flags);
#if defined(__aarch64__) && defined(__ANDROID__)
        if (NativeMode()) {
            namespace execution = swift::runtime::execution;
            options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatch);
            options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
        }
#else
        options.dispatcher = 0x1000;
        options.exit_handler = 0x2000;
#endif
        const auto allocation = scope == Scope::Block ? arm64::AllocateBlock(program, &options)
                                                      : arm64::AllocateFunction(program, &options);
        Check(bool(allocation) && !allocation.spill_bytes &&
                      allocation.homes[inputs[6].node.value].reg == 28 &&
                      allocation.homes[first.node.value].reg == 28,
              "shared context frees x28 for the seventh scalar and its read-before-write reuse");
        const auto linked = arm64::Compile(program, options);
        Check(bool(linked) && !linked.spill_slots, "linked pin16 retains seven live scalar values");
#if defined(__aarch64__) && defined(__ANDROID__)
        if (NativeMode()) {
            execution::CodeHeap heap{TestBackendOps(),
                                     static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
            auto unit = heap.Publish(linked.words, linked.faults, linked.links);
            std::atomic<const execution::NativeCode*> active{};
            execution::NativeRunContext context;
            context.opaque = &unit;
            context.resolve = [](void* raw, execution::NativeRunContext*) noexcept {
                return reinterpret_cast<std::uintptr_t>(
                        static_cast<execution::NativeCode*>(raw)->entry);
            };
            context.active = &active;
            context.anchor = &unit;
            context.pinned_uniform_offset = options.pinned_uniform_offset;
            context.pinned_uniform_slots = 16;
            context.flags_offset = options.flags_offset;
            for (unsigned i = 0; i < inputs.size(); ++i)
                Write64(reinterpret_cast<std::uint8_t*>(context.state.ymm_high) + i * 8, i + 1);
            context.state.gprs[0] = 0x1234;
            Check(execution::SvmRunNative(&context) == 28 && context.state.gprs[0] == 0x1234 &&
                          context.opaque == &unit && !active.load(),
                  "x28 values execute without corrupting shared context metadata or pinned GPRs");
        }
#endif
        const auto standalone = scope == Scope::Block ? arm64::AllocateBlock(program)
                                                      : arm64::AllocateFunction(program);
        Check(bool(standalone) && std::none_of(standalone.homes.begin(),
                                               standalone.homes.end(),
                                               [](const auto& home) { return home.reg == 28; }),
              "standalone scalar homes retain their original register ABI");
        GuestState state;
        for (unsigned i = 0; i < inputs.size(); ++i)
            Write64(reinterpret_cast<std::uint8_t*>(state.ymm_high) + i * 8, i + 1);
        if (NativeMode()) Execute(Compile(program), reinterpret_cast<std::uint8_t*>(&state), 28);
    }
}

void ArithmeticFlags() {
    struct Case {
        OpCode op;
        unsigned width;
        u64 left, right;
        FlagProfile profile;
        u64 mask;
    };

    const std::array cases{
            Case{OpCode::Add, 64, ~u64{}, 1, FlagProfile::ArithmeticRaw, FlagBits(Flag::All)},
            Case{OpCode::Sub, 64, 0, 1, FlagProfile::ArithmeticRaw, FlagBits(Flag::All)},
            Case{OpCode::Sub, 64, 0, 1, FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::All)},
            Case{OpCode::Add, 32, 0x7fffffff, 1, FlagProfile::ArithmeticRaw, FlagBits(Flag::All)},
            Case{OpCode::Sub,
                 32,
                 0x80000000,
                 1,
                 FlagProfile::ArithmeticCanonicalCarry,
                 FlagBits(Flag::All)},
            Case{OpCode::Neg,
                 64,
                 u64{1} << 63,
                 0,
                 FlagProfile::ArithmeticCanonicalCarry,
                 FlagBits(Flag::All)},
            Case{OpCode::Add, 64, 1, 1, FlagProfile::ArithmeticRaw, FlagBits(Flag::Zero)},
            Case{OpCode::Add,
                 8,
                 0x7f,
                 1,
                 FlagProfile::ArithmeticCanonicalCarry,
                 FlagBits(Flag::All)},
            Case{OpCode::Sub, 16, 0, 1, FlagProfile::ArithmeticCanonicalCarry, FlagBits(Flag::All)},
    };
    constexpr u64 initial = 0xa5a50000bc00003cULL;
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    std::array<u64, cases.size()> expected_flags{}, expected_values{};
    auto flags = initial;
    Value last_condition;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        const auto type = c.width == 8    ? Type::U8
                          : c.width == 16 ? Type::U16
                          : c.width == 32 ? Type::U32
                                          : Type::U64;
        const auto left = Constant(builder, c.left);
        const OperationMetadata metadata{c.mask, c.profile, false};
        const auto result = c.op == OpCode::Neg
                                    ? Need(builder.Emit<OpCode::Neg>(
                                              type, {Operand::ValueOf(left, type)}, metadata))
                                    : Need(builder.Append(c.op,
                                                          type,
                                                          {Operand::ValueOf(left, type),
                                                           Operand::Immediate(c.right, type)},
                                                          metadata));
        // This CMP deliberately destroys hardware NZCV between producer and reader.
        (void)Need(builder.Emit<OpCode::TestZero>(Type::Bool, {Operand::ValueOf(left)}));
        Store(builder, static_cast<std::uint32_t>(i * 16), result);
        Store(builder, static_cast<std::uint32_t>(i * 16 + 8), Snapshot(builder, result));
        last_condition = Need(builder.Append(i & 1 ? OpCode::LocalCondSet : OpCode::CondSet,
                                             Type::Bool,
                                             {Operand::Condition(Condition::EQ)}));
        Store(builder, static_cast<std::uint32_t>(cases.size() * 16 + i * 8), last_condition);
        flags = ReferenceArithmetic(c.op, c.left, c.right, c.width, c.profile, c.mask, flags);
        expected_flags[i] = flags;
        expected_values[i] = (c.op == OpCode::Neg   ? u64{} - c.left
                              : c.op == OpCode::Sub ? c.left - c.right
                                                    : c.left + c.right) &
                             (c.width == 64 ? ~u64{} : (u64{1} << c.width) - 1);
    }
    Need(builder.EndBlock(Terminal::Return(last_condition)));
    auto program = Submit(builder);
    const auto disabled = arm64::Compile(program);
    Check(!disabled && disabled.error == arm64::CompileError::UnsupportedFlags &&
                  disabled.words.empty(),
          "flags require explicit ABI field");
    auto code = Compile(program, {240});
    std::array<std::uint8_t, 256> state{};
    Write64(state.data() + 240, initial);
    if (NativeMode()) Execute(code, state.data(), (flags >> 30) & 1);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        for (std::size_t i = 0; i < cases.size(); ++i) {
            Check(Read64(state.data() + i * 16) == expected_values[i],
                  "flag-setting numeric result");
            Check(Read64(state.data() + i * 16 + 8) == expected_flags[i],
                  "raw/canonical/partial packed flags");
            Check(Read64(state.data() + cases.size() * 16 + i * 8) ==
                          ((expected_flags[i] >> 30) & 1),
                  "CondSet restores NZCV after CMP");
        }
        Check(Read64(state.data() + 240) == flags, "return publishes packed flags");
    }
#endif
}

void FlagsQueriesAndAliases() {
    constexpr std::uint32_t slot = 65;
    constexpr u64 initial = 0xa5a50000bc00003cULL, replacement = 0x1122334474000055ULL;
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto input = Constant(builder, 0x1234);
    const auto logical = Need(
            builder.Emit<OpCode::Xor>(Type::U64,
                                      {Operand::ValueOf(input), Operand::ValueOf(input)},
                                      {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false}));
    const auto parity =
            Need(builder.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::Parity)}));
    Store(builder, 0, parity);
    const auto no_cz = Need(builder.Emit<OpCode::TestNotFlags>(
            Type::Bool, {Operand::Flags(Flag::Carry | Flag::Zero)}));
    Store(builder, 8, no_cz);
    const auto three = Constant(builder, 3);
    const auto local_parity = Need(builder.Emit<OpCode::LocalParitySet>(
            Type::Bool, {Operand::ValueOf(three), Operand::Immediate(0, Type::U8)}));
    Store(builder, 16, local_parity);
    const auto one = Constant(builder, 1);
    Need(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(one)}));
    Need(builder.Emit<OpCode::SetOverflow>(Type::Void, {Operand::ValueOf(one)}));
    Need(builder.Emit<OpCode::InvertCarry>(Type::Void, {}));
    Need(builder.Emit<OpCode::ClearFlags>(Type::Void, {Operand::Flags(Flag::Zero | Flag::Parity)}));
    const auto mixed = Need(builder.Emit<OpCode::TestFlags>(
            Type::Bool, {Operand::Flags(Flag::Overflow | Flag::Parity | Flag::AuxiliaryCarry)}));
    Store(builder, 24, mixed);
    const auto yes = Constant(builder, 0x100), no = Constant(builder, 0x22);
    const auto selected = Need(builder.Emit<OpCode::CondSelect>(Type::U64,
                                                                {Operand::Condition(Condition::VS),
                                                                 Operand::ValueOf(yes, Type::U8),
                                                                 Operand::ValueOf(no)}));
    Store(builder, 32, selected);
    const auto wide = Constant(builder, 0xaabbccdd00000055ULL);
    const auto selected32 = Need(builder.Emit<OpCode::CondSelect>(
            Type::U32,
            {Operand::Condition(Condition::VS), Operand::ValueOf(wide), Operand::ValueOf(no)}));
    Store(builder, 112, selected32);
    const auto select32 = Need(builder.Emit<OpCode::Select>(
            Type::U32, {Operand::ValueOf(yes), Operand::ValueOf(wide), Operand::ValueOf(no)}));
    Store(builder, 120, select32);
    const auto narrow_condition = Need(builder.Emit<OpCode::Select>(
            Type::U64,
            {Operand::ValueOf(yes, Type::U8), Operand::ValueOf(one), Operand::ValueOf(no)}));
    Store(builder, 40, narrow_condition);
    const auto wide_condition = Need(builder.Emit<OpCode::Select>(
            Type::U64, {Operand::ValueOf(yes), Operand::ValueOf(one), Operand::ValueOf(no)}));
    Store(builder, 48, wide_condition);
    const auto byte = Constant(builder, 0x50);
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(slot + 3, Type::U8), Operand::ValueOf(byte, Type::U8)}));
    const auto uniform_word = Load(builder, slot);
    Store(builder, 80, uniform_word);
    const auto after_partial =
            Need(builder.Emit<OpCode::CondSet>(Type::Bool, {Operand::Condition(Condition::EQ)}));
    Store(builder, 88, after_partial);
    const auto address = Load(builder, 128);
    const auto result = Need(
            builder.Emit<OpCode::Add>(Type::U64,
                                      {Operand::ValueOf(input), Operand::Immediate(1, Type::U64)},
                                      {FlagBits(Flag::All), FlagProfile::ArithmeticRaw, false}));
    const auto observed =
            Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(address)}));
    Store(builder, 96, observed);
    const auto new_word = Constant(builder, replacement);
    Need(builder.Emit<OpCode::StoreMemoryTSO>(
            Type::Void, {Operand::ValueOf(address), Operand::ValueOf(new_word)}));
    Store(builder, 104, Snapshot(builder, result));
    const auto raw_all = Need(builder.Emit<OpCode::GetFlags>(
            Type::U64, {Operand::ValueOf(logical), Operand::Flags(UINT64_MAX)}));
    Need(builder.EndBlock(Terminal::Return(raw_all)));
    auto program = Submit(builder);
    auto code = Compile(program, {slot});
    const auto store_shadow = 0xf9000000u | (17u << 5) | 1u;
    const auto barrier = std::find(code.words.begin(), code.words.end(), 0xd5033bbfu);
    Check(barrier != code.words.end() &&
                  std::find(code.words.begin(), barrier, store_shadow) != barrier,
          "flags must be stored before guest memory can fault");
    std::array<std::uint8_t, 160> state{};
    Write64(state.data() + slot, initial);
    Write64(state.data() + 128, reinterpret_cast<std::uintptr_t>(state.data() + slot));
    if (NativeMode()) Execute(code, state.data(), replacement);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 1 && Read64(state.data() + 8) == 0 &&
                      Read64(state.data() + 16) == 1,
              "PF and any-CZ queries");
        Check(Read64(state.data() + 24) == 0 && Read64(state.data() + 32) == 0,
              "mixed test conjunction/CondSelect view");
        Check(Read64(state.data() + 112) == 0x55 && Read64(state.data() + 120) == 0x55,
              "U32 conditional results normalize wide inputs");
        Check(Read64(state.data() + 40) == 0x22 && Read64(state.data() + 48) == 1,
              "Select reads complete condition view");
        const auto partial = (initial & ~u64{0xffffffff}) | 0x50000001ULL;
        Check(Read64(state.data() + 80) == partial && Read64(state.data() + 88) == 1,
              "partial Uniform flags alias reload");
        const auto pending = ReferenceArithmetic(OpCode::Add,
                                                 0x1234,
                                                 1,
                                                 64,
                                                 FlagProfile::ArithmeticRaw,
                                                 FlagBits(Flag::All),
                                                 partial);
        Check(Read64(state.data() + 96) == pending,
              "memory read sees pending guest flags before fault point");
        Check(Read64(state.data() + 104) == replacement &&
                      Read64(state.data() + slot) == replacement,
              "memory store alias reload/raw GetFlags");
    }
#endif
}

void FlagsWithSpilledResult() {
    constexpr std::uint32_t slot = 65;
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto left = Constant(builder, 15);
    const auto result = Need(
            builder.Emit<OpCode::Add>(Type::U64,
                                      {Operand::ValueOf(left), Operand::Immediate(1, Type::U64)},
                                      {FlagBits(Flag::All), FlagProfile::ArithmeticRaw, false}));
    std::array<Value, 32> values{};
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = Constant(builder, i + 20);
    const auto address = Load(builder, 128);
    Need(builder.Emit<OpCode::StoreMemoryTSO>(
            Type::Void, {Operand::ValueOf(address), Operand::ValueOf(result)}));
    auto total = values[0];
    for (std::size_t i = 1; i < values.size(); ++i)
        total = Need(builder.Emit<OpCode::Add>(
                Type::U64, {Operand::ValueOf(total), Operand::ValueOf(values[i])}));
    Store(builder, 0, total);
    Store(builder, 8, Snapshot(builder, result));
    Need(builder.EndBlock(Terminal::Return(result)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateBlock(program);
    Check(bool(allocation) && allocation.homes[result.node.value].spilled,
          "flags result is a real spill");
    auto code = Compile(program, {slot});
    std::array<std::uint8_t, 144> state{};
    u64 memory{};
    constexpr u64 initial = 0xabc0000080000033ULL;
    Write64(state.data() + slot, initial);
    Write64(state.data() + 128, reinterpret_cast<std::uintptr_t>(&memory));
    if (NativeMode()) Execute(code, state.data(), 16);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        const auto expected = ReferenceArithmetic(
                OpCode::Add, 15, 1, 64, FlagProfile::ArithmeticRaw, FlagBits(Flag::All), initial);
        Check(memory == 16 && Read64(state.data() + 8) == expected &&
                      Read64(state.data() + slot) == expected,
              "flags helpers preserve spilled numeric result and unaligned store input");
    }
#endif
}

void LegacyFlags() {
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    constexpr u64 initial = 0xcafe0000f40000aaULL;
    std::array<u64, 3> expected{};
    auto previous = initial;
    Value last;
    for (unsigned i = 0; i < 3; ++i) {
        const auto type = i == 1 ? Type::U16 : Type::U8;
        const auto left = Constant(builder, i == 0 ? 255 : i == 1 ? 0x8000 : 0x80);
        const auto op = i == 0 ? OpCode::Add : i == 1 ? OpCode::Sub : OpCode::Or;
        const u64 mask = i == 0   ? FlagBits(Flag::All)
                         : i == 1 ? FlagBits(Flag::NZCV | Flag::Parity)
                                  : FlagBits(Flag::Negate | Flag::Zero | Flag::Parity);
        last = Need(builder.Append(
                op,
                type,
                {Operand::ValueOf(left, type), Operand::Immediate(i == 2 ? 0 : 1, type)}));
        if (i)
            Need(builder.Emit<OpCode::ClearFlags>(
                    Type::Void,
                    {Operand::Flags(i == 1 ? Flag::AuxiliaryCarry
                                           : Flag::Carry | Flag::Overflow |
                                                     Flag::AuxiliaryCarry)}));
        Need(builder.Emit<OpCode::SaveFlags>(Type::Void,
                                             {Operand::ValueOf(last), Operand::Flags(mask)}));
        Store(builder, i * 8, Snapshot(builder, last));
        if (i < 2) {
            if (i) previous &= ~(u64{1} << 26);
            previous = ReferenceArithmetic(op,
                                           i == 0 ? 255 : 0x8000,
                                           1,
                                           i == 0 ? 8 : 16,
                                           FlagProfile::ArithmeticRaw,
                                           mask,
                                           previous);
        } else {
            previous &= ~((u64{15} << 28) | (u64{1} << 26) | 255);
            previous |= (u64{1} << 31) | 0x80;
        }
        expected[i] = previous;
    }
    Need(builder.EndBlock(Terminal::Return(last)));
    auto program = Submit(builder);
    auto code = Compile(program, {64});
    std::array<std::uint8_t, 80> state{};
    Write64(state.data() + 64, initial);
    if (NativeMode()) Execute(code, state.data(), 0x80);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        for (unsigned i = 0; i < expected.size(); ++i)
            Check(Read64(state.data() + i * 8) == expected[i], "legacy exact narrow flags pair");
    }
#endif
    // Each failure has a legal IR body but exceeds the explicit local-pair domain.
    for (unsigned bad = 0; bad < 3; ++bad) {
        Builder invalid{Scope::Block};
        Need(invalid.BeginBlock(Create(invalid)));
        const auto input = Constant(invalid, 1);
        const OperationMetadata metadata =
                bad == 2 ? OperationMetadata{FlagBits(Flag::All), FlagProfile::ArithmeticRaw, false}
                         : OperationMetadata{};
        const auto producer = Need(invalid.Emit<OpCode::Add>(
                Type::U64, {Operand::ValueOf(input), Operand::Immediate(1, Type::U64)}, metadata));
        if (bad == 0) Need(invalid.Emit<OpCode::Nop>(Type::Void, {}));
        if (bad == 1)
            Need(invalid.Emit<OpCode::ClearFlags>(Type::Void, {Operand::Flags(Flag::Carry)}));
        Need(invalid.Emit<OpCode::SaveFlags>(
                Type::Void, {Operand::ValueOf(producer), Operand::Flags(Flag::All)}));
        Need(invalid.EndBlock(Terminal::Return(producer)));
        auto source = Submit(invalid);
        const auto rejected = arm64::Compile(source, {64});
        Check(!rejected && rejected.error == arm64::CompileError::UnsupportedFlags &&
                      rejected.words.empty(),
              "legacy gap/overlap/conflict fails closed");
    }
}

void ConstantsAndViews() {
    Builder builder{Scope::Block, arm64::KnownTargetInfo()};
    Need(builder.BeginBlock(Create(builder)));
    const auto source = Constant(builder, 0xffffffff80000080ULL);
    Need(builder.Emit<OpCode::Nop>(Type::Void, {}));
    const auto raw =
            Need(builder.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(source, Type::U8)}));
    Store(builder, 16, raw);
    const auto zero = Need(builder.Emit<OpCode::ZeroExtend32To64>(
            Type::U64, {Operand::ValueOf(source, Type::U8)}));
    const auto sign =
            Need(builder.Emit<OpCode::SignExtend>(Type::S64, {Operand::ValueOf(source, Type::S8)}));
    Store(builder, 0, zero);
    Store(builder, 8, sign);
    const auto shifted = Need(builder.Emit<OpCode::AsrImm>(
            Type::U32, {Operand::ValueOf(source, Type::S8), Operand::Immediate(33, Type::U8)}));
    Store(builder,
          24,
          Need(builder.Emit<OpCode::LslImm>(
                  Type::U8, {Operand::ValueOf(source), Operand::Immediate(9, Type::U8)})));
    Store(builder,
          32,
          Need(builder.Emit<OpCode::LsrImm>(
                  Type::U32, {Operand::ValueOf(source), Operand::Immediate(36, Type::U8)})));
    Store(builder,
          40,
          Need(builder.Emit<OpCode::RorImm>(
                  Type::U32, {Operand::ValueOf(source), Operand::Immediate(4, Type::U8)})));
    const auto masked = Need(builder.Emit<OpCode::And>(
            Type::U64,
            {Operand::ValueOf(source), Operand::Immediate(0xffffffff0000ffffULL, Type::U64)},
            {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA}));
    Store(builder, 48, masked);
    Store(builder,
          56,
          Need(builder.Emit<OpCode::Or>(Type::U32,
                                        {Operand::ValueOf(source, Type::U32),
                                         Operand::Immediate(0x00ff00ff, Type::U32)})));
    Store(builder,
          64,
          Need(builder.Emit<OpCode::Xor>(Type::U64,
                                         {Operand::ValueOf(source),
                                          Operand::Immediate(0x0123456789abcdefULL, Type::U64)})));
    Store(builder,
          72,
          Need(builder.Emit<OpCode::AndNot>(
                  Type::U64, {Operand::ValueOf(source), Operand::Immediate(0xf0, Type::U8)})));
    Store(builder, 80, Constant(builder, UINT32_MAX));
    Store(builder, 88, Constant(builder, UINT64_MAX));
    Store(builder, 96, Snapshot(builder, masked));
    Store(builder,
          112,
          Need(builder.Emit<OpCode::GetOperand>(Type::U32,
                                                {Operand::Expression(OperandOp::AddShift, 2, 31),
                                                 Operand::ValueOf(source),
                                                 Operand::Immediate(1, Type::U64)})));
    Store(builder,
          120,
          Need(builder.Emit<OpCode::GetOperand>(Type::U32,
                                                {Operand::Expression(OperandOp::AddShift, 2, 32),
                                                 Operand::ValueOf(source),
                                                 Operand::Immediate(1, Type::U64)})));
    Store(builder,
          128,
          Need(builder.Emit<OpCode::GetOperand>(Type::U32,
                                                {Operand::Expression(OperandOp::Sub, 2),
                                                 Operand::ValueOf(source),
                                                 Operand::Immediate(0xffffffff, Type::U32)})));
    Need(builder.EndBlock(Terminal::Return(shifted, Type::U64)));
    auto program = Submit(builder);
    auto code = Compile(program, {104});
    const auto expected_words = code.words;
    const auto capacity = code.words.capacity();
    Program empty;
    arm64::Compile(empty, {104}, code);
    Check(!code && code.words.empty() && code.faults.empty() && code.links.empty() &&
                  !code.frame_bytes && !code.spill_slots && code.words.capacity() == capacity,
          "reused output rejects an empty owner without exposing stale code or discarding "
          "capacity");
    arm64::Compile(program, {104}, code);
    Check(bool(code) && code.words == expected_words && code.words.capacity() == capacity,
          "success after a rejected request resets the actual output and reuses its buffer");
    for (const auto word : code.words)
        Check(word != 0xd503201f, "Nop must not emit a machine instruction");
    std::array<std::uint8_t, 136> state{};
    if (NativeMode()) Execute(code, state.data(), 0xffffffc0ULL);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 0x80 && Read64(state.data() + 8) == 0xffffffffffffff80ULL,
              "conversion consumer views");
        Check(Read64(state.data() + 16) == 0xffffffff80000080ULL,
              "BitCast retains raw carrier bits");
        Check(Read64(state.data() + 24) == 0 && Read64(state.data() + 32) == 0x08000008 &&
                      Read64(state.data() + 40) == 0x08000008,
              "immediate shifts keep their natural width and masked count");
        Check(Read64(state.data() + 48) == (0xffffffff80000080ULL & 0xffffffff0000ffffULL) &&
                      Read64(state.data() + 56) == (0x80000080u | 0x00ff00ffu) &&
                      Read64(state.data() + 64) ==
                              (0xffffffff80000080ULL ^ 0x0123456789abcdefULL) &&
                      Read64(state.data() + 72) == (0xffffffff80000080ULL & ~u64{0xf0}),
              "logical constants preserve full and narrow operand views");
        Check(Read64(state.data() + 80) == UINT32_MAX && Read64(state.data() + 88) == UINT64_MAX &&
                      Read64(state.data() + 96) == 0x80000080ULL,
              "negative constants and flag-setting logical masks");
        Check(Read64(state.data() + 112) == 0x80 && Read64(state.data() + 120) == 0x80000080 &&
                      Read64(state.data() + 128) == 0x80000081,
              "word expressions truncate the result without masking their input carriers");
    }
#endif
}

void StateAndMemory() {
    Builder builder{Scope::Block, arm64::KnownTargetInfo()};
    Need(builder.BeginBlock(Create(builder)));
    const auto input = Load(builder, 0);
    const auto sum = Need(builder.Emit<OpCode::Add>(
            Type::U64, {Operand::ValueOf(input), Operand::Immediate(7, Type::U64)}));
    Store(builder, 8, sum);
    const auto base = Load(builder, 16), index = Constant(builder, 0xaabbccdd00000002ULL);
    const auto address =
            Need(builder.Emit<OpCode::GetOperand>(Type::U64,
                                                  {Operand::Expression(OperandOp::AddShift, 2, 3),
                                                   Operand::ValueOf(base),
                                                   Operand::ValueOf(index, Type::U32)}));
    Need(builder.Emit<OpCode::StoreMemoryTSO>(Type::Void,
                                              {Operand::ValueOf(address), Operand::ValueOf(sum)}));
    const auto loaded =
            Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(address)}));
    Need(builder.EndBlock(Terminal::Return(loaded)));
    auto program = Submit(builder);
    auto code = Compile(program);
    Check(std::count(code.words.begin(), code.words.end(), 0xd5033bbf) == 1 &&
                  std::count(code.words.begin(), code.words.end(), 0xd50339bf) == 1,
          "TSO store uses a leading ISH and load a trailing ISHLD");
    std::array<u64, 4> memory{};
    std::array<std::uint8_t, 24> state{};
    Write64(state.data(), 5);
    Write64(state.data() + 16, reinterpret_cast<std::uintptr_t>(memory.data()));
    if (NativeMode()) Execute(code, state.data(), 12);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(memory[2] == 12 && Read64(state.data() + 8) == 12, "state/TSO address semantics");
        Write64(state.data() + 16, reinterpret_cast<std::uintptr_t>(memory.data()) + 1);
        if (NativeMode()) Execute(code, state.data(), 12);
        Check(Read64(reinterpret_cast<const std::uint8_t*>(memory.data()) + 17) == 12,
              "unaligned TSO fallback remains accessible");
    }
#endif
}

void Spills() {
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    std::array<Value, 48> values{};
    u64 expected{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = Constant(builder, 0x1234000000000000ULL + i);
        expected += 0x1234000000000000ULL + i;
    }
    const auto skip = Need(builder.Emit<OpCode::Goto>(Type::U64, {Operand::ValueOf(values[0])}));
    Store(builder, 65521, Constant(builder, 999));
    Need(builder.Emit<OpCode::BindLabel>(Type::Void, {Operand::ValueOf(skip)}));
    auto total = values[1];
    for (std::size_t i = 2; i < values.size(); ++i)
        total = Need(builder.Emit<OpCode::Add>(
                Type::U64, {Operand::ValueOf(total), Operand::ValueOf(values[i])}));
    total = Need(builder.Emit<OpCode::Add>(Type::U64,
                                           {Operand::ValueOf(total), Operand::ValueOf(values[0])}));
    Store(builder, 65537, values[0]);  // Non-scaled address plus a real spilled input.
    Store(builder, 65529, skip);       // The Goto's numeric result must exist before either edge.
    Need(builder.EndBlock(Terminal::Return(total)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateBlock(program);
    Check(bool(allocation) && allocation.homes[values[0].node.value].spilled,
          "long-lived value actually spills");
    Check(allocation.homes[skip.node.value].spilled,
          "the forward branch's real numeric result spills under the existing pressure");
    auto code = Compile(program);
    Check(code.spill_slots > 0, "high-pressure frame missing");
    std::vector<std::uint8_t> state(65552);
    if (NativeMode()) Execute(code, state.data(), expected);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data() + 65537) == 0x1234000000000000ULL,
              "large unaligned uniform spill store");
        Check(Read64(state.data() + 65521) == 0 &&
                      Read64(state.data() + 65529) == 0x1234000000000000ULL,
              "Goto skips the body and saves its complete spilled numeric result before the taken "
              "edge");
    }
#endif
}

void CrossingLoops() {
    std::array<std::uint8_t, 80> state{};
    Builder builder{Scope::Function};
    std::array<BlockRef, 6> blocks{};
    for (auto& block : blocks) block = Create(builder);
    Need(builder.BeginBlock(blocks[0]));
    const auto invariant = Constant(builder, 1000);
    Need(builder.EndBlock(Terminal::Jump(blocks[1])));
    Need(builder.BeginBlock(blocks[1]));
    const auto outer = Load(builder, 0);
    const auto observed = Need(
            builder.Emit<OpCode::Add>(Type::U64,
                                      {Operand::ValueOf(invariant), Operand::ValueOf(outer)},
                                      {FlagBits(Flag::All), FlagProfile::ArithmeticRaw, false}));
    Store(builder, 16, observed);
    Store(builder, 32, Snapshot(builder, observed));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x6060, Type::U64)}));
    (void)Need(builder.Emit<OpCode::LoadMemory>(
            Type::U64,
            {Operand::ValueOf(
                    Constant(builder, reinterpret_cast<std::uintptr_t>(state.data() + 40)))}));
    Need(builder.EndBlock(Terminal::Jump(blocks[2])));
    Need(builder.BeginBlock(blocks[2]));
    const auto inner = Load(builder, 8);
    Need(builder.EndBlock(Terminal::Branch(inner, blocks[4], blocks[3])));
    Need(builder.BeginBlock(blocks[3]));
    const auto remaining = Load(builder, 0);
    const auto next = Need(builder.Emit<OpCode::Sub>(
            Type::U64, {Operand::ValueOf(remaining), Operand::Immediate(1, Type::U64)}));
    Store(builder, 0, next);
    Need(builder.EndBlock(Terminal::Branch(next, blocks[1], blocks[5])));
    Need(builder.BeginBlock(blocks[4]));
    std::array<Value, 32> pressure{};
    for (std::size_t i = 0; i < pressure.size(); ++i) pressure[i] = Constant(builder, 2000 + i);
    auto total = pressure[0];
    for (std::size_t i = 1; i < pressure.size(); ++i)
        total = Need(builder.Emit<OpCode::Add>(
                Type::U64, {Operand::ValueOf(total), Operand::ValueOf(pressure[i])}));
    Store(builder, 24, total);
    const auto counter = Load(builder, 8);
    const auto decrement = Need(builder.Emit<OpCode::Sub>(
            Type::U64, {Operand::ValueOf(counter), Operand::Immediate(1, Type::U64)}));
    Store(builder, 8, decrement);
    Need(builder.EndBlock(Terminal::Jump(blocks[2])));
    Need(builder.BeginBlock(blocks[5]));
    const auto answer = Load(builder, 16);
    Need(builder.EndBlock(Terminal::Return(answer)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateFunction(program);
    Check(bool(allocation), "Function allocation rejected");
    const auto& tail = program.Blocks()[4];
    Check(allocation.homes[invariant.node.value].last_use >=
                  allocation.homes[tail.first_node + tail.node_count - 1].definition_position + 1,
          "crossing backedge lifetime needs transitive closure");
    arm64::CompileOptions options{64};
    options.fault_locations = true;
    auto code = ::Compile(program, options);
    Check(code.spill_slots > 0, "loop pressure did not spill");
    Check(code.faults.size() == 1 && code.faults.front().guest_pc == 0x6060,
          "forward/backward Function emission preserves its full-flags fault location");
    Write64(state.data(), 3);
    Write64(state.data() + 8, 2);
    Write64(state.data() + 40, 0x55);
    if (NativeMode()) Execute(code, state.data(), 1001);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 0 && Read64(state.data() + 8) == 0,
              "both crossing loops execute");
        Check(Read64(state.data() + 32) == ReferenceArithmetic(OpCode::Add,
                                                               1000,
                                                               1,
                                                               64,
                                                               FlagProfile::ArithmeticRaw,
                                                               FlagBits(Flag::All),
                                                               0),
              "forward and backward edges keep full arithmetic flags");
    }
#endif
}

void EmptyHeaderCollision() {
    Builder builder{Scope::Function};
    std::array<BlockRef, 4> blocks{};
    for (auto& block : blocks) block = Create(builder);
    // Empty header is born before entry's first definition at the same node
    // coordinate. At execution, entry dominates the entire repeated loop.
    Need(builder.BeginBlock(blocks[1]));
    Need(builder.EndBlock(Terminal::Jump(blocks[2])));
    Need(builder.BeginBlock(blocks[0]));
    const auto invariant = Constant(builder, 0x123456789abcdef0ULL);
    Need(builder.EndBlock(Terminal::Jump(blocks[1])));
    Need(builder.BeginBlock(blocks[2]));
    Store(builder, 0, invariant);
    const auto counter = Load(builder, 8);
    const auto next = Need(builder.Emit<OpCode::Sub>(
            Type::U64, {Operand::ValueOf(counter), Operand::Immediate(1, Type::U64)}));
    Store(builder, 8, next);
    Need(builder.EndBlock(Terminal::Branch(next, blocks[1], blocks[3])));
    Need(builder.BeginBlock(blocks[3]));
    Need(builder.EndBlock(Terminal::Return()));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateFunction(program);
    Check(bool(allocation) && program.Blocks()[1].node_count == 0 &&
                  program.Blocks()[1].first_node == invariant.node.value,
          "empty header coordinate collision fixture");
    const auto& tail = program.Blocks()[2];
    Check(allocation.homes[invariant.node.value].last_use >=
                  allocation.homes[tail.first_node + tail.node_count - 1].definition_position + 1,
          "equal-coordinate empty backedge must preserve outside definition");
    auto code = Compile(program);
    std::array<std::uint8_t, 16> state{};
    Write64(state.data() + 8, 2);
    if (NativeMode()) Execute(code, state.data(), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 0x123456789abcdef0ULL && Read64(state.data() + 8) == 0,
              "empty-header loop preserves invariant on its second iteration");
    }
#endif
}

void ReorderedBodies() {
    for (const bool loop : {false, true}) {
        Builder builder{Scope::Function};
        std::array<BlockRef, 4> blocks{};
        for (std::size_t i = 0; i < (loop ? 4u : 3u); ++i) blocks[i] = Create(builder);
        // Birth order is middle/header, entry, body/exit. Runtime entry still
        // defines the invariant before the already-built middle overwrites
        // an unrelated register. Both examples satisfy definition dominance.
        Need(builder.BeginBlock(blocks[1]));
        if (loop)
            Need(builder.Emit<OpCode::Nop>(Type::Void, {}));
        else {
            const auto zero = Constant(builder, 0);
            Store(builder, 8, zero);
        }
        Need(builder.EndBlock(Terminal::Jump(blocks[2])));
        Need(builder.BeginBlock(blocks[0]));
        const auto invariant = Constant(builder, 0x123456789abcdef0ULL);
        Need(builder.EndBlock(Terminal::Jump(blocks[1])));
        Need(builder.BeginBlock(blocks[2]));
        Store(builder, 0, invariant);
        if (loop) {
            const auto counter = Load(builder, 8);
            const auto next = Need(builder.Emit<OpCode::Sub>(
                    Type::U64, {Operand::ValueOf(counter), Operand::Immediate(1, Type::U64)}));
            Store(builder, 8, next);
            Need(builder.EndBlock(Terminal::Branch(next, blocks[1], blocks[3])));
            Need(builder.BeginBlock(blocks[3]));
        }
        Need(builder.EndBlock(Terminal::Return()));
        auto program = Submit(builder);
        const auto allocation = arm64::AllocateFunction(program);
        Check(bool(allocation) && allocation.block_order.front() == 0 &&
                      program.Blocks()[1].first_node == 0,
              "reordered runtime-entry fixture");
        Check(allocation.homes[invariant.node.value].definition_position <
                      allocation.homes[program.Blocks()[1].first_node].definition_position,
              "allocation must follow CFG schedule rather than birth IDs");
        auto code = Compile(program);
        std::array<std::uint8_t, 16> state{};
        Write64(state.data() + 8, 2);
        if (NativeMode()) Execute(code, state.data(), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
        if (NativeMode()) {
            Check(Read64(state.data()) == 0x123456789abcdef0ULL && Read64(state.data() + 8) == 0,
                  "reordered acyclic/loop body must preserve the dominating value");
        }
#endif
    }
}

void ExplicitEntry() {
    Builder builder{Scope::Function};
    const auto unused = Create(builder), entry = Create(builder), exit = Create(builder);
    Need(builder.SetEntry(entry));
    Need(builder.BeginBlock(unused));
    const auto wrong = Constant(builder, 99);
    Store(builder, 0, wrong);
    Need(builder.EndBlock(Terminal::Return(wrong)));
    Need(builder.BeginBlock(entry));
    const auto seven = Constant(builder, 7);
    Need(builder.EndBlock(Terminal::Jump(exit)));
    Need(builder.BeginBlock(exit));
    const auto eight = Need(builder.Emit<OpCode::Add>(
            Type::U64, {Operand::ValueOf(seven), Operand::Immediate(1, Type::U64)}));
    Store(builder, 0, eight);
    Need(builder.EndBlock(Terminal::Return(eight)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateFunction(program);
    Check(bool(allocation) && program.EntryBlock() == entry.id &&
                  allocation.block_order.front() == entry.id.value,
          "Function schedule must begin at the explicit entry");
    auto code = Compile(program);
    std::array<std::uint8_t, 8> state{};
    if (NativeMode()) Execute(code, state.data(), 8);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 8, "nonzero entry skips the unrelated block-zero body");
    }
#endif
}

u64 Mask(unsigned bits) { return bits == 64 ? ~u64{} : bits ? (u64{1} << bits) - 1 : 0; }

void PureIntegers() {
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    std::vector<u64> expected;
    const auto expect = [&](Value value, u64 result) {
        StoreCarrier(builder, static_cast<std::uint32_t>(expected.size() * 8), value);
        expected.push_back(result);
    };
    constexpr u64 raw = 0xfedcba9876543210ULL, other_raw = 0x0123456789abcdefULL;
    const auto source = Constant(builder, raw), other = Constant(builder, other_raw);
    const auto alias =
            Need(builder.Emit<OpCode::BitCast>(Type::U8, {Operand::ValueOf(source, Type::U8)}));
    const auto inserted =
            Need(builder.Emit<OpCode::BitCast>(Type::U16, {Operand::ValueOf(other, Type::U16)}));
    const auto shifted_xor = Need(builder.Emit<OpCode::LslImm>(
            Type::U64, {Operand::ValueOf(source), Operand::Immediate(13, Type::U8)}));
    expect(Need(builder.Emit<OpCode::Xor>(
                   Type::U64, {Operand::ValueOf(shifted_xor), Operand::ValueOf(other)})),
           (raw << 13) ^ other_raw);
    const auto shifted_and = Need(builder.Emit<OpCode::LsrImm>(
            Type::U64, {Operand::ValueOf(alias, Type::U8), Operand::Immediate(3, Type::U8)}));
    expect(Need(builder.Emit<OpCode::And>(
                   Type::U64, {Operand::ValueOf(other, Type::U16), Operand::ValueOf(shifted_and)})),
           (other_raw & 65535) & ((raw & 255) >> 3));
    const auto shifted_or = Need(builder.Emit<OpCode::RorImm>(
            Type::U64, {Operand::ValueOf(source), Operand::Immediate(9, Type::U8)}));
    expect(Need(builder.Emit<OpCode::Or>(
                   Type::U64, {Operand::ValueOf(shifted_or), Operand::ValueOf(other, Type::U32)})),
           std::rotr(raw, 9) | (other_raw & UINT32_MAX));
    const auto clipped_shift = Need(builder.Emit<OpCode::LslImm>(
            Type::U64, {Operand::ValueOf(source), Operand::Immediate(1, Type::U8)}));
    expect(Need(builder.Emit<OpCode::Xor>(
                   Type::U64,
                   {Operand::ValueOf(clipped_shift, Type::U8), Operand::ValueOf(other)})),
           ((raw << 1) & 255) ^ other_raw);
    expect(Need(builder.Emit<OpCode::And>(
                   Type::U8,
                   {Operand::ValueOf(alias, Type::U8), Operand::Immediate(0x100, Type::U16)})),
           0);
    expect(Need(builder.Emit<OpCode::Xor>(Type::U16,
                                          {Operand::ValueOf(inserted, Type::U16),
                                           Operand::Immediate(0x10000, Type::U32)})),
           other_raw & 65535);
    expect(Need(builder.Emit<OpCode::AndNot>(
                   Type::U64, {Operand::ValueOf(source), Operand::Immediate(0, Type::U8)})),
           raw);
    expect(Need(builder.Emit<OpCode::Add>(
                   Type::U8,
                   {Operand::ValueOf(alias, Type::U8), Operand::Immediate(0xc6, Type::U8)})),
           ((raw & 255) + 0xc6) & 255);
    expect(Need(builder.Emit<OpCode::Sub>(
                   Type::U16,
                   {Operand::ValueOf(alias, Type::U16), Operand::Immediate(0xffc6, Type::U16)})),
           ((raw & 65535) - 0xffc6) & 65535);
    expect(Need(builder.Emit<OpCode::Add>(
                   Type::U16,
                   {Operand::ValueOf(alias, Type::Bool), Operand::Immediate(1, Type::U8)})),
           (raw & 1) + 1);

    struct Field {
        unsigned first, count;
        Type result;
    };

    const std::array fields{
            Field{0, 0, Type::U64},
            Field{63, 0, Type::U16},
            Field{0, 64, Type::U64},
            Field{63, 1, Type::U8},
            Field{8, 16, Type::U16},
            Field{40, 24, Type::U8},
            Field{0, 8, Type::U32},
            Field{8, 16, Type::U32},
            Field{24, 16, Type::U32},
    };
    for (const auto f : fields) {
        const auto mask = Mask(f.count), natural = Mask(TypeBits(f.result));
        const auto extracted =
                Need(builder.Emit<OpCode::BitExtract>(f.result,
                                                      {Operand::ValueOf(alias, Type::U8),
                                                       Operand::Immediate(f.first, Type::U64),
                                                       Operand::Immediate(f.count, Type::U64)}));
        expect(extracted, ((raw >> f.first) & mask) & natural);
        const auto cleared =
                Need(builder.Emit<OpCode::BitClear>(f.result,
                                                    {Operand::ValueOf(alias, Type::U8),
                                                     Operand::Immediate(f.first, Type::U64),
                                                     Operand::Immediate(f.count, Type::U64)}));
        expect(cleared, (raw & ~(mask << f.first)) & natural);
        const auto replacement =
                Need(builder.Emit<OpCode::BitInsert>(f.result,
                                                     {Operand::ValueOf(alias, Type::U8),
                                                      Operand::ValueOf(inserted, Type::U16),
                                                      Operand::Immediate(f.first, Type::U64),
                                                      Operand::Immediate(f.count, Type::U64)}));
        expect(replacement,
               ((raw & ~(mask << f.first)) | ((other_raw << f.first) & (mask << f.first))) &
                       natural);
    }
    for (const unsigned width : {16u, 32u, 64u}) {
        const auto swapped = Need(builder.Emit<OpCode::ByteSwap>(
                Type::U64,
                {Operand::ValueOf(alias, Type::U8), Operand::Immediate(width, Type::U64)}));
        u64 oracle{};
        for (unsigned byte = 0; byte < width / 8; ++byte)
            oracle = (oracle << 8) | ((raw >> (byte * 8)) & 255);
        expect(swapped, oracle);
    }
    const auto signed_byte =
            Need(builder.Emit<OpCode::LoadImm>(Type::S8, {Operand::Immediate(~u64{}, Type::S8)}));
    expect(signed_byte, 255);  // Observe the actual carrier through an explicitly wider use.
    expect(Need(builder.Emit<OpCode::BitExtract>(Type::U64,
                                                 {Operand::ValueOf(signed_byte, Type::S8),
                                                  Operand::Immediate(8, Type::U64),
                                                  Operand::Immediate(56, Type::U64)})),
           0);
    expect(Need(builder.Emit<OpCode::PopCount>(Type::U64,
                                               {Operand::ValueOf(signed_byte, Type::U8)})),
           8);
    expect(Need(builder.Emit<OpCode::PopCount>(Type::U64, {Operand::ValueOf(alias, Type::U8)})),
           std::popcount(raw));
    const std::array<u64, 7> inputs{
            0, 1, ~u64{}, u64{1} << 63, u64{1} << 31, 0x8000000000000001ULL, raw};
    for (const auto input : inputs) {
        const auto value = Constant(builder, input);
        expect(Need(builder.Emit<OpCode::CountLeadingZeros64>(Type::U64,
                                                              {Operand::ValueOf(value)})),
               std::countl_zero(input));
        expect(Need(builder.Emit<OpCode::CountTrailingZeros64>(Type::U64,
                                                               {Operand::ValueOf(value)})),
               std::countr_zero(input));
        expect(Need(builder.Emit<OpCode::CountLeadingZeros32>(
                       Type::U32, {Operand::ValueOf(value, Type::U32)})),
               std::countl_zero(static_cast<std::uint32_t>(input)));
        expect(Need(builder.Emit<OpCode::CountTrailingZeros32>(
                       Type::U32, {Operand::ValueOf(value, Type::U32)})),
               std::countr_zero(static_cast<std::uint32_t>(input)));
        expect(Need(builder.Emit<OpCode::PopCount>(Type::U64, {Operand::ValueOf(value)})),
               std::popcount(input));
    }
    const auto product = Need(builder.Emit<OpCode::Mul>(
            Type::U16, {Operand::ValueOf(alias, Type::U8), Operand::ValueOf(inserted, Type::U16)}));
    expect(product, ((raw & 255) * (other_raw & 65535)) & 65535);
    Need(builder.EndBlock(Terminal::Return(product)));
    auto program = Submit(builder);
    auto code = Compile(program);
    Check(std::find(code.words.begin(), code.words.end(), 0x0e205800u) != code.words.end(),
          "PopCount emits baseline CNT scratch");
    std::vector<std::uint8_t> state(expected.size() * 8);
    if (NativeMode()) Execute(code, state.data(), expected.back());
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        for (std::size_t i = 0; i < expected.size(); ++i)
            Check(Read64(state.data() + i * 8) == expected[i],
                  "pure integer raw-carrier/width oracle");
    }
#endif
}

void X87StatusResultUses() {
    namespace x86 = swift::runtime::x86;

    struct State {
        x86::X87State fpu;
        u64 initialized{~u64{}}, poison{};
    };

    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto state = Need(builder.Emit<OpCode::GetUniformAddress>(
            Type::U64, {Operand::Immediate(offsetof(State, fpu), Type::U32)}));
    const auto zero = Constant(builder, 0);
    const auto init = Need(builder.Emit<OpCode::X87Op>(
            Type::U64,
            {Operand::ValueOf(state),
             Operand::Immediate(x86::X87Encode(x86::X87Command::Init), Type::U64),
             Operand::ValueOf(zero)}));
    const auto live_init = Need(builder.Emit<OpCode::X87Op>(
            Type::U64,
            {Operand::ValueOf(state),
             Operand::Immediate(x86::X87Encode(x86::X87Command::Init), Type::U64),
             Operand::ValueOf(zero)}));
    Store(builder, offsetof(State, initialized), live_init);
    const auto push = Need(builder.Emit<OpCode::X87Op>(
            Type::U64,
            {Operand::ValueOf(state),
             Operand::Immediate(x86::X87Encode(x86::X87Command::PushOne), Type::U64),
             Operand::ValueOf(zero)}));
    Store(builder, offsetof(State, poison), Constant(builder, 0x55));
    const auto status = Need(builder.Emit<OpCode::X87Op>(
            Type::U64,
            {Operand::ValueOf(state),
             Operand::Immediate(x86::X87Encode(x86::X87Command::Status), Type::U64),
             Operand::ValueOf(zero)}));
    Need(builder.EndBlock(Terminal::Return(status)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateBlock(program);
    Check(bool(allocation) &&
                  allocation.homes[init.node.value].last_use ==
                          allocation.homes[init.node.value].definition_position &&
                  allocation.homes[push.node.value].last_use ==
                          allocation.homes[push.node.value].definition_position &&
                  allocation.homes[live_init.node.value].last_use >
                          allocation.homes[live_init.node.value].definition_position &&
                  allocation.homes[status.node.value].last_use >
                          allocation.homes[status.node.value].definition_position,
          "x87 status users include a real numeric store and the terminal return");
    const auto code = Compile(program);
    State observed;
    observed.fpu.control = 0;
    if (NativeMode()) Execute(code, reinterpret_cast<std::uint8_t*>(&observed), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(observed.initialized == 0 && observed.poison == 0x55 &&
                      observed.fpu.control == 0x037f && observed.fpu.status == 0x3800 &&
                      observed.fpu.tags == 0x3fff && observed.fpu.result == 0x3800 &&
                      observed.fpu.registers[7].significand == UINT64_C(0x8000000000000000) &&
                      observed.fpu.registers[7].sign_exp == 0x3fff,
              "dead x87 status results retain state effects and live status results remain zero");
    }
#endif
}

void PureIntegerSpills() {
    constexpr u64 a = 0xfedcba9876543210ULL, b = 0x0123456789abcdefULL;
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto left = Constant(builder, a), right = Constant(builder, b);
    std::array<Value, 32> pressure{};
    for (std::size_t i = 0; i < pressure.size(); ++i) pressure[i] = Constant(builder, i + 1);
    auto sum = pressure[0];
    for (std::size_t i = 1; i < pressure.size(); ++i)
        sum = Need(builder.Emit<OpCode::Add>(
                Type::U64, {Operand::ValueOf(sum), Operand::ValueOf(pressure[i])}));
    const auto product =
            Need(builder.Emit<OpCode::Mul>(Type::U64,
                                           {Operand::ValueOf(left),
                                            Operand::Expression(OperandOp::AddShift, 2, 2),
                                            Operand::ValueOf(right),
                                            Operand::Immediate(3, Type::U64)}));
    StoreCarrier(builder, 0, product);
    const auto inserted =
            Need(builder.Emit<OpCode::BitInsert>(Type::U64,
                                                 {Operand::ValueOf(left),
                                                  Operand::ValueOf(right),
                                                  Operand::Immediate(0, Type::U64),
                                                  Operand::Immediate(64, Type::U64)}));
    StoreCarrier(builder, 8, inserted);
    const auto unsigned_high =
            Need(builder.Emit<OpCode::MulHigh>(Type::U64,
                                               {Operand::ValueOf(left, Type::U8),
                                                Operand::ValueOf(right, Type::U16),
                                                Operand::Immediate(0, Type::U8)}));
    StoreCarrier(builder, 16, unsigned_high);
    const auto signed_high = Need(builder.Emit<OpCode::MulHigh>(
            Type::U64,
            {Operand::ValueOf(left), Operand::ValueOf(right), Operand::Immediate(1, Type::U8)}));
    StoreCarrier(builder, 24, signed_high);
    StoreCarrier(builder, 32, sum);
    Need(builder.EndBlock(Terminal::Return(product)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateBlock(program);
    Check(bool(allocation) && allocation.homes[left.node.value].spilled &&
                  allocation.homes[right.node.value].spilled,
          "both integer binary inputs actually spill");
    auto code = Compile(program);
    std::array<std::uint8_t, 40> state{};
    if (NativeMode()) Execute(code, state.data(), a * (b + 12));
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        const auto high_unsigned = static_cast<u64>((static_cast<unsigned __int128>(a) * b) >> 64);
        const __int128 signed_a = static_cast<__int128>(a) - (static_cast<__int128>(1) << 64);
        const auto high_signed =
                static_cast<u64>(static_cast<unsigned __int128>(signed_a * b) >> 64);
        Check(Read64(state.data()) == a * (b + 12) && Read64(state.data() + 8) == b,
              "compound Mul/full-width insert with two spilled inputs");
        Check(Read64(state.data() + 16) == high_unsigned &&
                      Read64(state.data() + 24) == high_signed && Read64(state.data() + 32) == 528,
              "signed/unsigned raw MulHigh and preserved pressure values");
    }
#endif

    // All fourteen scalar homes are live at the insert. Only the RHS dies
    // here; the parent and twelve other values must survive its overwrite.
    for (const auto type : {Type::U64, Type::U32}) {
        Builder alias{Scope::Block};
        Need(alias.BeginBlock(Create(alias)));
        const auto parent = Constant(alias, a), rhs = Constant(alias, b);
        std::array<Value, 12> live{};
        for (std::size_t i = 0; i < live.size(); ++i) live[i] = Constant(alias, 100 + i);
        const unsigned count = type == Type::U64 ? 48u : 16u;
        const auto result =
                Need(alias.Emit<OpCode::BitInsert>(type,
                                                   {Operand::ValueOf(parent, Type::U8),
                                                    Operand::ValueOf(rhs, Type::U8),
                                                    Operand::Immediate(8, Type::U8),
                                                    Operand::Immediate(count, Type::U8)}));
        StoreCarrier(alias, 0, result);
        StoreCarrier(alias, 8, parent);
        for (std::size_t i = 0; i < live.size(); ++i)
            StoreCarrier(alias, static_cast<std::uint32_t>(16 + i * 8), live[i]);
        Need(alias.EndBlock(Terminal::Return(result)));
        auto alias_program = Submit(alias);
        const auto locations = arm64::AllocateBlock(alias_program);
        Check(bool(locations) && !locations.spill_bytes &&
                      !locations.homes[result.node.value].spilled &&
                      !locations.homes[rhs.node.value].spilled &&
                      locations.homes[result.node.value].reg ==
                              locations.homes[rhs.node.value].reg &&
                      locations.homes[rhs.node.value].last_use ==
                              locations.homes[result.node.value].definition_position &&
                      locations.homes[parent.node.value].last_use >
                              locations.homes[result.node.value].definition_position,
              "full scalar bank reuses the insertion source's last-read home while its parent "
              "stays live");
        const auto mask = Mask(count) << 8;
        const auto expected = ((a & ~mask) | ((b << 8) & mask)) & Mask(TypeBits(type));
        auto alias_code = Compile(alias_program);
        std::array<std::uint8_t, 112> alias_state{};
        if (NativeMode()) Execute(alias_code, alias_state.data(), expected);
#if defined(__aarch64__) && defined(__ANDROID__)
        if (NativeMode()) {
            Check(Read64(alias_state.data()) == expected && Read64(alias_state.data() + 8) == a,
                  "X raw insertion preserves high source bits and W insertion clears the result's "
                  "high "
                  "half");
            for (std::size_t i = 0; i < live.size(); ++i)
                Check(Read64(alias_state.data() + 16 + i * 8) == 100 + i,
                      "insertion last-read reuse preserves the full live bank");
        }
#endif
    }
}

void InvalidIntegerShapes() {
    for (unsigned bad = 0; bad < 6; ++bad) {
        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        const auto input = Constant(builder, 3);
        Value result;
        if (bad == 0)
            result = Need(builder.Emit<OpCode::BitExtract>(Type::U64,
                                                           {Operand::ValueOf(input),
                                                            Operand::Immediate(64, Type::U64),
                                                            Operand::Immediate(0, Type::U64)}));
        if (bad == 1)
            result = Need(builder.Emit<OpCode::BitInsert>(Type::U64,
                                                          {Operand::ValueOf(input),
                                                           Operand::ValueOf(input),
                                                           Operand::Immediate(63, Type::U64),
                                                           Operand::Immediate(2, Type::U64)}));
        if (bad == 2)
            result = Need(
                    builder.Emit<OpCode::BitClear>(Type::U64,
                                                   {Operand::ValueOf(input),
                                                    Operand::Immediate(0, Type::U64),
                                                    Operand::Immediate(UINT64_MAX, Type::U64)}));
        if (bad == 3)
            result = Need(builder.Emit<OpCode::ByteSwap>(
                    Type::U64, {Operand::ValueOf(input), Operand::Immediate(8, Type::U8)}));
        if (bad == 4)
            result = Need(builder.Emit<OpCode::MulHigh>(Type::U64,
                                                        {Operand::ValueOf(input),
                                                         Operand::ValueOf(input),
                                                         Operand::Immediate(2, Type::U8)}));
        if (bad == 5)
            result = Need(builder.Emit<OpCode::Mul>(
                    Type::U64,
                    {Operand::ValueOf(input), Operand::Immediate(2, Type::U64)},
                    {FlagBits(Flag::All), FlagProfile::ArithmeticRaw, false}));
        Need(builder.EndBlock(Terminal::Return(result)));
        auto program = Submit(builder);
        const auto rejected = arm64::Compile(program, {64});
        Check(!rejected && rejected.words.empty() &&
                      rejected.error == (bad == 5 ? arm64::CompileError::UnsupportedFlags
                                                  : arm64::CompileError::UnsupportedOperand),
              "invalid fields/selectors or unsupported Mul flags fail explicitly");
    }
}

void EntryAndTerminals() {
    Builder builder{Scope::Function};
    std::array<BlockRef, 4> blocks{};
    for (auto& block : blocks) block = Create(builder);
    Need(builder.BeginBlock(blocks[2]));
    const auto no = Constant(builder, 42);
    Need(builder.EndBlock(Terminal::Return(no)));
    Need(builder.BeginBlock(blocks[3]));
    const auto yes = Constant(builder, 99);
    Need(builder.EndBlock(Terminal::Return(yes)));
    Need(builder.BeginBlock(blocks[0]));
    const auto condition = Constant(builder, 0x100);
    Need(builder.EndBlock(Terminal::Branch(condition, blocks[3], blocks[1], Type::U8)));
    Need(builder.BeginBlock(blocks[1]));
    Need(builder.EndBlock(Terminal::Jump(blocks[2])));
    auto program = Submit(builder);
    auto code = Compile(program);
    Check(program.Blocks()[0].first_node != 0 && program.Blocks()[1].node_count == 0,
          "out-of-order/empty body fixture");
    if (NativeMode()) Execute(code, nullptr, 42);
    for (const auto terminal :
         {Terminal::Return(), Terminal::Dispatch(), Terminal::Unreachable()}) {
        Builder single{Scope::Block};
        Need(single.BeginBlock(Create(single)));
        Need(single.EndBlock(terminal));
        auto empty = Submit(single);
        auto native = Compile(empty);
        if (terminal.kind == TerminalKind::Unreachable)
            Check(native.words.back() == 0xd4200000, "Unreachable must trap");
        else if (NativeMode())
            Execute(native, nullptr, 0);
    }
}

void DirectTerminalCondition() {
    Builder builder{Scope::Function};
    const auto entry = Create(builder), yes = Create(builder), no = Create(builder);
    Need(builder.BeginBlock(entry));
    const auto one = Constant(builder, 1);
    const auto condition =
            Need(builder.Emit<OpCode::CondSet>(Type::Bool, {Operand::Condition(Condition::EQ)}));
    // Pure numeric testing clobbers PSTATE, but not the authoritative flags
    // carrier. A delayed terminal condition must restore guest Z, not use it.
    (void)Need(builder.Emit<OpCode::TestZero>(Type::Bool, {Operand::ValueOf(one)}));
    Need(builder.EndBlock(Terminal::Branch(condition, yes, no)));
    Need(builder.BeginBlock(yes));
    const auto eleven = Constant(builder, 11);
    Need(builder.EndBlock(Terminal::Return(eleven)));
    Need(builder.BeginBlock(no));
    const auto twenty_two = Constant(builder, 22);
    Need(builder.EndBlock(Terminal::Return(twenty_two)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateFunction(program);
    Check(bool(allocation) && allocation.homes[condition.node.value].terminal_only &&
                  sizeof(arm64::Home) == 16,
          "sole local terminal condition stays in the existing home row");
    auto code = Compile(program, {64});
    std::size_t csets{}, conditional_branches{}, long_branches{};
    for (const auto word : code.words) {
        csets += (word & 0xffff0fe0u) == 0x9a9f07e0u;
        conditional_branches += (word & 0xff000010u) == 0x54000000u;
        long_branches += (word & 0xfc000000u) == 0x14000000u;
    }
    Check(csets == 1 && conditional_branches == 1 && long_branches == 0,
          "forward terminal removes its CSET and branches directly to the actual RPO label");
    std::array<std::uint8_t, 80> state{};
    Write64(state.data() + 64, u64{1} << 30);
    if (NativeMode()) Execute(code, state.data(), 11);
}

void DistantForwardEmission() {
    Builder builder{Scope::Function};
    const auto entry = Create(builder), distant = Create(builder), middle = Create(builder);
    Need(builder.BeginBlock(entry));
    const auto condition = Load(builder, 0);
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x7070, Type::U64)}));
    (void)Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(condition)}));
    Need(builder.EndBlock(Terminal::Branch(condition, distant, middle)));
    Need(builder.BeginBlock(middle));
    // Not executed: enough actual MOV instructions to exceed positive imm19.
    // Nops would emit no words and would not exercise the retry boundary.
    for (unsigned i = 0; i < (1u << 18) + 8; ++i) (void)Constant(builder, i & 255u);
    Need(builder.EndBlock(Terminal::Jump(distant)));
    Need(builder.BeginBlock(distant));
    Store(builder, 128, Constant(builder, 0x8080));
    Need(builder.EndBlock(Terminal::Dispatch()));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.dispatcher = 0x1000;
    options.exit_handler = 0x2000;
    options.pc_offset = 128;
    options.rcpc_loads = options.fault_locations = options.recover_unaligned = true;
    const auto code = arm64::Compile(program, options);
    Check(bool(code) && code.words.size() > (1u << 18),
          "distant forward edge retains the old long-branch support");
    Check(code.faults.size() == 1 && code.faults[0].guest_pc == 0x7070 &&
                  WordOffset(code.faults[0].fallback_offset) != UINT32_MAX &&
                  code.links.size() == 1 && code.links[0].target_pc == 0x8080 &&
                  WordOffset(code.links[0].data_offset) - WordOffset(code.links[0].branch_offset) ==
                          2 + (WordOffset(code.links[0].branch_offset) & 1) &&
                  WordOffset(code.links[0].data_offset) + 2 < code.words.size(),
          "fresh long emission replaces first-attempt fault/link positions without duplicate "
          "records");
    const auto branches =
            std::find_if(code.words.begin(), code.words.end(), [](std::uint32_t word) {
                return (word & 0x7e000000u) == 0x34000000u;
            });
    Check(branches != code.words.end() && ((*(branches) >> 5) & 0x7ffffu) == 2 &&
                  branches + 1 != code.words.end() &&
                  (*(branches + 1) & 0xfc000000u) == 0x14000000u,
          "only an out-of-range imm19 forward edge uses the original skip plus B26 form");
    const auto displacement = *(branches + 1) & 0x03ffffffu;
    Check(!(displacement & (1u << 25)) && displacement + 1 >= (1u << 18) &&
                  static_cast<std::size_t>(branches + 1 - code.words.begin()) + displacement <
                          code.words.size(),
          "the actual forward label distance, not the IR node count, exceeds positive imm19");
}

void TerminalReadBoundaries() {
    Builder builder{Scope::Function};
    std::array<BlockRef, 5> blocks{};
    for (auto& block : blocks) block = Create(builder);
    Need(builder.BeginBlock(blocks[0]));
    const auto one = Constant(builder, 1), zero = Constant(builder, 0);
    Need(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(one)}));
    const auto old_carry =
            Need(builder.Emit<OpCode::CondSet>(Type::Bool, {Operand::Condition(Condition::CS)}));
    Need(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(zero)}));
    Need(builder.EndBlock(Terminal::Branch(old_carry, blocks[1], blocks[4])));
    Need(builder.BeginBlock(blocks[1]));
    const auto auxiliary = Need(
            builder.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::AuxiliaryCarry)}));
    Need(builder.EndBlock(Terminal::Branch(auxiliary, blocks[2], blocks[4])));
    Need(builder.BeginBlock(blocks[2]));
    const auto data_condition = Need(
            builder.Emit<OpCode::LocalCondSet>(Type::Bool, {Operand::Condition(Condition::NE)}));
    StoreCarrier(builder, 0, data_condition);
    Need(builder.EndBlock(Terminal::Branch(data_condition, blocks[3], blocks[4])));
    Need(builder.BeginBlock(blocks[3]));
    const auto eleven = Constant(builder, 11);
    Need(builder.EndBlock(Terminal::Return(eleven)));
    Need(builder.BeginBlock(blocks[4]));
    const auto failure = Constant(builder, 22);
    Need(builder.EndBlock(Terminal::Return(failure)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateFunction(program);
    Check(bool(allocation) && !allocation.homes[old_carry.node.value].terminal_only &&
                  allocation.homes[auxiliary.node.value].terminal_only &&
                  !allocation.homes[data_condition.node.value].terminal_only,
          "flags mutation/data use retain materialized values, while sole AF is direct");
    auto code = Compile(program, {64});
    std::array<std::uint8_t, 80> state{};
    Write64(state.data() + 64, u64{1} << 26);
    if (NativeMode()) Execute(code, state.data(), 11);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 1, "a numeric consumer keeps the complete boolean value");
    }
#endif
}

void NarrowLogicalImmediates() {
    constexpr u64 initial = 0xa5a50000bc00003cULL;
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto canonical =
            Need(builder.Emit<OpCode::LoadImm>(Type::U8, {Operand::Immediate(0xe5, Type::U8)}));
    Store(builder,
          0,
          Need(builder.Emit<OpCode::And>(
                  Type::U8, {Operand::ValueOf(canonical), Operand::Immediate(0xdf, Type::U8)})));
    const auto raw = Constant(builder, 0x1234567890abcde5ULL);
    const auto byte_alias = Need(builder.Emit<OpCode::BitCast>(Type::U8, {Operand::ValueOf(raw)}));
    const auto flagged = Need(builder.Emit<OpCode::And>(
            Type::U8,
            {Operand::ValueOf(byte_alias), Operand::Immediate(0xdf, Type::U8)},
            {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false}));
    Store(builder, 8, flagged);
    Store(builder, 32, Snapshot(builder, flagged));
    const auto raw_half = Constant(builder, 0xabcdef012345b5efULL);
    const auto half_alias =
            Need(builder.Emit<OpCode::BitCast>(Type::U16, {Operand::ValueOf(raw_half)}));
    Store(builder,
          16,
          Need(builder.Emit<OpCode::And>(
                  Type::U16,
                  {Operand::ValueOf(half_alias), Operand::Immediate(0xdfff, Type::U16)})));
    const auto fallback = Need(builder.Emit<OpCode::And>(
            Type::U8, {Operand::ValueOf(byte_alias), Operand::Immediate(0xa5, Type::U8)}));
    Store(builder, 24, fallback);
    Store(builder, 40, Snapshot(builder, flagged));
    Need(builder.EndBlock(Terminal::Return(fallback)));
    auto program = Submit(builder);
    const auto code = ::Compile(program, {128});
    Check(std::count_if(code.words.begin(),
                        code.words.end(),
                        [](std::uint32_t word) { return (word & 0xff800000u) == 0x12000000u; }) >=
                  3,
          "byte/halfword repeated masks select ordinary W logical immediates");
    std::array<std::uint8_t, 144> state{};
    Write64(state.data() + 128, initial);
    if (NativeMode()) Execute(code, state.data(), 0xa5);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 0xc5 && Read64(state.data() + 8) == 0xc5 &&
                      Read64(state.data() + 16) == 0x95ef && Read64(state.data() + 24) == 0xa5,
              "narrow AND keeps complete canonical values after dirty raw inputs and fallback "
              "masks");
        constexpr auto expected = (initial & ~u64{0xf40000ff}) | (u64{1} << 31) | 0xc5;
        Check(Read64(state.data() + 32) == expected && Read64(state.data() + 40) == expected &&
                      Read64(state.data() + 128) == expected,
              "narrow logical SF/ZF/PF/AF/CV and unrelated operations preserve the original flag "
              "contract");
    }
#endif
}

QueryPair QueryPairFixture(u64 leaf, u64 subleaf) noexcept {
    return {leaf | (subleaf << 32), (leaf + 1) | ((subleaf + 1) << 32)};
}

void CpuQueryPairLifetime() {
    Builder builder{Scope::Function};
    Need(builder.BeginBlock(Create(builder)));
    const auto leaf = Constant(builder, 3), subleaf = Constant(builder, 5);
    const auto live = Constant(builder, 0x123456789abcdef0ULL);
    const auto callback =
            Operand::Immediate(reinterpret_cast<std::uintptr_t>(&QueryPairFixture), Type::U64);
    const auto first = Need(builder.Emit<OpCode::Cpuid>(
            Type::U64, {Operand::ValueOf(leaf), Operand::ValueOf(subleaf), callback}));
    const auto second = Need(builder.Emit<OpCode::Cpuid>(
            Type::U64, {Operand::ValueOf(subleaf), Operand::ValueOf(leaf), callback}));
    const auto upper_first =
            Need(builder.Emit<OpCode::CpuidUpper>(Type::U64, {Operand::ValueOf(first)}));
    const auto upper_second =
            Need(builder.Emit<OpCode::CpuidUpper>(Type::U64, {Operand::ValueOf(second)}));
    Store(builder, 0, first);
    Store(builder, 8, upper_first);
    Store(builder, 16, second);
    Store(builder, 24, upper_second);
    Store(builder, 32, live);
    Store(builder, 40, Snapshot(builder, second));
    const auto carry =
            Need(builder.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::Carry)}));
    Need(builder.EndBlock(Terminal::Return(carry)));
    auto program = Submit(builder);
    const auto allocation = arm64::AllocateFunction(program);
    Check(program.ScopeKind() == Scope::Function && bool(allocation) &&
                  allocation.block_order.size() == 1 && allocation.block_order.front() == 0 &&
                  allocation.homes[first.node.value].extra_slot &&
                  allocation.homes[second.node.value].extra_slot,
          "single external Function retains its scope, real homes and both helper result slots");
    arm64::CompileOptions options{48};
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        options.native_nzcv = (getauxval(AT_HWCAP) & HWCAP_FLAGM) != 0;
    }
#else
    options.native_nzcv = true;
#endif
    const auto code = ::Compile(program, options);
    constexpr u64 initial = 0xa5a50000bc00003cULL;
    std::array<std::uint8_t, 64> state{};
    Write64(state.data() + 48, initial);
    if (NativeMode()) Execute(code, state.data(), 1);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == ((u64{5} << 32) | 3) &&
                      Read64(state.data() + 8) == ((u64{6} << 32) | 4),
              "first CPU query retains its own upper result across another query");
        Check(Read64(state.data() + 16) == ((u64{3} << 32) | 5) &&
                      Read64(state.data() + 24) == ((u64{4} << 32) | 6),
              "CPU query calls callback once and returns both ABI words");
        Check(Read64(state.data() + 32) == 0x123456789abcdef0ULL,
              "CPU query preserves live scalar homes");
        Check(Read64(state.data() + 40) == initial && Read64(state.data() + 48) == initial,
              "CPU query preserves guest NZCV, AF and parity");
    }
#endif
}

void ScalarAtomicSequence() {
    alignas(8) u64 memory = 5;
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto address = Constant(builder, reinterpret_cast<std::uintptr_t>(&memory));
    const auto zero = Constant(builder, 0), one = Constant(builder, 1);
    std::vector<u64> expected;
    const auto record = [&](Value old, u64 value) {
        Store(builder, static_cast<std::uint32_t>(expected.size() * 8), old);
        expected.push_back(value);
    };
    record(Need(builder.Emit<OpCode::AtomicFetchAdd>(
                   Type::U32,
                   {Operand::ValueOf(address), Operand::ValueOf(Constant(builder, 3), Type::U32)})),
           5);
    record(Need(builder.Emit<OpCode::CompareAndSwap>(
                   Type::U32,
                   {Operand::ValueOf(address),
                    Operand::ValueOf(Constant(builder, 8), Type::U32),
                    Operand::ValueOf(Constant(builder, 11), Type::U32)})),
           8);
    record(Need(builder.Emit<OpCode::CompareAndSwap>(
                   Type::U32,
                   {Operand::ValueOf(address),
                    Operand::ValueOf(Constant(builder, 8), Type::U32),
                    Operand::ValueOf(Constant(builder, 99), Type::U32)})),
           11);
    record(Need(builder.Emit<OpCode::AtomicExchange>(
                   Type::U16,
                   {Operand::ValueOf(address),
                    Operand::ValueOf(Constant(builder, 0x1234), Type::U16)})),
           11);
    const auto update = [&](AtomicRMWOp operation, Type type, u64 operand, Value carry, u64 old) {
        const auto result = Need(builder.Emit<OpCode::AtomicRMW>(
                type,
                {Operand::Immediate(static_cast<unsigned>(operation), Type::U8),
                 Operand::ValueOf(address),
                 Operand::ValueOf(Constant(builder, operand), type),
                 Operand::ValueOf(carry)}));
        record(result, old);
        return result;
    };
    update(AtomicRMWOp::Sub, Type::U8, 0x35, zero, 0x34);
    update(AtomicRMWOp::AddCarry, Type::U16, 0, one, 0x12ff);
    update(AtomicRMWOp::SubBorrow, Type::U16, 0, one, 0x1300);
    update(AtomicRMWOp::Neg, Type::U8, 0, zero, 0xff);
    update(AtomicRMWOp::And, Type::U16, 0xff00, zero, 0x1201);
    update(AtomicRMWOp::Or, Type::U16, 0x81, zero, 0x1200);
    update(AtomicRMWOp::Xor, Type::U8, 1, zero, 0x81);
    update(AtomicRMWOp::Add, Type::U64, 0x10, zero, 0x1280);
    const auto last = update(AtomicRMWOp::Sub, Type::U64, 0x20, zero, 0x1290);
    Store(builder, 112, Snapshot(builder, last));
    const auto carry =
            Need(builder.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::Carry)}));
    Need(builder.EndBlock(Terminal::Return(carry)));
    auto program = Submit(builder);
    constexpr u64 initial = 0xa5a50000bc00003cULL;
    for (const bool lse : {false, true}) {
        arm64::CompileOptions options{128};
        options.lse_atomics = lse;
#if defined(__aarch64__) && defined(__ANDROID__)
        if (NativeMode()) {
            const auto capabilities = getauxval(AT_HWCAP);
            if (lse && !(capabilities & HWCAP_ATOMICS)) continue;
            options.native_nzcv = (capabilities & HWCAP_FLAGM) != 0;
        }
#else
        options.native_nzcv = true;
#endif
        const auto code = ::Compile(program, options);
        std::array<std::uint8_t, 144> state{};
        Write64(state.data() + 128, initial);
        memory = 5;
        if (NativeMode()) Execute(code, state.data(), 1);
#if defined(__aarch64__) && defined(__ANDROID__)
        if (NativeMode()) {
            for (std::size_t i = 0; i < expected.size(); ++i)
                Check(Read64(state.data() + i * 8) == expected[i],
                      "atomic sequence returns observed old values");
            Check(memory == 0x1270,
                  "mixed-width atomics preserve untouched bytes and explicit carry");
            Check(Read64(state.data() + 112) == initial && Read64(state.data() + 128) == initial,
                  "internal atomic predicates preserve guest NZCV, AF and parity");
        }
#endif
    }
}

void ScalarDivision() {
    using swift::runtime::ExitValue;
    using swift::runtime::NativeExit;
    for (const auto scope : {Scope::Block, Scope::Function}) {
        for (const auto op : {OpCode::Div, OpCode::SignedDiv64}) {
            Builder builder{scope};
            Need(builder.BeginBlock(Create(builder)));
            const auto lhs = Load(builder, 0), rhs = Load(builder, 8);
            const auto result = Need(
                    builder.Append(op, Type::U64, {Operand::ValueOf(lhs), Operand::ValueOf(rhs)}));
            Need(builder.EndBlock(Terminal::Return(result)));
            const auto code = ::Compile(Submit(builder), arm64::CompileOptions{16});
            const auto check = [&](u64 a, u64 b, u64 wanted) {
                std::array<std::uint8_t, 24> state{};
                Write64(state.data(), a);
                Write64(state.data() + 8, b);
                Write64(state.data() + 16, 0xa400003c);
                if (NativeMode()) {
                    Execute(code, state.data(), wanted);
                    Check(Read64(state.data() + 16) == 0xa400003c,
                          "scalar division leaves guest flags intact");
                }
            };
            check(42, 6, 7);
            check(17, 0, ExitValue(NativeExit::DivideFault));
            if (op == OpCode::SignedDiv64) {
                check(u64(-7), 3, u64(-2));
                check(u64{1} << 63, u64(-1), ExitValue(NativeExit::DivideFault));
                check(u64{1} << 63, 1, u64{1} << 63);
            } else {
                check(UINT64_MAX, 2, UINT64_MAX / 2);
            }
        }
    }
}

void Unsupported() {
    {
        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        const auto left = Constant(builder, 9);
        OperationMetadata metadata;
        metadata.flag_outputs = 1;
        const auto value =
                Need(builder.Append(OpCode::Add,
                                    Type::U64,
                                    {Operand::ValueOf(left), Operand::Immediate(3, Type::U64)},
                                    metadata));
        Need(builder.EndBlock(Terminal::Return(value)));
        auto program = Submit(builder);
        const auto code = arm64::Compile(program);
        Check(!code && code.words.empty() && code.node == value.node &&
                      code.error == arm64::CompileError::UnsupportedFlags,
              "unsupported work must not publish partial words");
    }
    const auto empty = arm64::Compile(Program{});
    Check(!empty && empty.words.empty() && empty.error == arm64::CompileError::InvalidProgram,
          "empty Program rejection");
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto source = Constant(builder, 0x1234);
    const auto vector_view =
            Need(builder.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(source, Type::V128)}));
    Need(builder.EndBlock(Terminal::Return(vector_view)));
    auto program = Submit(builder);
    const auto unsupported = arm64::Compile(program);
    Check(!unsupported && unsupported.words.empty() &&
                  unsupported.error == arm64::CompileError::UnsupportedType,
          "raw transport cannot bypass scalar view support");
}

}  // namespace

TEST_CASE("ConstantsAndViews", "[compile][native][arm64]") { CHECK_NOTHROW(ConstantsAndViews()); }

TEST_CASE("StateAndMemory", "[compile][native][arm64]") { CHECK_NOTHROW(StateAndMemory()); }

TEST_CASE("Spills", "[compile][native][arm64]") { CHECK_NOTHROW(Spills()); }

TEST_CASE("SharedContextScalarRegister", "[compile][native][arm64]") {
    CHECK_NOTHROW(SharedContextScalarRegister());
}

TEST_CASE("CrossingLoops", "[compile][native][arm64]") { CHECK_NOTHROW(CrossingLoops()); }

TEST_CASE("EmptyHeaderCollision", "[compile][native][arm64]") {
    CHECK_NOTHROW(EmptyHeaderCollision());
}

TEST_CASE("ReorderedBodies", "[compile][native][arm64]") { CHECK_NOTHROW(ReorderedBodies()); }

TEST_CASE("EntryAndTerminals", "[compile][native][arm64]") { CHECK_NOTHROW(EntryAndTerminals()); }

TEST_CASE("Unsupported", "[compile][arm64]") { CHECK_NOTHROW(Unsupported()); }

TEST_CASE("ScalarDivision", "[compile][native][arm64]") { CHECK_NOTHROW(ScalarDivision()); }

TEST_CASE("ArithmeticFlags", "[compile][native][arm64]") { CHECK_NOTHROW(ArithmeticFlags()); }

TEST_CASE("FlagsQueriesAndAliases", "[compile][native][arm64]") {
    CHECK_NOTHROW(FlagsQueriesAndAliases());
}

TEST_CASE("FlagsWithSpilledResult", "[compile][native][arm64]") {
    CHECK_NOTHROW(FlagsWithSpilledResult());
}

TEST_CASE("LegacyFlags", "[compile][native][arm64]") { CHECK_NOTHROW(LegacyFlags()); }

TEST_CASE("ExplicitEntry", "[compile][native][arm64]") { CHECK_NOTHROW(ExplicitEntry()); }

TEST_CASE("PureIntegers", "[compile][native][arm64]") { CHECK_NOTHROW(PureIntegers()); }

TEST_CASE("X87StatusResultUses", "[compile][native][arm64]") {
    CHECK_NOTHROW(X87StatusResultUses());
}

TEST_CASE("PureIntegerSpills", "[compile][native][arm64]") { CHECK_NOTHROW(PureIntegerSpills()); }

TEST_CASE("InvalidIntegerShapes", "[compile][arm64]") { CHECK_NOTHROW(InvalidIntegerShapes()); }

TEST_CASE("DirectTerminalCondition", "[compile][native][arm64]") {
    CHECK_NOTHROW(DirectTerminalCondition());
}

TEST_CASE("DistantForwardEmission", "[compile][native][arm64]") {
    CHECK_NOTHROW(DistantForwardEmission());
}

TEST_CASE("TerminalReadBoundaries", "[compile][native][arm64]") {
    CHECK_NOTHROW(TerminalReadBoundaries());
}

TEST_CASE("ScalarAtomicSequence", "[compile][native][arm64]") {
    CHECK_NOTHROW(ScalarAtomicSequence());
}

TEST_CASE("CpuQueryPairLifetime", "[compile][native][arm64]") {
    CHECK_NOTHROW(CpuQueryPairLifetime());
}

TEST_CASE("NarrowLogicalImmediates", "[compile][native][arm64]") {
    CHECK_NOTHROW(NarrowLogicalImmediates());
}

TEST_CASE("Guest safepoint prevents an early architectural pin overwrite", "[compile][arm64]") {
    using swift::runtime::x86::GuestState;
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto future = Constant(builder, 0xabcdef);
    Need(builder.Emit<OpCode::GuestSafepoint>(Type::Void, {Operand::Immediate(0x1200, Type::U64)}));
    Store(builder, offsetof(GuestState, gprs), future);
    Need(builder.EndBlock(Terminal::Return(Constant(builder, 42))));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.dispatcher = 0x1000;
    options.exit_handler = 0x2000;
    options.pc_offset = offsetof(GuestState, pc);
    options.flags_offset = offsetof(GuestState, packed_flags);
    options.pinned_uniform_offset = offsetof(GuestState, gprs);
    options.private_uniforms = true;
    const auto allocation = arm64::AllocateBlock(program, &options);
    REQUIRE(bool(allocation));
    CHECK(allocation.homes[future.node.value].reg != arm64::abi::PinnedUniformRegister(0, 8));
    const auto code = arm64::Compile(program, options);
    REQUIRE(bool(code));
    CHECK(code.faults.empty());
    CHECK(code.links.empty());
    CHECK(!code.words.empty());
    // The fixed callback addresses qualify emission only, not execution.
    CHECK(!arm64::Compile(program));
}
