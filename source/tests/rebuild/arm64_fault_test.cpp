#include "tests/support/arm64_fixture.h"

namespace {
using namespace swift::tests::arm64_fixture;

void FaultMetadataAndRecovery() {
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto address = Load(builder, 0), value = Constant(builder, 0x12345678);
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x1000, Type::U64)}));
    const auto loaded =
            Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(address)}));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x1010, Type::U64)}));
    Need(builder.Emit<OpCode::StoreMemoryTSO>(
            Type::Void, {Operand::ValueOf(address), Operand::ValueOf(value)}));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x1020, Type::U64)}));
    (void)Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U8, {Operand::ValueOf(address)}));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x1030, Type::U64)}));
    (void)Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::V128, {Operand::ValueOf(address)}));
    Need(builder.EndBlock(Terminal::Return(loaded)));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.rcpc_loads = options.fault_locations = options.recover_unaligned = true;
    auto code = ::Compile(program, options);
    Check(code.faults.size() == 4 && sizeof(arm64::FaultSite) == 32,
          "one real fault range per marked scalar/vector access");
    for (std::size_t i = 0; i < code.faults.size(); ++i) {
        const auto& site = code.faults[i];
        Check(site.guest_pc == 0x1000 + i * 16 &&
                      WordOffset(site.first_offset) < WordOffset(site.last_offset) &&
                      WordOffset(site.last_offset) <= code.words.size(),
              "fault range maps actual native words to the consumed guest PC");
    }
    const auto& load = code.faults[0];
    const auto& store = code.faults[1];
    Check(WordOffset(load.first_offset) <= WordOffset(load.unaligned_offset) &&
                  WordOffset(load.unaligned_offset) + 1 == WordOffset(load.last_offset) &&
                  (code.words[WordOffset(load.unaligned_offset)] & 0xfffffc00u) == 0xf8bfc000u &&
                  (code.words[WordOffset(load.fallback_offset)] & 0xfffffc00u) == 0xf9400000u &&
                  code.words[WordOffset(load.fallback_offset) + 1] == 0xd50339bfu &&
                  code.words[WordOffset(load.fallback_offset) + 2] ==
                          (0x14000000u | ((WordOffset(load.unaligned_offset) + 1 -
                                           (WordOffset(load.fallback_offset) + 2)) &
                                          0x03ffffffu)),
          "recoverable LDAPR has one hot access and a cold load/barrier returning to its next "
          "word");
    Check(WordOffset(store.first_offset) <= WordOffset(store.unaligned_offset) &&
                  WordOffset(store.unaligned_offset) + 1 == WordOffset(store.last_offset) &&
                  (code.words[WordOffset(store.unaligned_offset)] & 0xfffffc00u) == 0xc89ffc00u &&
                  code.words[WordOffset(store.fallback_offset)] == 0xd5033bbfu &&
                  (code.words[WordOffset(store.fallback_offset) + 1] & 0xfffffc00u) ==
                          0xf9000000u &&
                  code.words[WordOffset(store.fallback_offset) + 2] ==
                          (0x14000000u | ((WordOffset(store.unaligned_offset) + 1 -
                                           (WordOffset(store.fallback_offset) + 2)) &
                                          0x03ffffffu)),
          "recoverable STLR's cold store follows its barrier without a hot placeholder");
    const auto& byte = code.faults[2];
    const auto& vector = code.faults[3];
    Check(WordOffset(byte.unaligned_offset) == UINT32_MAX &&
                  WordOffset(byte.fallback_offset) == UINT32_MAX &&
                  (code.words[WordOffset(byte.first_offset)] & 0xfffffc00u) == 0x38bfc000u &&
                  WordOffset(vector.unaligned_offset) == UINT32_MAX &&
                  WordOffset(vector.fallback_offset) == UINT32_MAX &&
                  code.words[WordOffset(vector.last_offset) - 1] == 0xd50339bfu,
          "byte accesses need no alignment rescue and V128 retains its half barrier");

    Builder halfwords{Scope::Block};
    Need(halfwords.BeginBlock(Create(halfwords)));
    const auto pointer = Load(halfwords, 0);
    Need(halfwords.Emit<OpCode::FaultLocation>(Type::Void,
                                               {Operand::Immediate(0x1040, Type::U64)}));
    const auto half =
            Need(halfwords.Emit<OpCode::LoadMemoryTSO>(Type::U16, {Operand::ValueOf(pointer)}));
    const auto extended = Need(
            halfwords.Emit<OpCode::SignExtend>(Type::U32, {Operand::ValueOf(half, Type::U16)}));
    Store(halfwords, 8, extended);
    Need(halfwords.Emit<OpCode::FaultLocation>(Type::Void,
                                               {Operand::Immediate(0x1050, Type::U64)}));
    const auto shared_half =
            Need(halfwords.Emit<OpCode::LoadMemoryTSO>(Type::U16, {Operand::ValueOf(pointer)}));
    const auto shared_extended = Need(halfwords.Emit<OpCode::SignExtend>(
            Type::U32, {Operand::ValueOf(shared_half, Type::U16)}));
    Store(halfwords, 16, shared_extended);
    Store(halfwords, 24, shared_half);
    Need(halfwords.Emit<OpCode::FaultLocation>(Type::Void,
                                               {Operand::Immediate(0x1060, Type::U64)}));
    const auto word =
            Need(halfwords.Emit<OpCode::LoadMemoryTSO>(Type::U32, {Operand::ValueOf(pointer)}));
    const auto extended_word = Need(
            halfwords.Emit<OpCode::SignExtend>(Type::U64, {Operand::ValueOf(word, Type::U32)}));
    Store(halfwords, 32, extended_word);
    Need(halfwords.EndBlock(Terminal::Return()));
    auto half_program = Submit(halfwords);
    options.rcpc_imm9 = true;
    const auto signed_code = ::Compile(half_program, options);
    Check(signed_code.faults.size() == 3,
          "both actual signed-load widths retain their original fault records");
    const auto& signed_site = signed_code.faults[0];
    const auto& shared_site = signed_code.faults[1];
    Check((signed_code.words[WordOffset(signed_site.unaligned_offset)] & 0xfffffc00u) ==
                          0x59c00000u &&
                  signed_code.words[WordOffset(signed_site.fallback_offset)] ==
                          (signed_code.words[WordOffset(signed_site.unaligned_offset)] ^
                           0x21000000u) &&
                  signed_code.words[WordOffset(signed_site.fallback_offset) + 1] == 0xd50339bfu &&
                  signed_code.words[WordOffset(signed_site.fallback_offset) + 2] ==
                          (0x14000000u | ((WordOffset(signed_site.unaligned_offset) + 1 -
                                           (WordOffset(signed_site.fallback_offset) + 2)) &
                                          0x03ffffffu)) &&
                  (signed_code.words[WordOffset(shared_site.unaligned_offset)] & 0xfffffc00u) ==
                          0x59400000u,
          "sole-use adjacent SXTH uses a signed hot/cold load; a shared unsigned result retains "
          "its original load");
    const auto& signed_word = signed_code.faults[2];
    Check((signed_code.words[WordOffset(signed_word.unaligned_offset)] & 0xfffffc00u) ==
                          0x99800000u &&
                  signed_code.words[WordOffset(signed_word.fallback_offset)] ==
                          (signed_code.words[WordOffset(signed_word.unaligned_offset)] ^
                           0x21000000u) &&
                  signed_code.words[WordOffset(signed_word.fallback_offset) + 1] == 0xd50339bfu,
          "adjacent SXTW uses LDAPURSW and its matching LDURSW acquire fallback");
    options.rcpc_imm9 = false;
    const auto unsigned_code = ::Compile(half_program, options);
    Check((unsigned_code.words[WordOffset(unsigned_code.faults[0].unaligned_offset)] &
           0xfffffc00u) == 0x78bfc000u &&
                  (unsigned_code.words[WordOffset(unsigned_code.faults[0].last_offset)] &
                   0xfffffc00u) == 0x13003c00u,
          "without RCpc imm9 the original unsigned ordered load and SXTH remain separate");
}

void FaultLocationBoundaries() {
    Builder builder{Scope::Function};
    const auto first = Create(builder), second = Create(builder);
    Need(builder.BeginBlock(first));
    const auto address = Load(builder, 0);
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0, Type::U64)}));
    (void)Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(address)}));
    Need(builder.EndBlock(Terminal::Jump(second)));
    Need(builder.BeginBlock(second));
    const auto result =
            Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(address)}));
    Need(builder.EndBlock(Terminal::Return(result)));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.rcpc_loads = options.fault_locations = options.recover_unaligned = true;
    auto code = ::Compile(program, options);
    Check(code.faults.size() == 1 && code.faults[0].guest_pc == 0 &&
                  std::count(code.words.begin(), code.words.end(), 0xb5000060u) == 1,
          "guest PC zero is valid and location validity never leaks across blocks");
    const auto rejected = arm64::Compile(program);
    Check(!rejected && rejected.error == arm64::CompileError::UnsupportedOpcode &&
                  rejected.words.empty() && rejected.faults.empty(),
          "unenabled FaultLocation fails without publishing code or metadata");
    Builder marked{Scope::Block};
    Need(marked.BeginBlock(Create(marked)));
    Need(marked.Emit<OpCode::FaultLocation>(
            Type::Void, {Operand::Immediate(0x123456789abcdef0ULL, Type::U64)}));
    Need(marked.EndBlock(Terminal::Return()));
    auto marker_program = Submit(marked);
    Builder empty{Scope::Block};
    Need(empty.BeginBlock(Create(empty)));
    Need(empty.EndBlock(Terminal::Return()));
    auto empty_program = Submit(empty);
    const auto marker_code = ::Compile(marker_program, options),
               empty_code = Compile(empty_program);
    Check(marker_code.words == empty_code.words && marker_code.faults.empty(),
          "FaultLocation emits zero machine instructions");
}

void LinkedRawAuxiliaryCarry() {
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        if (!(getauxval(AT_HWCAP) & HWCAP_FLAGM)) return;
        namespace execution = swift::runtime::execution;

        struct ObservedState {
            execution::NativeRunContext native;
            std::array<u64, 17> comparisons{};
        };

        constexpr auto comparisons =
                offsetof(ObservedState, comparisons) - offsetof(execution::NativeRunContext, state);
        constexpr u64 entry_pc = 0x1000, next_pc = 0x2000, initial = 0xa5a50000bc00003cULL;
        constexpr u64 byte_carrier = 0x7fedcba987654380ULL, word_carrier = 0x12345678abcde100ULL;
        const OperationMetadata metadata{
                FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry, false};
        arm64::CompileOptions options;
        options.flags_offset = offsetof(swift::runtime::x86::GuestState, packed_flags);
        options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
        options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.private_uniforms = options.native_nzcv = true;
        options.fault_locations = options.deferred_pc = true;
        options.entry_pc = entry_pc;
        options.dispatcher =
                reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCVCounted);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);

        auto target = arm64::KnownTargetInfo();
        target.features |= static_cast<u64>(TargetFeature::PrivateGPRState);
        Builder first{Scope::Block, target};
        Need(first.BeginBlock(Create(first)));
        const auto delayed = Constant(first, 0xaa);
        const auto observed = Constant(first, 0xcc);
        Store(first,
              options.pinned_uniform_offset + 8 * sizeof(u64),
              Load(first, options.pinned_uniform_offset + 9 * sizeof(u64)));
        Store(first, options.pinned_uniform_offset + 9 * sizeof(u64), observed);
        const auto overwritten = Constant(first, 0x1111), byte = Constant(first, 0x22);
        Need(first.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(options.pinned_uniform_offset + 11 * sizeof(u64), Type::U8),
                 Operand::ValueOf(byte, Type::U8)}));
        Store(first, options.pinned_uniform_offset + 11 * sizeof(u64), overwritten);
        Store(first, options.pinned_uniform_offset + 10 * sizeof(u64), delayed);
        const auto fifteen = Constant(first, 15);
        (void)Need(first.Emit<OpCode::Add>(
                Type::U64,
                {Operand::ValueOf(fifteen), Operand::Immediate(1, Type::U64)},
                metadata));
        const auto no_auxiliary = Need(first.Emit<OpCode::TestNotFlags>(
                Type::Bool, {Operand::Flags(Flag::AuxiliaryCarry)}));
        Need(first.Emit<OpCode::StoreUniformSelectImm>(
                Type::Void,
                {Operand::Uniform(options.pc_offset, Type::U64),
                 Operand::ValueOf(no_auxiliary),
                 Operand::Immediate(0x3000, Type::U64),
                 Operand::Immediate(next_pc, Type::U64)}));
        Need(first.EndBlock(Terminal::Dispatch()));
        auto first_program = Submit(first);
        const auto first_allocation = arm64::AllocateBlock(first_program, &options);
        Check(bool(first_allocation) && !first_allocation.publish_entry_pc &&
                      first_allocation.homes[no_auxiliary.node.value].terminal_only,
              "raw AF-only PC consumer uses the existing direct predicate home");
        const auto first_code = arm64::Compile(first_program, options);
        Check(bool(first_code) && !first_code.words.empty(), "RawAF linked producer compilation");

        Builder second{Scope::Block};
        Need(second.BeginBlock(Create(second)));
        Store(second,
              offsetof(swift::runtime::x86::GuestState, gprs) + 13 * sizeof(u64),
              Load(second, options.pc_offset));
        const auto before = Need(
                second.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::AuxiliaryCarry)}));
        Store(second, offsetof(swift::runtime::x86::GuestState, gprs), before);
        const auto clear = Constant(second, 0);
        Need(second.Emit<OpCode::RestoreFlags>(
                Type::Void, {Operand::ValueOf(clear), Operand::Flags(Flag::AuxiliaryCarry)}));
        const auto after = Need(
                second.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::AuxiliaryCarry)}));
        Store(second, offsetof(swift::runtime::x86::GuestState, gprs) + sizeof(u64), after);
        (void)Need(second.Emit<OpCode::Xor>(
                Type::U64,
                {Operand::ValueOf(clear), Operand::ValueOf(clear)},
                {FlagBits(Flag::Parity), FlagProfile::LogicalZeroCVA, false}));
        const auto kept_af = Need(
                second.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::AuxiliaryCarry)}));
        Store(second, offsetof(swift::runtime::x86::GuestState, gprs) + 2 * sizeof(u64), kept_af);
        Store(second,
              offsetof(swift::runtime::x86::GuestState, gprs) + 3 * sizeof(u64),
              Snapshot(second, clear));
        const auto adc_left = Constant(second, 15);
        (void)Need(second.Emit<OpCode::Add>(
                Type::U64,
                {Operand::ValueOf(adc_left), Operand::Immediate(1, Type::U64)},
                {FlagBits(Flag::AuxiliaryCarry), FlagProfile::ArithmeticCanonicalCarry, false}));
        const auto new_af = Need(
                second.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::AuxiliaryCarry)}));
        Store(second, offsetof(swift::runtime::x86::GuestState, gprs) + 4 * sizeof(u64), new_af);
        Need(second.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(Constant(second, 1))}));
        const auto narrow =
                Need(second.Emit<OpCode::Adc>(Type::U8,
                                              {Operand::ValueOf(Constant(second, 0x7f), Type::U8),
                                               Operand::Immediate(0, Type::U8)},
                                              metadata));
        Store(second, offsetof(swift::runtime::x86::GuestState, gprs) + 5 * sizeof(u64), narrow);
        Store(second,
              offsetof(swift::runtime::x86::GuestState, gprs) + 6 * sizeof(u64),
              Snapshot(second, narrow));
        const auto result = Need(second.Emit<OpCode::Adc>(
                Type::U64,
                {Operand::ValueOf(adc_left), Operand::Immediate(1, Type::U64)},
                metadata));
        const auto result_flags = Snapshot(second, result);
        Need(second.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(Constant(second, 1))}));
        const auto sub32 = Need(
                second.Emit<OpCode::Sbb>(Type::U32,
                                         {Operand::ValueOf(Constant(second, 0x80000000), Type::U32),
                                          Operand::Immediate(0, Type::U32)},
                                         metadata));
        Store(second, offsetof(swift::runtime::x86::GuestState, gprs) + 7 * sizeof(u64), sub32);
        Store(second,
              offsetof(swift::runtime::x86::GuestState, gprs) + 12 * sizeof(u64),
              Snapshot(second, sub32));
        constexpr auto observations = offsetof(swift::runtime::x86::GuestState, xmm);
        Need(second.Emit<OpCode::RestoreFlags>(
                Type::Void,
                {Operand::ValueOf(Constant(second, 0x940000a5ULL)), Operand::Flags(Flag::All)}));
        const auto raw_source = Constant(second, 0x1122334400000100ULL);
        const auto raw_alias = Need(
                second.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(raw_source, Type::U8)}));
        const OperationMetadata only_zero{FlagBits(Flag::Zero), FlagProfile::LogicalZeroCVA, false};
        (void)Need(second.Emit<OpCode::And>(
                Type::U8,
                {Operand::ValueOf(raw_alias, Type::U8), Operand::ValueOf(raw_alias, Type::U8)},
                only_zero));
        Store(second,
              observations,
              Snapshot(second, raw_source));  // Do not give TEST a numerical anchor use.
        Store(second, observations + 48, raw_source);
        const auto word_snapshot = Need(second.Emit<OpCode::LoadUniform>(
                Type::U16, {Operand::Uniform(observations + 48, Type::U16)}));
        (void)Need(second.Emit<OpCode::And>(
                Type::U16,
                {Operand::ValueOf(word_snapshot), Operand::ValueOf(word_snapshot, Type::U16)},
                only_zero));
        Store(second, observations + 8, Snapshot(second, raw_source));
        Store(second,
              observations + 16,
              Need(second.Emit<OpCode::Xor>(Type::U8,
                                            {Operand::ValueOf(raw_alias, Type::U8),
                                             Operand::Immediate(0xab, Type::U8)})));
        Store(second,
              observations + 24,
              Need(second.Emit<OpCode::Or>(
                      Type::U8,
                      {Operand::ValueOf(raw_alias), Operand::Immediate(0x80, Type::U8)})));
        Store(second,
              observations + 32,
              Need(second.Emit<OpCode::Xor>(Type::U16,
                                            {Operand::ValueOf(raw_alias, Type::U16),
                                             Operand::Expression(OperandOp::Add, 2),
                                             Operand::Immediate(0x10000, Type::U64),
                                             Operand::Immediate(0x35, Type::U64)})));
        constexpr auto pin14 = static_cast<std::uint32_t>(
                offsetof(swift::runtime::x86::GuestState, gprs) + 14 * sizeof(u64));
        constexpr auto pin15 = pin14 + 8u;
        const auto byte_snapshot = Need(
                second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
        const auto pin_word_snapshot = Need(
                second.Emit<OpCode::LoadUniform>(Type::U16, {Operand::Uniform(pin15, Type::U16)}));
        const auto address_base = Constant(second, 0x1000);
        Store(second,
              observations + 120,
              Need(second.Emit<OpCode::GetOperand>(Type::U64,
                                                   {Operand::Expression(OperandOp::AddShift, 2),
                                                    Operand::ValueOf(address_base),
                                                    Operand::ValueOf(byte_snapshot, Type::U64)})));
        Store(second,
              observations + 128,
              Need(second.Emit<OpCode::GetOperand>(
                      Type::U64,
                      {Operand::Expression(OperandOp::AddShift, 2),
                       Operand::ValueOf(address_base),
                       Operand::ValueOf(pin_word_snapshot, Type::U32)})));
        const auto zero_byte_snapshot = Need(
                second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin15, Type::U8)}));
        (void)Need(second.Emit<OpCode::And>(Type::U32,
                                            {Operand::ValueOf(zero_byte_snapshot, Type::U32),
                                             Operand::ValueOf(zero_byte_snapshot, Type::U32)},
                                            only_zero));
        Store(second, observations + 136, Snapshot(second, address_base));
        Store(second,
              observations + 64,
              Need(second.Emit<OpCode::BitCast>(Type::U64,
                                                {Operand::ValueOf(byte_snapshot, Type::U64)})));
        Store(second,
              observations + 112,
              Need(second.Emit<OpCode::SignExtend>(Type::S64,
                                                   {Operand::ValueOf(byte_snapshot, Type::S64)})));
        Store(second,
              observations + 72,
              Need(second.Emit<OpCode::Xor>(Type::U8,
                                            {Operand::ValueOf(byte_snapshot),
                                             Operand::ValueOf(pin_word_snapshot, Type::U8)})));
        const auto aligned_add = Need(second.Emit<OpCode::Add>(
                Type::U8,
                {Operand::ValueOf(byte_snapshot), Operand::ValueOf(byte_snapshot)},
                metadata));
        Store(second, observations + 144, aligned_add);
        Store(second, observations + 152, Snapshot(second, aligned_add));
        const auto aligned_sub = Need(second.Emit<OpCode::Sub>(
                Type::U16,
                {Operand::ValueOf(byte_snapshot, Type::Bool), Operand::ValueOf(pin_word_snapshot)},
                metadata));
        Store(second, observations + 160, aligned_sub);
        Store(second, observations + 168, Snapshot(second, aligned_sub));
        const auto zero_sub = Need(second.Emit<OpCode::Sub>(
                Type::U8,
                {Operand::ValueOf(byte_snapshot), Operand::Immediate(0x100, Type::U16)},
                metadata));
        Store(second, observations + 176, zero_sub);
        Store(second, observations + 184, Snapshot(second, zero_sub));
        const auto numeric_byte =
                Need(second.Emit<OpCode::BitExtract>(Type::U8,
                                                     {Operand::ValueOf(raw_source),
                                                      Operand::Immediate(8, Type::U64),
                                                      Operand::Immediate(1, Type::U64)}));
        const auto numeric_other = Need(second.Emit<OpCode::And>(
                Type::U8, {Operand::ValueOf(numeric_byte), Operand::Immediate(1, Type::U8)}));
        (void)Need(second.Emit<OpCode::Sub>(
                Type::U8,
                {Operand::ValueOf(numeric_byte), Operand::ValueOf(numeric_other)},
                {FlagBits(Flag::Zero), FlagProfile::ArithmeticCanonicalCarry, false}));
        Store(second, observations + 192, Snapshot(second, raw_source));
        (void)Need(second.Emit<OpCode::Sub>(
                Type::U8,
                {Operand::ValueOf(numeric_byte), Operand::Immediate(~u64{}, Type::S8)},
                {FlagBits(Flag::Carry | Flag::Zero), FlagProfile::ArithmeticRaw, false}));
        Store(second, observations + 200, Snapshot(second, raw_source));
        const auto numeric_word = Need(
                second.Emit<OpCode::LoadImm>(Type::U16, {Operand::Immediate(0x1000, Type::U16)}));
        (void)Need(second.Emit<OpCode::Sub>(
                Type::U16,
                {Operand::ValueOf(numeric_word), Operand::Immediate(0x100001000ULL, Type::U64)},
                {FlagBits(Flag::Carry | Flag::Zero),
                 FlagProfile::ArithmeticCanonicalCarry,
                 false}));
        Store(second, observations + 208, Snapshot(second, raw_source));
        (void)Need(second.Emit<OpCode::Sub>(
                Type::U16,
                {Operand::ValueOf(byte_snapshot, Type::Bool), Operand::ValueOf(pin_word_snapshot)},
                {FlagBits(Flag::Carry | Flag::Zero),
                 FlagProfile::ArithmeticCanonicalCarry,
                 false}));
        Store(second, observations + 216, Snapshot(second, raw_source));
        const auto compare = [&](unsigned index, Type type, Value input, u64 right) {
            (void)Need(second.Emit<OpCode::Sub>(
                    type,
                    {Operand::ValueOf(input, type), Operand::Immediate(right, Type::U64)},
                    metadata));
            Store(second, comparisons + index * sizeof(u64), Snapshot(second, raw_source));
        };
        compare(0, Type::U8, Constant(second, 0x7f), 0xff);
        compare(1, Type::U8, byte_snapshot, 0x7f);  // The actual borrowed dirty pin's low8 is 0x80.
        compare(2, Type::U8, Constant(second, 0xff), 0);
        compare(3, Type::U16, Constant(second, 0x8000), 0xffff);
        compare(4, Type::U16, Constant(second, 0x8000), 0x1234);  // Non-imm12: aligned fallback.
        const auto full_test = [&](unsigned index, Type type, Value input, Type view) {
            (void)Need(second.Emit<OpCode::And>(
                    type,
                    {Operand::ValueOf(input, view), Operand::ValueOf(input, view)},
                    {FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false}));
            Store(second, comparisons + index * sizeof(u64), Snapshot(second, raw_source));
        };
        full_test(5, Type::U8, byte_snapshot, Type::U8);
        full_test(6, Type::U16, pin_word_snapshot, Type::U16);
        full_test(7, Type::U16, byte_snapshot, Type::Bool);
        full_test(8, Type::U64, Constant(second, 0x8000000000000081ULL), Type::U64);
        full_test(9, Type::U64, Constant(second, 0), Type::U64);
        const OperationMetadata full_logical{
                FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false};
        const auto live_byte_xor = Need(second.Emit<OpCode::Xor>(
                Type::U8,
                {Operand::ValueOf(byte_snapshot), Operand::Immediate(1, Type::U8)},
                full_logical));
        Store(second,
              observations + 224,
              Need(second.Emit<OpCode::BitCast>(Type::U64,
                                                {Operand::ValueOf(live_byte_xor, Type::U64)})));
        Store(second, observations + 232, Snapshot(second, live_byte_xor));
        const auto live_word_xor = Need(second.Emit<OpCode::Xor>(
                Type::U16,
                {Operand::ValueOf(pin_word_snapshot), Operand::Immediate(0x10, Type::U16)},
                full_logical));
        Store(second,
              observations + 240,
              Need(second.Emit<OpCode::BitCast>(Type::U64,
                                                {Operand::ValueOf(live_word_xor, Type::U64)})));
        Store(second, observations + 248, Snapshot(second, live_word_xor));
        Store(second, observations + 88, Load(second, pin14));
        const auto cross_byte = Need(
                second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
        const auto ah_snapshot = Need(second.Emit<OpCode::LoadUniform>(
                Type::U8, {Operand::Uniform(pin14 + 1, Type::U8)}));
        Store(second, pin14, Constant(second, 0xaaaaaaaaaaaaaa55ULL));
        Store(second,
              observations + 80,
              Need(second.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(cross_byte)})));
        Store(second, observations + 104, ah_snapshot);
        const auto cross_word = Need(
                second.Emit<OpCode::LoadUniform>(Type::U16, {Operand::Uniform(pin15, Type::U16)}));
        const auto full_byte = Need(
                second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin15, Type::U8)}));
        Store(second, pin15, full_byte);
        Store(second,
              observations + 96,
              Need(second.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(cross_word)})));
        Need(second.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(pin14, Type::U8), Operand::ValueOf(live_byte_xor)}));
        Need(second.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(pin15, Type::U16), Operand::ValueOf(live_word_xor)}));
        const auto partial_left = Need(
                second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
        const auto partial_right = Need(
                second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin15, Type::U8)}));
        const auto partial_xor = Need(second.Emit<OpCode::Xor>(
                Type::U8,
                {Operand::ValueOf(partial_left), Operand::ValueOf(partial_right)},
                full_logical));
        Need(second.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(pin14, Type::U8), Operand::ValueOf(partial_xor, Type::U8)}));
        Store(second, comparisons + 10 * sizeof(u64), Snapshot(second, raw_source));
        const auto partial_input = Need(
                second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
        const auto partial_add = Need(second.Emit<OpCode::Add>(
                Type::U8, {Operand::ValueOf(partial_input), Operand::Immediate(0xc6, Type::U8)}));
        Need(second.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(pin14, Type::U8), Operand::ValueOf(partial_add, Type::U8)}));
        Store(second, comparisons + 11 * sizeof(u64), Snapshot(second, raw_source));
        const auto madd_left = Need(second.Emit<OpCode::SignExtend>(
                Type::U32,
                {Operand::ValueOf(Need(second.Emit<OpCode::LoadUniform>(
                                          Type::U16, {Operand::Uniform(pin15, Type::U16)})),
                                  Type::U16)}));
        const auto madd_right = Need(second.Emit<OpCode::SignExtend>(
                Type::U32,
                {Operand::ValueOf(Need(second.Emit<OpCode::LoadUniform>(
                                          Type::U16, {Operand::Uniform(pin14, Type::U16)})),
                                  Type::U16)}));
        const auto madd_accumulator = Need(
                second.Emit<OpCode::LoadUniform>(Type::U32, {Operand::Uniform(pin14, Type::U32)}));
        const auto product = Need(second.Emit<OpCode::Mul>(
                Type::U32,
                {Operand::ValueOf(madd_left, Type::U32), Operand::ValueOf(madd_right, Type::U32)}));
        const auto sum =
                Need(second.Emit<OpCode::Add>(Type::U32,
                                              {Operand::ValueOf(product, Type::U32),
                                               Operand::ValueOf(madd_accumulator, Type::U32)}));
        Store(second, comparisons + 12 * sizeof(u64), sum);
        Store(second, comparisons + 13 * sizeof(u64), Snapshot(second, raw_source));
        const auto flagged_product = Need(second.Emit<OpCode::Mul>(
                Type::U32,
                {Operand::ValueOf(madd_left, Type::U32), Operand::ValueOf(madd_right, Type::U32)}));
        (void)Need(second.Emit<OpCode::Add>(Type::U32,
                                            {Operand::ValueOf(flagged_product, Type::U32),
                                             Operand::ValueOf(madd_accumulator, Type::U32)},
                                            metadata));
        Store(second, comparisons + 14 * sizeof(u64), Snapshot(second, raw_source));
        const auto carry_zero = Need(second.Emit<OpCode::TestNotZero>(
                Type::Bool, {Operand::ValueOf(Constant(second, 0x100), Type::U8)}));
        Need(second.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(carry_zero)}));
        Store(second, comparisons + 15 * sizeof(u64), Snapshot(second, raw_source));
        const auto carry_nonzero = Need(second.Emit<OpCode::TestNotZero>(
                Type::Bool, {Operand::ValueOf(Constant(second, 0x101), Type::U8)}));
        Need(second.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(carry_nonzero)}));
        Store(second, comparisons + 16 * sizeof(u64), Snapshot(second, raw_source));
        Need(second.Emit<OpCode::RestoreFlags>(
                Type::Void, {Operand::ValueOf(result_flags), Operand::Flags(Flag::All)}));
        Need(second.EndBlock(Terminal::Return(result)));
        options.entry_pc = next_pc;
        auto second_program = Submit(second);
        const auto second_allocation = arm64::AllocateBlock(second_program, &options);
        Check(bool(second_allocation) &&
                      second_allocation.homes[byte_snapshot.node.value].narrow_pin == 1 &&
                      second_allocation.homes[pin_word_snapshot.node.value].narrow_pin == 2 &&
                      second_allocation.homes[zero_byte_snapshot.node.value].narrow_pin == 1 &&
                      !second_allocation.homes[cross_byte.node.value].narrow_pin &&
                      !second_allocation.homes[cross_word.node.value].narrow_pin &&
                      !second_allocation.homes[ah_snapshot.node.value].narrow_pin &&
                      !second_allocation.homes[full_byte.node.value].narrow_pin,
              "low8/16 pins borrow only their original window; crossings, AH and full publications "
              "keep canonical homes");
        Check(second_allocation.homes[live_byte_xor.node.value].reg != 1 &&
                      second_allocation.homes[live_word_xor.node.value].reg != 1 &&
                      second_allocation.homes[live_byte_xor.node.value].last_use >
                              second_allocation.homes[live_byte_xor.node.value]
                                      .definition_position &&
                      second_allocation.homes[live_word_xor.node.value].last_use >
                              second_allocation.homes[live_word_xor.node.value].definition_position,
              "full narrow logical results stay numerical through raw-wide reads and later partial "
              "publications");
        Check(!second_allocation.homes[partial_xor.node.value].spilled &&
                      second_allocation.homes[partial_xor.node.value].reg != 1 &&
                      second_allocation.homes[partial_left.node.value].narrow_pin == 1 &&
                      second_allocation.homes[partial_right.node.value].narrow_pin == 1,
              "the byte-only publication uses real borrowed sources and an unspilled numeric home");
        Check(!second_allocation.homes[product.node.value].spilled &&
                      !second_allocation.homes[sum.node.value].spilled &&
                      !second_allocation.homes[madd_left.node.value].spilled &&
                      !second_allocation.homes[madd_right.node.value].spilled &&
                      !second_allocation.homes[madd_accumulator.node.value].spilled,
              "the adjacent U32 multiply/add uses actual unspilled numeric inputs and result "
              "homes");
        const auto second_code = arm64::Compile(second_program, options);
        Check(bool(second_code) && !second_code.words.empty(),
              "RawAF linked ADC bridge compilation");
        Check(std::count(second_code.words.begin(), second_code.words.end(), 0x3a00082du) == 3 &&
                      std::count(second_code.words.begin(), second_code.words.end(), 0x3a00482du) ==
                              1,
              "full flags-only narrow immediate comparisons use SETF8/16 while non-encodable word "
              "keeps fallback");
        const auto partial_reg = second_allocation.homes[partial_xor.node.value].reg;
        const auto partial_eor =
                0xca000000u | (second_allocation.homes[partial_right.node.value].reg << 16) |
                (second_allocation.homes[partial_left.node.value].reg << 5) | partial_reg;
        const std::array partial_words{partial_eor, 0x93401c01u | (partial_reg << 5)};
        Check(std::search(second_code.words.begin(),
                          second_code.words.end(),
                          partial_words.begin(),
                          partial_words.end()) != second_code.words.end(),
              "the sole adjacent byte store and PF SXTB each cut their own input without an output "
              "UBFX");
        Check(std::count_if(
                      second_code.words.begin(),
                      second_code.words.end(),
                      [](std::uint32_t word) { return (word & 0xfffffc1fu) == 0xf1000001u; }) >= 2,
              "full-width self TEST uses SUBS into the PF carrier rather than ANDS plus carry "
              "inversion");
        const auto partial_add_reg = second_allocation.homes[partial_add.node.value].reg;
        const std::array partial_add_words{
                0x51000000u | (0x3au << 10) |
                        (second_allocation.homes[partial_input.node.value].reg << 5) |
                        partial_add_reg,
                0xb3401c00u | (partial_add_reg << 5) | 14u};
        Check(std::search(second_code.words.begin(),
                          second_code.words.end(),
                          partial_add_words.begin(),
                          partial_add_words.end()) != second_code.words.end(),
              "the sole byte publication cuts the non-S immediate result without an output UBFX");
        const auto multiply_word = 0x1b000000u |
                                   (second_allocation.homes[madd_right.node.value].reg << 16) |
                                   (second_allocation.homes[madd_left.node.value].reg << 5);
        const auto madd_word = multiply_word |
                               (second_allocation.homes[madd_accumulator.node.value].reg << 10) |
                               second_allocation.homes[sum.node.value].reg;
        const auto flagged_mul_word = multiply_word | (31u << 10) |
                                      second_allocation.homes[flagged_product.node.value].reg;
        Check(std::find(second_code.words.begin(), second_code.words.end(), madd_word) !=
                              second_code.words.end() &&
                      std::find(second_code.words.begin(),
                                second_code.words.end(),
                                flagged_mul_word) != second_code.words.end(),
              "the unflagged adjacent U32 pair uses MADD while the flags-producing Add retains its "
              "separate multiply");

        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        const auto entry = arena.Publish(first_code.words, {}, first_code.links);
        std::array<execution::DispatchSlot, 2> slots{};
        auto& ready =
                slots[(next_pc ^ (next_pc >> arm64::abi::DispatchHashShift)) & (slots.size() - 1)];
        ready.pc = next_pc;
        ready.entry = arena.Publish(second_code.words, {}, second_code.links).entry;
        ready.occupied = true;
        const auto selected = std::find_if(
                first_code.links.begin(), first_code.links.end(), [](const arm64::LinkSite& link) {
                    return link.target_pc == next_pc;
                });
        Check(selected != first_code.links.end(), "selected static edge has a real relocation");
        const auto* executable = reinterpret_cast<const std::uintptr_t*>(
                reinterpret_cast<const std::uint8_t*>(entry.entry) +
                WordOffset(selected->data_offset) * 4);
        Check(Read64(executable) == reinterpret_cast<std::uintptr_t>(entry.entry) +
                                            (WordOffset(selected->data_offset) + 2) * 4,
              "initial relocation addresses the RX cold stub before the mapping becomes "
              "executable");
        auto* writable = entry.writable
                                 ? reinterpret_cast<std::uintptr_t*>(
                                           entry.writable + WordOffset(selected->data_offset) * 4)
                                 : nullptr;
        const auto back_words =
                WordOffset(selected->data_offset) - WordOffset(selected->branch_offset);
        if (writable)
            PatchLinkWords(writable,
                           executable,
                           reinterpret_cast<std::uintptr_t>(ready.entry),
                           back_words);
        ObservedState observed_state;
        auto& context = observed_state.native;
        auto& state = context.state;
        state.pc = entry_pc;
        state.packed_flags = initial;
        state.gprs[9] = 0xbb;
        state.gprs[14] = byte_carrier;
        state.gprs[15] = word_carrier;

        struct Resolver {
            const execution::NativeCode* entry;
            unsigned calls{};
        } resolver{&entry};

        std::atomic<const execution::NativeCode*> active{};
        context.opaque = &resolver;
        context.resolve = [](void* opaque,
                             execution::NativeRunContext* current) noexcept -> std::uintptr_t {
            auto& request = *static_cast<Resolver*>(opaque);
            ++request.calls;
            return request.calls == 1 && current->state.pc == entry_pc
                           ? reinterpret_cast<std::uintptr_t>(request.entry->entry)
                           : 0;
        };
        context.slots = slots.data();
        context.mask = slots.size() - 1;
        context.active = &active;
        context.anchor = &entry;
        context.pinned_uniform_offset = options.pinned_uniform_offset;
        context.flags_offset = options.flags_offset;
        context.pc_offset = options.pc_offset;
        context.pinned_uniform_slots = options.pinned_uniform_slots;
        context.native_nzcv = true;
        Check(execution::SvmRunNative(&context) == 16, "RawAF linked ADC numeric result");
        Check(resolver.calls == 1 && context.dispatcher_entries == (writable ? 0u : 1u) &&
                      context.cache_hits == (writable ? 0u : 1u),
              "a resolved edge bypasses PC setup; W^X cold stub keeps the ordinary ready lookup");
        Check(state.gprs[13] == next_pc,
              "target's actual PC observer materializes entry PC after a direct link");
        Check(state.gprs[0] == 1 && state.gprs[1] == 0,
              "RawAF hot-edge observation and masked import");
        Check(state.gprs[8] == 0xbb && state.gprs[9] == 0xcc && state.gprs[10] == 0xaa &&
                      state.gprs[11] == 0x1111,
              "delayed pins respect intervening reads and partial writes across a real linked "
              "edge");
        const auto produced = ReferenceArithmetic(
                OpCode::Add, 15, 1, 64, metadata.profile, metadata.flag_outputs, initial);
        const auto restored = produced & ~(u64{1} << 26);
        Check(state.gprs[2] == 0 && state.gprs[3] == (restored & ~u64{255}) && state.gprs[4] == 1,
              "raw PF-only and AF-only writes preserve the other channel and whole-word snapshots");
        const auto expected = ReferenceArithmetic(
                OpCode::Add, 15, 1, 64, metadata.profile, metadata.flag_outputs, restored);
        Check(state.packed_flags == expected, "RawAF packed ADC bridge and Return publication");
        constexpr u64 written = (u64{15} << 28) | (u64{1} << 26) | 255;
        Check(state.gprs[5] == 0x80 && state.gprs[6] == ((initial & ~written) | 0x94000080ULL),
              "raw ADC8 consumes carry and publishes overflow, AF and preserved reserved bits");
        Check(state.gprs[7] == 0x7fffffff &&
                      state.gprs[12] == ((initial & ~written) | 0x140000ffULL),
              "raw SBB32 publishes canonical carry/overflow and keeps an older SSA alive across "
              "snapshots");
        Check(Read64(state.xmm) == ((initial & ~written) | 0xd40000a5ULL) &&
                      Read64(state.xmm[0] + 8) == ((initial & ~written) | 0x940000a5ULL),
              "byte/word eager zero TEST preserves N/C/V, raw AF/PF and whole-word reserved flags");
        Check(Read64(state.xmm[1]) == 0xab && Read64(state.xmm[1] + 8) == 0x80 &&
                      Read64(state.xmm[2]) == 0x135,
              "narrow bitwise outputs retain raw-view cuts and wide/compound fallback "
              "normalization");
        Check(Read64(state.xmm[4]) == 0x80 && Read64(state.xmm[4] + 8) == 0x80 &&
                      Read64(state.xmm[7]) == 0x80,
              "borrowed bytes expose canonical SSA carriers to raw/wide reads and cut a physical "
              "bitwise result once");
        Check(Read64(state.xmm[7] + 8) == 0x1080 && Read64(state.xmm[8]) == 0xf100,
              "low8/16 address inputs cannot become an unmasked UXTW index through wider views");
        const auto aligned_add_flags = ReferenceArithmetic(
                OpCode::Add, 0x80, 0x80, 8, metadata.profile, metadata.flag_outputs, initial);
        const auto aligned_sub_flags = ReferenceArithmetic(OpCode::Sub,
                                                           0,
                                                           0xe100,
                                                           16,
                                                           metadata.profile,
                                                           metadata.flag_outputs,
                                                           aligned_add_flags);
        Check(Read64(state.xmm[9]) == 0 && Read64(state.xmm[9] + 8) == aligned_add_flags &&
                      Read64(state.xmm[10]) == 0x1f00 &&
                      Read64(state.xmm[10] + 8) == aligned_sub_flags,
              "aligned byte/word ALU cuts dirty pin inputs, keeps a Bool read narrow and publishes "
              "exact AF/PF/NZCV/reserved bits");
        const auto zero_sub_flags = ReferenceArithmetic(OpCode::Sub,
                                                        0x80,
                                                        0,
                                                        8,
                                                        metadata.profile,
                                                        metadata.flag_outputs,
                                                        aligned_sub_flags);
        const auto byte_cmp_flags = ReferenceArithmetic(OpCode::Sub,
                                                        1,
                                                        1,
                                                        8,
                                                        FlagProfile::ArithmeticCanonicalCarry,
                                                        FlagBits(Flag::Zero),
                                                        zero_sub_flags);
        const auto raw_cmp_flags = ReferenceArithmetic(OpCode::Sub,
                                                       1,
                                                       255,
                                                       8,
                                                       FlagProfile::ArithmeticRaw,
                                                       FlagBits(Flag::Carry | Flag::Zero),
                                                       byte_cmp_flags);
        const auto word_cmp_flags = ReferenceArithmetic(OpCode::Sub,
                                                        0x1000,
                                                        0x1000,
                                                        16,
                                                        FlagProfile::ArithmeticCanonicalCarry,
                                                        FlagBits(Flag::Carry | Flag::Zero),
                                                        raw_cmp_flags);
        const auto borrowed_cmp_flags = ReferenceArithmetic(OpCode::Sub,
                                                            0,
                                                            0xe100,
                                                            16,
                                                            FlagProfile::ArithmeticCanonicalCarry,
                                                            FlagBits(Flag::Carry | Flag::Zero),
                                                            word_cmp_flags);
        Check(Read64(state.xmm[11]) == 0x80 && Read64(state.xmm[11] + 8) == zero_sub_flags,
              "effective zero immediate keeps the full narrow arithmetic flags and raw PF/AF");
        Check(Read64(state.xmm[12]) == byte_cmp_flags &&
                      Read64(state.xmm[12] + 8) == raw_cmp_flags &&
                      Read64(state.xmm[13]) == word_cmp_flags &&
                      Read64(state.xmm[13] + 8) == borrowed_cmp_flags,
              "dead narrow CMP preserves PF/AF/reserved and N/V, handles carry profiles and "
              "retains "
              "borrowed/Bool fallback");
        Check(observed_state.comparisons[0] == ReferenceArithmetic(OpCode::Sub,
                                                                   0x7f,
                                                                   0xff,
                                                                   8,
                                                                   metadata.profile,
                                                                   metadata.flag_outputs,
                                                                   initial) &&
                      observed_state.comparisons[1] == ReferenceArithmetic(OpCode::Sub,
                                                                           0x80,
                                                                           0x7f,
                                                                           8,
                                                                           metadata.profile,
                                                                           metadata.flag_outputs,
                                                                           initial) &&
                      observed_state.comparisons[2] == ReferenceArithmetic(OpCode::Sub,
                                                                           0xff,
                                                                           0,
                                                                           8,
                                                                           metadata.profile,
                                                                           metadata.flag_outputs,
                                                                           initial) &&
                      observed_state.comparisons[3] == ReferenceArithmetic(OpCode::Sub,
                                                                           0x8000,
                                                                           0xffff,
                                                                           16,
                                                                           metadata.profile,
                                                                           metadata.flag_outputs,
                                                                           initial) &&
                      observed_state.comparisons[4] == ReferenceArithmetic(OpCode::Sub,
                                                                           0x8000,
                                                                           0x1234,
                                                                           16,
                                                                           metadata.profile,
                                                                           metadata.flag_outputs,
                                                                           initial),
              "full narrow comparisons preserve exact overflow/borrow, PF/AF/reserved and "
              "non-encodable fallback");
        Check(observed_state.comparisons[5] == ((initial & ~written) | 0x80000080ULL) &&
                      observed_state.comparisons[6] == ((initial & ~written) | 0x80000000ULL) &&
                      observed_state.comparisons[7] == ((initial & ~written) | 0x40000000ULL),
              "full dead byte/word TEST preserves raw PF/AF/NZCV/reserved and respects a narrower "
              "Bool "
              "view");
        Check(observed_state.comparisons[8] == ((initial & ~written) | 0x80000081ULL) &&
                      observed_state.comparisons[9] == ((initial & ~written) | 0x40000000ULL) &&
                      observed_state.comparisons[10] == ((initial & ~written) | 0x80000091ULL),
              "wide self TEST and a sole byte publication preserve all logical flags, PF/AF and "
              "reserved bits");
        constexpr auto product_bits = (u64{0xffffe110} * 0xffffaa57ULL) & 0xffffffffULL;
        Check(observed_state.comparisons[11] == observed_state.comparisons[10] &&
                      observed_state.comparisons[12] ==
                              ((product_bits + 0xaaaaaa57ULL) & 0xffffffffULL) &&
                      observed_state.comparisons[13] == observed_state.comparisons[10] &&
                      observed_state.comparisons[14] == ReferenceArithmetic(OpCode::Add,
                                                                            product_bits,
                                                                            0xaaaaaa57ULL,
                                                                            32,
                                                                            metadata.profile,
                                                                            metadata.flag_outputs,
                                                                            initial),
              "non-S byte publication and MADD preserve old flags; a flagged add keeps its full "
              "arithmetic result flags");
        Check(observed_state.comparisons[15] ==
                              (observed_state.comparisons[14] & ~(u64{1} << 29)) &&
                      observed_state.comparisons[16] ==
                              (observed_state.comparisons[14] | (u64{1} << 29)),
              "adjacent nonzero predicates set only CF from their logical byte view and preserve "
              "NZV, raw PF/AF and reserved bits");
        Check(Read64(state.xmm[14]) == 0x81 &&
                      Read64(state.xmm[14] + 8) == ((initial & ~written) | 0x80000081ULL) &&
                      Read64(state.xmm[15]) == 0xe110 &&
                      Read64(state.xmm[15] + 8) == ((initial & ~written) | 0x80000010ULL),
              "full byte/word XOR keeps canonical numeric results while its signed PF carrier "
              "supplies "
              "exact flags");
        Check(Read64(state.xmm[8] + 8) == ((initial & ~written) | 0xd40000a5ULL),
              "a wider zero predicate tests only the borrowed byte and preserves every other flag");
        Check(Read64(state.xmm[5]) == 0x80 && Read64(state.xmm[5] + 8) == byte_carrier &&
                      Read64(state.xmm[6]) == 0xe100 && Read64(state.xmm[6] + 8) == 0x43 &&
                      state.gprs[14] == 0xaaaaaaaaaaaaaa57ULL && state.gprs[15] == 0xe110,
              "borrowed reads and full publications preserve snapshots; later partial XOR writes "
              "keep "
              "the parent high bits");
        Check((state.packed_flags & ~written) == (initial & ~written),
              "RawAF preserves packed reserved bits");
        if (writable) {
            UnlinkWords(writable, executable, back_words);
            Check(Read64(executable) == reinterpret_cast<std::uintptr_t>(executable + 1) &&
                          *(reinterpret_cast<const std::uint32_t*>(executable) - back_words) ==
                                  (0x58000010u | (back_words << 5)),
                  "unlink restores this actual cold stub and the original LDR");
            state = {};
            state.pc = entry_pc;
            state.packed_flags = initial;
            state.gprs[9] = 0xbb;
            state.gprs[14] = byte_carrier;
            state.gprs[15] = word_carrier;
            resolver.calls = 0;
            context.dispatcher_entries = context.cache_hits = 0;
            Check(execution::SvmRunNative(&context) == 16 && resolver.calls == 1 &&
                          context.dispatcher_entries == 1 && context.cache_hits == 1 &&
                          state.gprs[13] == next_pc && state.packed_flags == expected,
                  "unlinked edge republishes its PC and preserves the raw ABI through the existing "
                  "dispatcher");
        }
    }
#endif
}

void FunctionDeferredTestFaults() {
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        if (!(getauxval(AT_HWCAP) & HWCAP_FLAGM)) return;
        namespace execution = swift::runtime::execution;
        constexpr u64 entry_pc = 0x4000, load_pc = 0x4010, store_pc = 0x4020;
        constexpr u64 initial = 0xa5a50000bc00003cULL, dynamic = 0xf40000ff;
        const auto field = [](unsigned reg) {
            return static_cast<std::uint32_t>(offsetof(swift::runtime::x86::GuestState, gprs) +
                                              reg * sizeof(u64));
        };
        arm64::CompileOptions options;
        options.flags_offset = offsetof(swift::runtime::x86::GuestState, packed_flags);
        options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
        options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.private_uniforms = options.native_nzcv = true;
        options.fault_locations = options.recover_unaligned = true;
        options.deferred_pc = true;
        options.entry_pc = entry_pc;
        options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCV);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
        const OperationMetadata logical{FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false};

        Builder builder{Scope::Function};
        const auto preheader = Create(builder), loop = Create(builder), exit = Create(builder);
        Need(builder.BeginBlock(preheader));
        const auto zero_value = Constant(builder, 0);
        const auto complete = Need(builder.Emit<OpCode::Xor>(
                Type::U64, {Operand::ValueOf(zero_value), Operand::ValueOf(zero_value)}, logical));
        Store(builder, field(swift::runtime::x86::R11), complete);
        Need(builder.EndBlock(Terminal::Jump(loop)));
        Need(builder.BeginBlock(loop));
        const auto incoming = Load(builder, field(swift::runtime::x86::R10));
        Store(builder, field(swift::runtime::x86::R12), incoming);
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(load_pc, Type::U64)}));
        const auto next =
                Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(incoming)}));
        Store(builder, field(swift::runtime::x86::R10), next);
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(store_pc, Type::U64)}));
        const auto previous = Load(builder, field(swift::runtime::x86::R11));
        Need(builder.Emit<OpCode::StoreMemoryTSO>(
                Type::Void, {Operand::ValueOf(incoming), Operand::ValueOf(previous)}));
        (void)Need(builder.Emit<OpCode::And>(
                Type::U64, {Operand::ValueOf(next), Operand::ValueOf(next)}, logical));
        Store(builder, field(swift::runtime::x86::R11), incoming);
        const auto nonzero = Need(
                builder.Emit<OpCode::CondSet>(Type::Bool, {Operand::Condition(Condition::NE)}));
        Need(builder.EndBlock(Terminal::Branch(nonzero, loop, exit)));
        Need(builder.BeginBlock(exit));
        Need(builder.EndBlock(Terminal::Return()));
        auto program = Submit(builder);
        const auto compiled = arm64::Compile(program, options);
        Check(bool(compiled) && compiled.faults.size() == 2 &&
                      std::all_of(compiled.faults.begin(),
                                  compiled.faults.end(),
                                  [](const arm64::FaultSite& site) {
                                      return arm64::DecodeFaultRecovery(site).test_bits == 64 &&
                                             arm64::DecodeFaultRecovery(site).test_register == 12;
                                  }),
              "the real fault operations carry the admitted TEST recipe");
        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        const auto code = arena.Publish(compiled.words, compiled.faults, compiled.links);

        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        auto* memory = static_cast<std::uint8_t*>(mmap(
                nullptr, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        Check(memory != MAP_FAILED, "TEST fault scene mapping");
        const auto head = reinterpret_cast<std::uintptr_t>(memory);
        const auto readonly = reinterpret_cast<std::uintptr_t>(memory + page);
        const auto missing = reinterpret_cast<std::uintptr_t>(memory + page * 2);
        const auto loaded_before_store_fault = head + 0x38;
        Write64(memory + page, loaded_before_store_fault);
        Check(mprotect(memory + page, page, PROT_READ) == 0 &&
                      mprotect(memory + page * 2, page, PROT_NONE) == 0,
              "TEST fault scene permissions");

        // State mutated by native code or siglongjmp lives outside its automatic
        // frame, just as the runtime's Process-owned contexts do.
        struct Scene {
            execution::FaultContext faults;
            execution::NativeRunContext native;
            const execution::NativeCode* code{};
            execution::DispatchSlot slot;
            unsigned resolves{};
        };

        auto scene = std::make_unique<Scene>();
        scene->code = &code;
        scene->faults.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->faults.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->faults.flags_offset = options.flags_offset;
        scene->faults.native_nzcv = true;
        scene->native.opaque = scene.get();
        scene->native.resolve =
                [](void* opaque, execution::NativeRunContext* context) noexcept -> std::uintptr_t {
            auto& value = *static_cast<Scene*>(opaque);
            ++value.resolves;
            return value.resolves == 1 && context->state.pc == entry_pc
                           ? reinterpret_cast<std::uintptr_t>(value.code->entry)
                           : 0;
        };
        scene->native.slots = &scene->slot;
        scene->native.mask = 0;
        scene->native.active = &scene->faults.active;
        scene->native.anchor = &code;
        scene->native.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->native.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->native.flags_offset = options.flags_offset;
        scene->native.pc_offset = options.pc_offset;
        scene->native.native_nzcv = true;
        const auto test_flags = [](u64 value) {
            return (initial & ~dynamic) | (value & 255) | (u64{value == 0} << 30) |
                   ((value >> 63) << 31);
        };
        {
            BindFaultCallbacks(scene->faults);
            execution::FaultScope scope(scene->faults, scene->native.state);
            const auto run = [&](u64 input, u64 expected_pc, u64 expected_flags) {
                scene->native.state = {};
                scene->native.state.pc = entry_pc;
                scene->native.state.packed_flags = initial;
                scene->native.state.gprs[swift::runtime::x86::R10] = input;
                scene->resolves = 0;
                scene->faults.signal_number = 0;
                if (sigsetjmp(scene->faults.jump, 1) == 0) {
                    Check(execution::SvmRunNative(&scene->native) == 0 && !expected_pc,
                          "zero TEST exit returns through the real native ABI");
                } else
                    Check(expected_pc && scene->faults.signal_number == SIGSEGV,
                          "tagged TEST faults return through the registered signal owner");
                Check(scene->resolves == 1 && scene->native.state.packed_flags == expected_flags &&
                              (!expected_pc || scene->native.state.pc == expected_pc),
                      "TEST fault/exit publishes exact flags, reserved bits and PC");
            };
            run(missing, load_pc, test_flags(0));  // First entry is still Complete XOR-zero.
            Write64(memory, missing);
            run(head, load_pc, test_flags(missing));
            Write64(memory, readonly);
            run(head, store_pc, test_flags(readonly));
            Check(scene->native.state.gprs[swift::runtime::x86::R10] == loaded_before_store_fault &&
                          scene->native.state.gprs[swift::runtime::x86::R12] == readonly,
                  "store fault uses the entry snapshot after the load overwrote the current pin");
            Write64(memory, 0);
            run(head, 0, test_flags(0));
            Check(scene->native.state.gprs[swift::runtime::x86::R10] == 0 &&
                          scene->native.state.gprs[swift::runtime::x86::R11] == head,
                  "zero exit completes the current TEST rather than its old nonzero snapshot");
            // Patch both accesses at one writable unaligned node, then exercise
            // the same real recipes on faults from the resulting cold accesses.
            // Cross a 16-byte boundary as well: FEAT_LSE2 can execute some
            // unaligned ordered accesses without raising an alignment fault.
            Write64(memory + 15, 0);
            run(head + 15, 0, test_flags(0));
            Check(scene->faults.unaligned_patches == 2,
                  "one load and one store redirect to their published cold stubs");
            Write64(memory, missing);
            run(head, load_pc, test_flags(missing));
            Check(scene->faults.host_pc ==
                          reinterpret_cast<std::uintptr_t>(code.entry) +
                                  WordOffset(compiled.faults[0].fallback_offset) * 4,
                  "cold load faults retain the original guest PC and TEST snapshot");
            Write64(memory, readonly);
            run(head, store_pc, test_flags(readonly));
            Check(scene->faults.host_pc == reinterpret_cast<std::uintptr_t>(code.entry) +
                                                   (WordOffset(compiled.faults[1].fallback_offset) +
                                                    1) * 4 &&
                          scene->native.state.gprs[swift::runtime::x86::R10] ==
                                  loaded_before_store_fault &&
                          scene->native.state.gprs[swift::runtime::x86::R12] == readonly,
                  "cold store faults use the entry snapshot after the load overwrites its "
                  "destination "
                  "pin");
        }
        Check(munmap(memory, page * 3) == 0, "TEST fault scene release");
    }
#endif
}

void BlockDeferredTestBoundaries() {
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        if (!(getauxval(AT_HWCAP) & HWCAP_FLAGM)) return;
        namespace execution = swift::runtime::execution;
        constexpr u64 entry_pc = 0x7000, exit_pc = 0x7100, load_pc = 0x7010, store_pc = 0x7020;
        constexpr u64 initial = 0xa5a50000bc00003cULL, dynamic = 0xf40000ff;
        const auto field = [](unsigned reg) {
            return static_cast<std::uint32_t>(offsetof(swift::runtime::x86::GuestState, gprs) +
                                              reg * sizeof(u64));
        };
        arm64::CompileOptions options;
        options.flags_offset = offsetof(swift::runtime::x86::GuestState, packed_flags);
        options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
        options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.private_uniforms = options.native_nzcv = true;
        options.fault_locations = options.recover_unaligned = options.deferred_pc = true;
        options.entry_pc = entry_pc;
        options.dispatcher =
                reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCVCounted);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
        const OperationMetadata logical{FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false};

        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        const auto snapshot = Load(builder, field(swift::runtime::x86::R10));
        Store(builder, field(swift::runtime::x86::R12), snapshot);
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(load_pc, Type::U64)}));
        const auto next =
                Need(builder.Emit<OpCode::LoadMemoryTSO>(Type::U64, {Operand::ValueOf(snapshot)}));
        Store(builder, field(swift::runtime::x86::R10), next);
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(store_pc, Type::U64)}));
        const auto previous = Load(builder, field(swift::runtime::x86::R11));
        Need(builder.Emit<OpCode::StoreMemoryTSO>(
                Type::Void, {Operand::ValueOf(snapshot), Operand::ValueOf(previous)}));
        (void)Need(builder.Emit<OpCode::And>(
                Type::U64, {Operand::ValueOf(next), Operand::ValueOf(next)}, logical));
        Store(builder, field(swift::runtime::x86::R11), snapshot);
        const auto condition = Need(
                builder.Emit<OpCode::CondSet>(Type::Bool, {Operand::Condition(Condition::NE)}));
        Need(builder.Emit<OpCode::StoreUniformSelectImm>(
                Type::Void,
                {Operand::Uniform(options.pc_offset, Type::U64),
                 Operand::ValueOf(condition),
                 Operand::Immediate(entry_pc, Type::U64),
                 Operand::Immediate(exit_pc, Type::U64)}));
        Need(builder.EndBlock(Terminal::Dispatch()));
        auto program = Submit(builder);
        const auto allocation = arm64::AllocateBlock(program, &options);
        const auto compiled = arm64::Compile(program, options);
        Check(program.Operations().size() == 12 && bool(allocation) &&
                      !allocation.publish_entry_pc && bool(compiled) && compiled.frame_bytes == 0 &&
                      compiled.spill_slots == 0 && compiled.faults.size() == 2 &&
                      std::all_of(compiled.faults.begin(),
                                  compiled.faults.end(),
                                  [](const arm64::FaultSite& site) {
                                      return arm64::DecodeFaultRecovery(site).test_bits == 64 &&
                                             arm64::DecodeFaultRecovery(site).test_register == 12;
                                  }),
              "the actual 12-IR Block admits a frame-free TEST recipe without publishing entry PC");
        const auto self_link = std::find_if(
                compiled.links.begin(), compiled.links.end(), [](const arm64::LinkSite& link) {
                    return link.target_pc == entry_pc;
                });
        const auto exit_link = std::find_if(
                compiled.links.begin(), compiled.links.end(), [](const arm64::LinkSite& link) {
                    return link.target_pc == exit_pc;
                });
        Check(self_link != compiled.links.end() && exit_link != compiled.links.end(),
              "Block has actual self and zero-exit relocations");
        const auto self_gate = WordOffset(self_link->data_offset) - 2;
        Check(WordOffset(self_link->branch_offset) < self_gate && !(self_gate & 1) &&
                      compiled.words[WordOffset(self_link->branch_offset)] ==
                              (0xb500000au |
                               ((self_gate - WordOffset(self_link->branch_offset)) << 5)) &&
                      compiled.words[self_gate] == 0x58000050u &&
                      compiled.words[self_gate + 1] == 0xd61f0200u,
              "qualified Block has a real CBNZ head and an aligned patchable cold self gate");

        Builder exit_builder{Scope::Block};
        Need(exit_builder.BeginBlock(Create(exit_builder)));
        Need(exit_builder.EndBlock(Terminal::Return()));
        auto exit_program = Submit(exit_builder);
        auto exit_options = options;
        exit_options.entry_pc = exit_pc;
        const auto exit_compiled = arm64::Compile(exit_program, exit_options);
        Check(bool(exit_compiled), "Block zero-exit native target compiles");
        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        const auto code = arena.Publish(compiled.words, compiled.faults, compiled.links);
        const auto exit_code =
                arena.Publish(exit_compiled.words, exit_compiled.faults, exit_compiled.links);
        const auto* words = reinterpret_cast<const std::uint32_t*>(code.entry);
        const auto cold = WordOffset(self_link->data_offset) + 2;
        Check(words[cold] == (0xaa0003e1u | (10u << 16)) && words[cold + 1] == 0x2a0103fdu &&
                      words[cold + 2] == (0xf100001fu | (10u << 5)),
              "unlinked Block self tail completes the actual next TEST and clears the physical tag "
              "before dispatch");

        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        auto* memory = static_cast<std::uint8_t*>(mmap(
                nullptr, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        Check(memory != MAP_FAILED, "Block TEST boundary mapping");
        const auto head = reinterpret_cast<std::uintptr_t>(memory);
        const auto readonly = head + page, missing = head + page * 2;
        const auto loaded_before_store_fault = head + 0x38;
        Write64(memory + page, loaded_before_store_fault);
        Check(mprotect(memory + page, page, PROT_READ) == 0 &&
                      mprotect(memory + page * 2, page, PROT_NONE) == 0,
              "Block TEST boundary permissions");

        struct Scene {
            execution::FaultContext faults;
            execution::NativeRunContext native;
            std::array<execution::DispatchSlot, 2> slots{};
            const execution::NativeCode* code{};
            std::uintptr_t* self_write{};
            const std::uintptr_t* self_execute{};
            std::uint32_t self_back_words{};
            std::uintptr_t code_page{}, protected_page{};
            std::size_t code_pages{}, page{};
            u64 expected_flags{};
            unsigned resolves{};
            bool expect_cold{}, cold_published{};
            volatile sig_atomic_t smc_enabled{}, smc_recovered{};

            bool ProtectCode(bool write) const noexcept {
                return code->writable ||
                       mprotect(reinterpret_cast<void*>(code_page),
                                code_pages,
                                write ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC) == 0;
            }

            bool UnlinkSelf() const noexcept {
                if (!ProtectCode(true)) return false;
                UnlinkWords(self_write, self_execute, self_back_words);
                return ProtectCode(false);
            }
        };

        auto scene = std::make_unique<Scene>();
        scene->code = &code;
        scene->page = page;
        scene->protected_page = readonly;
        const auto begin = reinterpret_cast<std::uintptr_t>(code.entry);
        const auto end = reinterpret_cast<std::uintptr_t>(exit_code.entry) + exit_code.bytes;
        scene->code_page = begin - begin % page;
        scene->code_pages = ((end + page - 1) / page) * page - scene->code_page;
        scene->self_execute = reinterpret_cast<const std::uintptr_t*>(
                begin + WordOffset(self_link->data_offset) * 4);
        scene->self_back_words =
                WordOffset(self_link->data_offset) - WordOffset(self_link->branch_offset);
        auto* write_base = code.writable ? code.writable : reinterpret_cast<std::uint8_t*>(begin);
        scene->self_write = reinterpret_cast<std::uintptr_t*>(
                write_base + WordOffset(self_link->data_offset) * 4);
        const auto* exit_execute = reinterpret_cast<const std::uintptr_t*>(
                begin + WordOffset(exit_link->data_offset) * 4);
        auto* exit_write = reinterpret_cast<std::uintptr_t*>(
                write_base + WordOffset(exit_link->data_offset) * 4);
        Check(scene->ProtectCode(true), "Block conditional far-link write access");
        PatchLinkWords(scene->self_write,
                       scene->self_execute,
                       begin + (std::uintptr_t{1} << 21),
                       scene->self_back_words);
        Check(words[WordOffset(self_link->branch_offset)] ==
                              compiled.words[WordOffset(self_link->branch_offset)] &&
                      words[self_gate] == (0x14000000u | (((1u << 19) - self_gate) & 0x03ffffffu)),
              "imm19 range miss keeps CBNZ on its actual B26 gate");
        PatchLinkWords(scene->self_write,
                       scene->self_execute,
                       begin + (std::uintptr_t{1} << 28),
                       scene->self_back_words);
        Check(words[WordOffset(self_link->branch_offset)] ==
                              compiled.words[WordOffset(self_link->branch_offset)] &&
                      words[self_gate] == 0x58000050u &&
                      Read64(scene->self_execute) == begin + (std::uintptr_t{1} << 28),
              "far gate preserves the original LDR/BR DATA transfer without narrowing link "
              "support");
        UnlinkWords(scene->self_write, scene->self_execute, scene->self_back_words);
        Check(words[WordOffset(self_link->branch_offset)] ==
                              compiled.words[WordOffset(self_link->branch_offset)] &&
                      words[self_gate] == 0x58000050u &&
                      Read64(scene->self_execute) ==
                              reinterpret_cast<std::uintptr_t>(scene->self_execute + 1) &&
                      scene->ProtectCode(false),
              "conditional unlink restores its cold gate and own complete-flags stub");
        const auto ready_self = [&] {
            Check(scene->ProtectCode(true), "Block self-link write access");
            PatchLinkWords(scene->self_write, scene->self_execute, begin, scene->self_back_words);
            Check(scene->ProtectCode(false) &&
                          words[WordOffset(self_link->branch_offset)] ==
                                  (0xb500000au |
                                   (((0u - WordOffset(self_link->branch_offset)) & 0x7ffffu)
                                    << 5)) &&
                          words[self_gate] == (0x14000000u | ((0u - self_gate) & 0x03ffffffu)),
                  "Block ready self links its actual CBNZ directly to native entry while retaining "
                  "the "
                  "fallback gate");
        };
        ready_self();
        Check(scene->ProtectCode(true), "Block exit-link write access");
        PatchLinkWords(exit_write,
                       exit_execute,
                       reinterpret_cast<std::uintptr_t>(exit_code.entry),
                       WordOffset(exit_link->data_offset) - WordOffset(exit_link->branch_offset));
        Check(scene->ProtectCode(false) &&
                      (words[WordOffset(exit_link->branch_offset)] & 0xfc000000u) == 0x14000000u,
              "Block zero exit is an actual near ready branch");
        for (const auto& item :
             {std::pair{entry_pc, code.entry}, std::pair{exit_pc, exit_code.entry}}) {
            auto at = (item.first ^ (item.first >> arm64::abi::DispatchHashShift)) &
                      (scene->slots.size() - 1);
            while (scene->slots[at].occupied) at = (at + 1) & (scene->slots.size() - 1);
            scene->slots[at].pc = item.first;
            scene->slots[at].entry = item.second;
            scene->slots[at].occupied = true;
        }
        scene->native.opaque = scene.get();
        scene->native.resolve =
                [](void* opaque, execution::NativeRunContext* context) noexcept -> std::uintptr_t {
            auto& value = *static_cast<Scene*>(opaque);
            ++value.resolves;
            if (value.resolves == 1 && context->state.pc == entry_pc)
                return reinterpret_cast<std::uintptr_t>(value.code->entry);
            value.cold_published = value.expect_cold && value.resolves == 2 &&
                                   context->state.pc == entry_pc &&
                                   context->state.packed_flags == value.expected_flags;
            return 0;  // A real cold resolver may retire this old arena; never re-enter it here.
        };
        scene->native.mask = scene->slots.size() - 1;
        scene->native.active = &scene->faults.active;
        scene->native.anchor = &code;
        scene->native.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->native.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->native.flags_offset = options.flags_offset;
        scene->native.pc_offset = options.pc_offset;
        scene->native.native_nzcv = true;
        scene->faults.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->faults.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->faults.flags_offset = options.flags_offset;
        scene->faults.native_nzcv = true;
        scene->faults.code_write_opaque = scene.get();
        scene->faults.recover_code_write = [](void* opaque, std::uintptr_t address) noexcept {
            auto& value = *static_cast<Scene*>(opaque);
            if (!value.smc_enabled || address < value.protected_page ||
                address - value.protected_page >= value.page)
                return false;
            if (mprotect(reinterpret_cast<void*>(value.protected_page),
                         value.page,
                         PROT_READ | PROT_WRITE) ||
                !value.UnlinkSelf())
                return false;
            value.smc_enabled = 0;
            value.smc_recovered = 1;
            value.faults.smc_pending = 1;
            __atomic_store_n(&value.native.slots,
                             static_cast<execution::DispatchSlot*>(nullptr),
                             __ATOMIC_RELEASE);
            return true;  // The real fault owner retries this protected store without consuming
                          // TEST state.
        };
        const auto test_flags = [](u64 value) {
            return (initial & ~dynamic) | (value & 255) | (u64{value == 0} << 30) |
                   ((value >> 63) << 31);
        };
        {
            BindFaultCallbacks(scene->faults);
            execution::FaultScope scope(scene->faults, scene->native.state);
            const auto run = [&](u64 input,
                                 u64 fault_pc,
                                 u64 flags,
                                 bool cold_stop = false,
                                 bool smc = false) {
                scene->native.state = {};
                scene->native.state.pc = entry_pc;
                scene->native.state.packed_flags = initial;
                scene->native.state.gprs[swift::runtime::x86::R10] = input;
                scene->expected_flags = flags;
                scene->resolves = 0;
                scene->cold_published = false;
                scene->expect_cold = cold_stop || smc;
                scene->smc_enabled = smc;
                scene->smc_recovered = 0;
                scene->faults.signal_number = 0;
                scene->faults.smc_pending = 0;
                scene->native.slots = cold_stop ? nullptr : scene->slots.data();
                scene->native.dispatcher_entries = scene->native.cache_hits = 0;
                if (sigsetjmp(scene->faults.jump, 1) == 0) {
                    const auto returned = execution::SvmRunNative(&scene->native);
                    Check(!fault_pc && returned == (scene->expect_cold ? 2u : 0u),
                          "Block TEST reaches its actual zero/cold exit");
                } else
                    Check(fault_pc && scene->faults.signal_number == SIGSEGV,
                          "Block TEST fault uses the actual signal owner");
                Check(scene->native.state.pc == (fault_pc             ? fault_pc
                                                 : scene->expect_cold ? entry_pc
                                                                      : exit_pc) &&
                              scene->native.state.packed_flags == flags &&
                              scene->resolves == (scene->expect_cold ? 2u : 1u) &&
                              (!scene->expect_cold || scene->cold_published),
                      "Block TEST boundaries publish exact flags/reserved bits and use the "
                      "real cold resolver only when required");
            };
            run(missing, load_pc, initial);  // No TEST has executed at the first entry.
            Write64(memory, missing);
            run(head, load_pc, test_flags(missing));
            Write64(memory, readonly);
            run(head, store_pc, test_flags(readonly));
            Check(scene->native.state.gprs[swift::runtime::x86::R10] == loaded_before_store_fault &&
                          scene->native.state.gprs[swift::runtime::x86::R12] == readonly,
                  "Block tagged store fault reads R12 after the load has already replaced R10");
            Write64(memory, 0);
            run(head, 0, test_flags(0));
            Check(scene->native.state.gprs[swift::runtime::x86::R10] == 0 &&
                          scene->native.state.gprs[swift::runtime::x86::R11] == head,
                  "Block zero edge completes the current zero TEST");
            Write64(memory + 15, 0);
            run(head + 15, 0, test_flags(0));
            Check(scene->faults.unaligned_patches == 2,
                  "Block's real load/store each reuse the cold alignment fallback once");
            Check(scene->UnlinkSelf(), "Block self unlink");
            Write64(memory, loaded_before_store_fault);
            run(head, 0, test_flags(loaded_before_store_fault), true);
            Check(scene->native.dispatcher_entries == 1 && scene->native.cache_hits == 0,
                  "unlinked self reaches a genuine cold miss with completed TEST flags");
            ready_self();
            Write64(memory, readonly);
            const auto smc_before = scene->faults.smc_write_faults.load(std::memory_order_relaxed);
            run(head, 0, test_flags(loaded_before_store_fault), false, true);
            Check(scene->smc_recovered && scene->faults.smc_write_faults == smc_before + 1 &&
                          !scene->native.slots && scene->native.dispatcher_entries == 1 &&
                          scene->native.cache_hits == 0 && Read64(memory + page) == head &&
                          scene->native.state.gprs[swift::runtime::x86::R10] ==
                                  loaded_before_store_fault &&
                          scene->native.state.gprs[swift::runtime::x86::R12] == readonly,
                  "protected tagged store retries, retires the ready self edge and stops in the "
                  "real "
                  "cold resolver");
        }
        Check(munmap(memory, page * 3) == 0, "Block TEST boundary release");
    }
#endif
}

void LocalFlagsFaults() {
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        if (!(getauxval(AT_HWCAP) & HWCAP_FLAGM)) return;
        namespace execution = swift::runtime::execution;
        constexpr u64 entry_pc = 0x5000, first_pc = 0x5010, second_pc = 0x5020, third_pc = 0x5030;
        constexpr u64 initial = 0xa5a50000bc00003cULL;
        const OperationMetadata metadata{
                FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry, false};
        const auto field = [](unsigned reg) {
            return static_cast<std::uint32_t>(offsetof(swift::runtime::x86::GuestState, gprs) +
                                              reg * sizeof(u64));
        };
        arm64::CompileOptions options;
        options.flags_offset = offsetof(swift::runtime::x86::GuestState, packed_flags);
        options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
        options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.private_uniforms = options.native_nzcv = options.fault_locations =
                options.local_flags = true;
        options.rcpc_imm9 = (getauxval(AT_HWCAP) & HWCAP_ILRCPC) != 0;
        options.recover_unaligned = true;
        options.deferred_pc = true;
        options.entry_pc = entry_pc;
        options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCV);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);

        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        const auto incoming = Load(builder, field(swift::runtime::x86::RAX));
        const auto result = Need(builder.Emit<OpCode::Add>(
                Type::U64,
                {Operand::ValueOf(incoming), Operand::Immediate(1, Type::U64)},
                metadata));
        Store(builder, field(swift::runtime::x86::RAX), result);
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(first_pc, Type::U64)}));
        (void)Need(builder.Emit<OpCode::LoadMemoryTSO>(
                Type::U64, {Operand::ValueOf(Load(builder, field(swift::runtime::x86::R8)))}));
        Store(builder, field(swift::runtime::x86::RAX), Constant(builder, 0x55));
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(second_pc, Type::U64)}));
        const auto half = Need(builder.Emit<OpCode::LoadMemoryTSO>(
                Type::U16, {Operand::ValueOf(Load(builder, field(swift::runtime::x86::R9)))}));
        const auto extended = Need(
                builder.Emit<OpCode::SignExtend>(Type::U32, {Operand::ValueOf(half, Type::U16)}));
        Store(builder, field(swift::runtime::x86::RDX), extended);
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(third_pc, Type::U64)}));
        const auto word = Need(builder.Emit<OpCode::LoadMemoryTSO>(
                Type::U32, {Operand::ValueOf(Load(builder, field(swift::runtime::x86::R11)))}));
        const auto extended_word = Need(
                builder.Emit<OpCode::SignExtend>(Type::U64, {Operand::ValueOf(word, Type::U32)}));
        Store(builder, field(swift::runtime::x86::RCX), extended_word);
        Need(builder.EndBlock(Terminal::Return(Constant(builder, 42))));
        auto program = Submit(builder);
        const auto allocation = arm64::AllocateBlock(program, &options);
        Check(bool(allocation) && allocation.homes[result.node.value].reg == 20 &&
                      !allocation.homes[result.node.value].spilled &&
                      !allocation.homes[result.node.value].narrow_pin,
              "local flags borrow the actual full numeric RAX pin");
        Check(!allocation.homes[half.node.value].spilled &&
                      !allocation.homes[extended.node.value].spilled &&
                      allocation.homes[extended.node.value].reg == 22 &&
                      !allocation.homes[word.node.value].spilled &&
                      !allocation.homes[extended_word.node.value].spilled &&
                      allocation.homes[extended_word.node.value].reg == 21,
              "the adjacent signed loads write their actual published RDX/RCX homes");
        const auto compiled = arm64::Compile(program, options);
        Check(bool(compiled) && compiled.faults.size() == 3 &&
                      compiled.faults[0].guest_pc == first_pc &&
                      arm64::DecodeFaultRecovery(compiled.faults[0]).parity_register == 20 &&
                      arm64::DecodeFaultRecovery(compiled.faults[0]).carry_direct == 1 &&
                      !arm64::DecodeFaultRecovery(compiled.faults[0]).test_bits &&
                      compiled.faults[1].guest_pc == second_pc &&
                      !arm64::DecodeFaultRecovery(compiled.faults[1]).parity_register &&
                      arm64::DecodeFaultRecovery(compiled.faults[1]).carry_direct == 1 &&
                      !arm64::DecodeFaultRecovery(compiled.faults[1]).test_bits &&
                      compiled.faults[2].guest_pc == third_pc &&
                      !arm64::DecodeFaultRecovery(compiled.faults[2]).parity_register &&
                      arm64::DecodeFaultRecovery(compiled.faults[2]).carry_direct == 1 &&
                      !arm64::DecodeFaultRecovery(compiled.faults[2]).test_bits,
              "actual memory sites retain direct carry and materialize PF before its source pin is "
              "overwritten");
        if (options.rcpc_imm9)
            Check((compiled.words[WordOffset(compiled.faults[1].unaligned_offset)] & 0xfffffc1fu) ==
                                  0x59c00016u &&
                          (compiled.words[WordOffset(compiled.faults[2].unaligned_offset)] &
                           0xfffffc1fu) == 0x99800015u,
                  "both real recoverable signed loads target their pins without an intermediate "
                  "unsigned result");
        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        const auto code = arena.Publish(compiled.words, compiled.faults, compiled.links);
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        auto* memory = static_cast<std::uint8_t*>(mmap(
                nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        Check(memory != MAP_FAILED, "local flags fault scene mapping");
        Check(mprotect(memory + page, page, PROT_NONE) == 0, "local flags fault scene permissions");
        const auto valid = reinterpret_cast<std::uintptr_t>(memory), missing = valid + page;
        Write64(memory, 0x8001);
        Write64(memory + 15, 0x8001);
        Write64(memory + 32, 0x80010001);
        Write64(memory + 47, 0x80010001);

        struct Scene {
            execution::FaultContext faults;
            execution::NativeRunContext native;
            const execution::NativeCode* code{};
            execution::DispatchSlot slot;
            unsigned resolves{};
        };

        auto scene = std::make_unique<Scene>();
        scene->code = &code;
        scene->faults.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->faults.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->faults.flags_offset = options.flags_offset;
        scene->faults.native_nzcv = true;
        scene->native.opaque = scene.get();
        scene->native.resolve =
                [](void* opaque, execution::NativeRunContext* context) noexcept -> std::uintptr_t {
            auto& value = *static_cast<Scene*>(opaque);
            ++value.resolves;
            return value.resolves == 1 && context->state.pc == entry_pc
                           ? reinterpret_cast<std::uintptr_t>(value.code->entry)
                           : 0;
        };
        scene->native.slots = &scene->slot;
        scene->native.mask = 0;
        scene->native.active = &scene->faults.active;
        scene->native.anchor = &code;
        scene->native.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->native.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->native.flags_offset = options.flags_offset;
        scene->native.pc_offset = options.pc_offset;
        scene->native.native_nzcv = true;
        const auto expected_flags = ReferenceArithmetic(
                OpCode::Add, ~u64{}, 1, 64, metadata.profile, metadata.flag_outputs, initial);
        {
            BindFaultCallbacks(scene->faults);
            execution::FaultScope scope(scene->faults, scene->native.state);
            const auto run = [&](u64 first_address,
                                 u64 second_address,
                                 u64 third_address,
                                 u64 fault_pc,
                                 u64 expected_rax) {
                scene->native.state = {};
                scene->native.state.pc = entry_pc;
                scene->native.state.packed_flags = initial;
                scene->native.state.gprs[swift::runtime::x86::RAX] = ~u64{};
                scene->native.state.gprs[swift::runtime::x86::RDX] = 0x1234567887654321ULL;
                scene->native.state.gprs[swift::runtime::x86::RCX] = 0xabcdef0011223344ULL;
                scene->native.state.gprs[swift::runtime::x86::R8] = first_address;
                scene->native.state.gprs[swift::runtime::x86::R9] = second_address;
                scene->native.state.gprs[swift::runtime::x86::R11] = third_address;
                scene->resolves = 0;
                scene->faults.signal_number = 0;
                if (sigsetjmp(scene->faults.jump, 1) == 0) {
                    const auto returned = execution::SvmRunNative(&scene->native);
                    Check(!fault_pc && returned == 42,
                          "local flags complete before the real Return42 exit");
                } else
                    Check(fault_pc && scene->faults.signal_number == SIGSEGV,
                          "local PF/carry faults use the registered signal owner");
                Check(scene->resolves == 1 &&
                              scene->native.state.pc == (fault_pc ? fault_pc : entry_pc) &&
                              scene->native.state.gprs[swift::runtime::x86::RAX] == expected_rax &&
                              scene->native.state.packed_flags == expected_flags &&
                              scene->native.state.gprs[swift::runtime::x86::RDX] ==
                                      ((fault_pc == first_pc || fault_pc == second_pc)
                                               ? 0x1234567887654321ULL
                                               : 0xffff8001ULL) &&
                              scene->native.state.gprs[swift::runtime::x86::RCX] ==
                                      (fault_pc ? 0xabcdef0011223344ULL : 0xffffffff80010001ULL),
                      "local flags fault/exit preserves exact PC, current RAX, whole packed flags "
                      "and "
                      "reserved bits");
            };
            run(missing, valid, valid + 32, first_pc, 0);
            run(valid, missing, valid + 32, second_pc, 0x55);
            run(valid, valid, valid + 32, 0, 0x55);
            if (options.rcpc_imm9) {
                run(valid, valid + 15, valid + 47, 0, 0x55);
                Check(scene->faults.unaligned_patches == 2,
                      "signed ordered loads patch only their real fast instructions to "
                      "LDURSH/LDURSW "
                      "plus DMB");
                run(valid, missing, valid + 32, second_pc, 0x55);
                Check(scene->faults.host_pc ==
                              reinterpret_cast<std::uintptr_t>(code.entry) +
                                      WordOffset(compiled.faults[1].fallback_offset) * 4,
                      "the signed cold load retains precise flags, old destination pin and "
                      "original "
                      "guest PC on SIGSEGV");
                run(valid, valid, missing, third_pc, 0x55);
                Check(scene->faults.host_pc ==
                              reinterpret_cast<std::uintptr_t>(code.entry) +
                                      WordOffset(compiled.faults[2].fallback_offset) * 4,
                      "the cold signed word fault preserves its old full pin after the earlier "
                      "signed "
                      "halfword committed");
            }
        }
        Check(munmap(memory, page * 2) == 0, "local flags fault scene release");
    }
#endif
}

void VectorPinBoundaries() {
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        if (!(getauxval(AT_HWCAP) & HWCAP_FLAGM)) return;
        namespace execution = swift::runtime::execution;
        using Vector = std::array<u64, 2>;
        using Bank = std::array<Vector, 16>;
        constexpr u64 entry_pc = 0x6000, fault_pc = 0x6010, next_pc = 0x7000;
        constexpr u64 flags = 0xa5a50000bc00003cULL, partial = 0xfedcba9876543210ULL;
        constexpr Vector delta{0x100000003ULL, 0x200000005ULL};
        constexpr Vector crossing{0x1234567890abcdefULL, 0xfedcba0987654321ULL};
        constexpr Vector last14{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
        constexpr Vector last15{0x8877665544332211ULL, 0x1020304050607080ULL};
        Bank initial{};
        for (unsigned i = 0; i < initial.size(); ++i)
            initial[i] = {0x1122334455667700ULL + i, 0x99aabbccddeeff00ULL + i};
        auto expected = initial;
        expected[2][1] = partial;
        expected[4] = initial[2];
        expected[3] = {initial[3][0] + 2 * delta[0], initial[3][1] + 2 * delta[1]};
        expected[5] = {expected[3][1], expected[4][0]};
        expected[6][1] = crossing[0];
        expected[7][0] = crossing[1];
        expected[14] = last14;
        expected[15] = last15;
        std::array<Vector, 8> live_bits{};
        for (unsigned i = 0; i < live_bits.size(); ++i)
            live_bits[i] = {0x1020304050607080ULL + i, 0x8877665544332200ULL + i};
        const auto field = [](unsigned reg) {
            return static_cast<std::uint32_t>(offsetof(swift::runtime::x86::GuestState, gprs) +
                                              reg * sizeof(u64));
        };
        const auto xmm = [](unsigned slot) {
            return static_cast<std::uint32_t>(offsetof(swift::runtime::x86::GuestState, xmm) +
                                              slot * 16);
        };
        arm64::CompileOptions options;
        options.flags_offset = offsetof(swift::runtime::x86::GuestState, packed_flags);
        options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
        options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.pinned_vector_offset = xmm(0);
        options.private_uniforms = options.native_nzcv = options.fault_locations =
                options.deferred_pc = true;
        options.entry_pc = entry_pc;
        options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCV);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);

        Builder builder{Scope::Function};
        const auto first = Create(builder), second = Create(builder);
        const auto vector_constant = [&](Vector bits) {
            return Need(
                    builder.Emit<OpCode::VecLoadConst>(Type::V128,
                                                       {Operand::Immediate(bits[0], Type::U64),
                                                        Operand::Immediate(bits[1], Type::U64)}));
        };
        const auto store_vector = [&](std::uint32_t offset, Value value) {
            Need(builder.Emit<OpCode::StoreUniform>(
                    Type::Void, {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value)}));
        };
        Need(builder.BeginBlock(first));
        const auto old = Need(builder.Emit<OpCode::LoadUniform>(
                Type::V128, {Operand::Uniform(xmm(2), Type::V128)}));
        Need(builder.EndBlock(Terminal::Jump(second)));
        Need(builder.BeginBlock(second));
        Store(builder, xmm(2) + 8, Constant(builder, partial));
        store_vector(xmm(4), old);
        const auto original3 = Need(builder.Emit<OpCode::LoadUniform>(
                Type::V128, {Operand::Uniform(xmm(3), Type::V128)}));
        const auto increment = vector_constant(delta);
        const auto add = [&](Value left) {
            return Need(builder.Emit<OpCode::VecAdd>(Type::V128,
                                                     {Operand::ValueOf(left),
                                                      Operand::ValueOf(increment),
                                                      Operand::Immediate(64, Type::U8)}));
        };
        const auto added = add(original3);
        store_vector(xmm(3), added);
        const auto added_again = add(added);
        store_vector(xmm(3), added_again);
        const auto joined = Need(builder.Emit<OpCode::LoadUniform>(
                Type::V128, {Operand::Uniform(xmm(3) + 8, Type::V128)}));
        store_vector(xmm(5), joined);
        store_vector(xmm(6) + 8, vector_constant(crossing));
        store_vector(xmm(14), vector_constant(last14));
        store_vector(xmm(15), vector_constant(last15));

        std::array<Value, 8> live{};
        for (unsigned i = 0; i < live.size(); ++i) live[i] = vector_constant(live_bits[i]);
        const auto callback = +[](u64 leaf, u64 subleaf) noexcept -> QueryPair {
            asm volatile(
                    "movi v2.16b, #0x5a\n"
                    "movi v10.16b, #0x6b\n"
                    "movi v11.16b, #0x7c\n"
                    "movi v30.16b, #0x8d\n"
                    "movi v31.16b, #0x9e\n" ::
                            : "v2", "v10", "v11", "v30", "v31");
            return {leaf | (subleaf << 32), 0};
        };
        (void)Need(builder.Emit<OpCode::Cpuid>(
                Type::U64,
                {Operand::ValueOf(Constant(builder, 3)),
                 Operand::ValueOf(Constant(builder, 5)),
                 Operand::Immediate(reinterpret_cast<std::uintptr_t>(callback), Type::U64)}));
        const auto scratch = Load(builder, field(swift::runtime::x86::RDI));
        for (unsigned i = 0; i < live.size(); ++i)
            Need(builder.Emit<OpCode::StoreMemory>(Type::Void,
                                                   {Operand::Expression(OperandOp::Add, 2),
                                                    Operand::ValueOf(scratch),
                                                    Operand::Immediate(i * 16, Type::U64),
                                                    Operand::ValueOf(live[i])}));
        Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                                 {Operand::Immediate(fault_pc, Type::U64)}));
        (void)Need(builder.Emit<OpCode::LoadMemory>(
                Type::U64, {Operand::ValueOf(Load(builder, field(swift::runtime::x86::R8)))}));
        Store(builder, options.pc_offset, Load(builder, field(swift::runtime::x86::RAX)));
        Need(builder.EndBlock(Terminal::Dispatch()));
        auto program = Submit(builder);
        const auto allocation = arm64::AllocateFunction(program, &options);
        Check(bool(allocation) &&
                      std::count_if(live.begin(),
                                    live.end(),
                                    [&](Value value) {
                                        const auto& home = allocation.homes[value.node.value];
                                        return !home.spilled && home.reg >= 10 && home.reg <= 15;
                                    }) >= 2,
              "live vector constants exercise complete Q homes in the new dynamic bank");
        const auto compiled = arm64::Compile(program, options);
        Check(bool(compiled) && compiled.faults.size() == 1 &&
                      compiled.faults.front().guest_pc == fault_pc && compiled.links.empty(),
              "vector boundary unit has one actual fatal access and a real dynamic PC tail");
        Builder target{Scope::Block};
        Need(target.BeginBlock(Create(target)));
        Need(target.EndBlock(Terminal::Return(Constant(target, 42))));
        auto target_program = Submit(target);
        options.entry_pc = next_pc;
        const auto target_code = arm64::Compile(target_program, options);
        Check(bool(target_code), "linked vector target compilation");
        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        const auto code = arena.Publish(compiled.words, compiled.faults, compiled.links);
        const auto target_entry =
                arena.Publish(target_code.words, target_code.faults, target_code.links);
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        auto* memory = static_cast<std::uint8_t*>(mmap(
                nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        Check(memory != MAP_FAILED && mprotect(memory + page, page, PROT_NONE) == 0,
              "vector boundary fault scene mapping");

        struct Scene {
            execution::FaultContext faults;
            execution::NativeRunContext native;
            execution::DispatchSlot slot;
            execution::NativeCode anchor, source, target;
            Bank initial{}, expected{};
            Vector replacement{0x13579bdf2468ace0ULL, 0xfdb97531eca86420ULL};
            alignas(16) std::array<Vector, 8> observed{};
            unsigned resolves{};
            bool fail_next{}, root_published{}, cold_published{};
        };

        auto scene = std::make_unique<Scene>();
        scene->source = scene->anchor = code;
        scene->target = target_entry;
        scene->initial = initial;
        scene->faults.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->faults.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->faults.pinned_vector_offset = options.pinned_vector_offset;
        scene->faults.flags_offset = options.flags_offset;
        scene->faults.native_nzcv = true;
        scene->native.opaque = scene.get();
        scene->native.resolve =
                [](void* opaque, execution::NativeRunContext* context) noexcept -> std::uintptr_t {
            auto& value = *static_cast<Scene*>(opaque);
            ++value.resolves;
            if (value.resolves == 1 && context->state.pc == entry_pc) {
                value.root_published = std::memcmp(value.native.state.xmm,
                                                   value.initial.data(),
                                                   sizeof(value.native.state.xmm)) == 0;
                value.anchor = value.source;
                return reinterpret_cast<std::uintptr_t>(value.source.entry);
            }
            if (value.resolves != 2 || context->state.pc != next_pc) return 0;
            value.cold_published = std::memcmp(value.native.state.xmm,
                                               value.expected.data(),
                                               sizeof(value.native.state.xmm)) == 0;
            std::memcpy(value.native.state.xmm[0], value.replacement.data(), sizeof(Vector));
            value.expected[0] = value.replacement;
            asm volatile(
                    "movi v16.16b, #0xa1\n"
                    "movi v30.16b, #0xb2\n"
                    "movi v31.16b, #0xc3\n" ::
                            : "v16", "v30", "v31");
            value.anchor = value.target;
            return value.fail_next ? 0 : reinterpret_cast<std::uintptr_t>(value.target.entry);
        };
        scene->native.slots = &scene->slot;
        scene->native.mask = 0;
        scene->native.active = &scene->faults.active;
        scene->native.anchor = &scene->anchor;
        scene->native.pinned_uniform_offset = options.pinned_uniform_offset;
        scene->native.pinned_uniform_slots = options.pinned_uniform_slots;
        scene->native.pinned_vector_offset = options.pinned_vector_offset;
        scene->native.flags_offset = options.flags_offset;
        scene->native.pc_offset = options.pc_offset;
        scene->native.native_nzcv = true;
        {
            BindFaultCallbacks(scene->faults);
            execution::FaultScope scope(scene->faults, scene->native.state);
            for (unsigned mode = 0; mode < 3; ++mode) {
                scene->native.state = {};
                scene->native.state.pc = entry_pc;
                scene->native.state.packed_flags = flags;
                std::memcpy(
                        scene->native.state.xmm, initial.data(), sizeof(scene->native.state.xmm));
                scene->native.state.gprs[swift::runtime::x86::RAX] = next_pc;
                scene->native.state.gprs[swift::runtime::x86::RDI] =
                        reinterpret_cast<std::uintptr_t>(scene->observed.data());
                scene->native.state.gprs[swift::runtime::x86::R8] =
                        reinterpret_cast<std::uintptr_t>(memory + (mode ? 0 : page));
                scene->expected = expected;
                scene->observed = {};
                scene->resolves = 0;
                scene->fail_next = mode == 2;
                scene->root_published = scene->cold_published = false;
                scene->faults.signal_number = 0;
                if (sigsetjmp(scene->faults.jump, 1) == 0) {
                    const auto returned = execution::SvmRunNative(&scene->native);
                    Check(mode && returned == (mode == 2 ? 2u : 42u),
                          "vector pins survive cold success and failure Return reasons");
                } else
                    Check(!mode && scene->faults.signal_number == SIGSEGV,
                          "vector pins recover through the actual fatal owner");
                Check(scene->root_published && scene->resolves == (mode ? 2u : 1u) &&
                              (!mode || scene->cold_published) &&
                              scene->native.state.pc == (mode ? next_pc : fault_pc) &&
                              scene->native.state.packed_flags == flags,
                      "root/cold/fatal boundaries publish the real PC and preserve whole packed "
                      "flags");
                Check(std::memcmp(scene->native.state.xmm,
                                  scene->expected.data(),
                                  sizeof(scene->native.state.xmm)) == 0,
                      "all XMM bits survive old SSA, partial/cross-slot aliases, helper clobbers "
                      "and "
                      "cold reload");
                Check(scene->observed == live_bits,
                      "helper preserves low and high halves of all live dynamic vector homes");
            }
        }
        Check(munmap(memory, page * 2) == 0, "vector boundary scene release");
    }
#endif
}

}  // namespace

TEST_CASE("FaultMetadataAndRecovery", "[compile][native][arm64]") {
    CHECK_NOTHROW(FaultMetadataAndRecovery());
}

TEST_CASE("FaultLocationBoundaries", "[compile][native][arm64]") {
    CHECK_NOTHROW(FaultLocationBoundaries());
}

TEST_CASE("LinkedRawAuxiliaryCarry", "[arm64][native]") {
    CHECK_NOTHROW(LinkedRawAuxiliaryCarry());
}

TEST_CASE("FunctionDeferredTestFaults", "[arm64][native]") {
    CHECK_NOTHROW(FunctionDeferredTestFaults());
}

TEST_CASE("BlockDeferredTestBoundaries", "[arm64][native]") {
    CHECK_NOTHROW(BlockDeferredTestBoundaries());
}

TEST_CASE("LocalFlagsFaults", "[arm64][native]") { CHECK_NOTHROW(LocalFlagsFaults()); }

TEST_CASE("VectorPinBoundaries", "[arm64][native]") { CHECK_NOTHROW(VectorPinBoundaries()); }
