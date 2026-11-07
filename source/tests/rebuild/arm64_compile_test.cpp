#include "runtime/backend/arm64/compile.h"
#include "runtime/backend/arm64/abi.h"
#include "runtime/backend/arm64/patch.h"
#include "runtime/arch/x86/x87.h"
#include "runtime/ir/builder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__aarch64__) && defined(__ANDROID__)
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#include "runtime/execution/code_heap.h"
#include "runtime/execution/code_cache.h"
#include "runtime/platform/linux/arm64_runtime.h"
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using namespace swift::runtime::ir;
namespace arm64 = swift::runtime::backend::arm64;
using u64 = std::uint64_t;

constexpr std::uint32_t WordOffset(swift::runtime::backend::NativeOffset offset) noexcept {
    return offset == swift::runtime::backend::NoNativeOffset ? UINT32_MAX : offset / 4;
}
#if defined(__aarch64__) && defined(__ANDROID__)
const swift::runtime::execution::HostBackendOps& TestBackendOps() {
    static swift::runtime::platform::Arm64Runtime binding{false};
    static const auto operations = binding.Bind();
    return operations;
}

void BindFaultCallbacks(swift::runtime::execution::FaultContext& context) {
    context.patch_ordered = TestBackendOps().patch_fault;
    context.recover_native = TestBackendOps().recover_native;
}

void PatchLinkWords(std::uintptr_t* write,
                    const std::uintptr_t* execute,
                    std::uintptr_t target,
                    std::uint32_t back) {
    arm64::PatchNativeLink(write, execute, target, back * 4);
}

void UnlinkWords(std::uintptr_t* write, const std::uintptr_t* execute, std::uint32_t back) {
    arm64::UnlinkNativeLink(write, execute, back * 4);
}
#endif

std::size_t checks{};

void Check(bool okay, const char* message) {
    ++checks;
    if (!okay) throw std::runtime_error(message);
}

void Need(Error error) { Check(error == Error::None, "IR lifecycle rejected"); }

Value Need(AppendResult result) {
    Check(bool(result), "IR append rejected");
    return result.GetValue();
}

BlockRef Create(Builder& builder) {
    const auto result = builder.CreateBlock();
    Check(bool(result), "block creation rejected");
    return result.block;
}

Program Submit(Builder& builder) {
    auto result = builder.Submit();
    Check(bool(result), "Submit rejected");
    return std::move(result.program);
}

Value Constant(Builder& builder, u64 value) {
    return Need(builder.Emit<OpCode::LoadImm>(Type::U64, {Operand::Immediate(value, Type::U64)}));
}

Value Load(Builder& builder, std::uint32_t offset) {
    return Need(
            builder.Emit<OpCode::LoadUniform>(Type::U64, {Operand::Uniform(offset, Type::U64)}));
}

void Store(Builder& builder, std::uint32_t offset, Value value) {
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(offset, Type::U64), Operand::ValueOf(value)}));
}

[[maybe_unused]] u64 Read64(const void* data) {
    u64 value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void Write64(void* data, u64 value) { std::memcpy(data, &value, sizeof(value)); }

arm64::CompileResult Compile(const Program& program, arm64::CompileOptions options = {}) {
    auto result = arm64::Compile(program, options);
    Check(bool(result) && !result.words.empty(), "native writer rejected supported IR");
    Check(result.frame_bytes >= 16 && result.frame_bytes % 16 == 0 &&
                  result.spill_slots * 8 <= result.frame_bytes - 16,
          "unaligned native frame");
    Check(result.words.front() == 0xa9bf7bf3 && result.words[1] == 0xaa0003f3,
          "state/callee-save prologue");
    return result;
}

void Execute(const arm64::CompileResult&, std::uint8_t*, u64);

Value Snapshot(Builder& builder, Value anchor) {
    return Need(builder.Emit<OpCode::GetFlags>(
            Type::U64, {Operand::ValueOf(anchor), Operand::Flags(Flag::All)}));
}

u64 ReferenceArithmetic(OpCode op,
                        u64 left,
                        u64 right,
                        unsigned width,
                        FlagProfile profile,
                        u64 mask,
                        u64 previous) {
    const u64 word_mask = width == 64 ? ~u64{} : (u64{1} << width) - 1;
    left &= word_mask;
    right &= word_mask;
    if (op == OpCode::Neg) {
        right = left;
        left = 0;
    }
    const bool subtract = op != OpCode::Add;
    const u64 result = (subtract ? left - right : left + right) & word_mask;
    const bool raw_carry = subtract
                                   ? left >= right
                                   : ((static_cast<unsigned __int128>(left) + right) >> width) != 0;
    const auto sign = static_cast<__int128>(1) << (width - 1);
    const auto modulus = static_cast<__int128>(1) << width;
    const auto signed_left =
            static_cast<__int128>(left) >= sign ? static_cast<__int128>(left) - modulus : left;
    const auto signed_right =
            static_cast<__int128>(right) >= sign ? static_cast<__int128>(right) - modulus : right;
    const auto signed_result = subtract ? signed_left - signed_right : signed_left + signed_right;
    const auto set = [&](Flag flag, unsigned bit, bool value) {
        if (!(mask & FlagBits(flag))) return;
        previous = (previous & ~(u64{1} << bit)) | (u64{value} << bit);
    };
    set(Flag::Carry,
        29,
        raw_carry ^ (subtract && profile == FlagProfile::ArithmeticCanonicalCarry));
    set(Flag::Overflow, 28, signed_result < -sign || signed_result >= sign);
    set(Flag::Zero, 30, result == 0);
    set(Flag::Negate, 31, (result >> (width - 1)) != 0);
    set(Flag::AuxiliaryCarry, 26, ((left ^ right ^ result) & 16) != 0);
    if (mask & FlagBits(Flag::Parity)) previous = (previous & ~u64{255}) | (result & 255);
    return previous;
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
    Execute(code, state.data(), (flags >> 30) & 1);
#if defined(__aarch64__) && defined(__ANDROID__)
    for (std::size_t i = 0; i < cases.size(); ++i) {
        Check(Read64(state.data() + i * 16) == expected_values[i], "flag-setting numeric result");
        Check(Read64(state.data() + i * 16 + 8) == expected_flags[i],
              "raw/canonical/partial packed flags");
        Check(Read64(state.data() + cases.size() * 16 + i * 8) == ((expected_flags[i] >> 30) & 1),
              "CondSet restores NZCV after CMP");
    }
    Check(Read64(state.data() + 240) == flags, "return publishes packed flags");
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
    Execute(code, state.data(), replacement);
#if defined(__aarch64__) && defined(__ANDROID__)
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
    const auto pending = ReferenceArithmetic(
            OpCode::Add, 0x1234, 1, 64, FlagProfile::ArithmeticRaw, FlagBits(Flag::All), partial);
    Check(Read64(state.data() + 96) == pending,
          "memory read sees pending guest flags before fault point");
    Check(Read64(state.data() + 104) == replacement && Read64(state.data() + slot) == replacement,
          "memory store alias reload/raw GetFlags");
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
    Execute(code, state.data(), 16);
#if defined(__aarch64__) && defined(__ANDROID__)
    const auto expected = ReferenceArithmetic(
            OpCode::Add, 15, 1, 64, FlagProfile::ArithmeticRaw, FlagBits(Flag::All), initial);
    Check(memory == 16 && Read64(state.data() + 8) == expected &&
                  Read64(state.data() + slot) == expected,
          "flags helpers preserve spilled numeric result and unaligned store input");
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
    Execute(code, state.data(), 0x80);
#if defined(__aarch64__) && defined(__ANDROID__)
    for (unsigned i = 0; i < expected.size(); ++i)
        Check(Read64(state.data() + i * 8) == expected[i], "legacy exact narrow flags pair");
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

void Execute(const arm64::CompileResult& code, std::uint8_t* state, u64 expected) {
#if defined(__aarch64__) && defined(__ANDROID__)
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    Check(page && page != static_cast<std::size_t>(-1), "page size unavailable");
    const auto bytes = (code.words.size() * 4 + page - 1) / page * page;
    void* address =
            mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(address != MAP_FAILED, "native publication mmap");

    struct Mapping {
        void* address;
        std::size_t bytes;

        ~Mapping() { munmap(address, bytes); }
    } mapping{address, bytes};

    std::memcpy(address, code.words.data(), code.words.size() * 4);
    Check(mprotect(address, bytes, PROT_READ | PROT_EXEC) == 0, "native publication W^X");
    __builtin___clear_cache(static_cast<char*>(address),
                            static_cast<char*>(address) + code.words.size() * 4);
    const auto function = reinterpret_cast<u64 (*)(std::uint8_t*)>(address);
    Check(function(state) == expected, "executed native result");
#else
    (void)code;
    (void)state;
    (void)expected;
#endif
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
    Execute(code, state.data(), 0xffffffc0ULL);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(Read64(state.data()) == 0x80 && Read64(state.data() + 8) == 0xffffffffffffff80ULL,
          "conversion consumer views");
    Check(Read64(state.data() + 16) == 0xffffffff80000080ULL, "BitCast retains raw carrier bits");
    Check(Read64(state.data() + 24) == 0 && Read64(state.data() + 32) == 0x08000008 &&
                  Read64(state.data() + 40) == 0x08000008,
          "immediate shifts keep their natural width and masked count");
    Check(Read64(state.data() + 48) == (0xffffffff80000080ULL & 0xffffffff0000ffffULL) &&
                  Read64(state.data() + 56) == (0x80000080u | 0x00ff00ffu) &&
                  Read64(state.data() + 64) == (0xffffffff80000080ULL ^ 0x0123456789abcdefULL) &&
                  Read64(state.data() + 72) == (0xffffffff80000080ULL & ~u64{0xf0}),
          "logical constants preserve full and narrow operand views");
    Check(Read64(state.data() + 80) == UINT32_MAX && Read64(state.data() + 88) == UINT64_MAX &&
                  Read64(state.data() + 96) == 0x80000080ULL,
          "negative constants and flag-setting logical masks");
    Check(Read64(state.data() + 112) == 0x80 && Read64(state.data() + 120) == 0x80000080 &&
                  Read64(state.data() + 128) == 0x80000081,
          "word expressions truncate the result without masking their input carriers");
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
    Execute(code, state.data(), 12);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(memory[2] == 12 && Read64(state.data() + 8) == 12, "state/TSO address semantics");
    Write64(state.data() + 16, reinterpret_cast<std::uintptr_t>(memory.data()) + 1);
    Execute(code, state.data(), 12);
    Check(Read64(reinterpret_cast<const std::uint8_t*>(memory.data()) + 17) == 12,
          "unaligned TSO fallback remains accessible");
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
    Execute(code, state.data(), expected);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(Read64(state.data() + 65537) == 0x1234000000000000ULL,
          "large unaligned uniform spill store");
    Check(Read64(state.data() + 65521) == 0 &&
                  Read64(state.data() + 65529) == 0x1234000000000000ULL,
          "Goto skips the body and saves its complete spilled numeric result before the taken "
          "edge");
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
    Execute(code, state.data(), 1001);
#if defined(__aarch64__) && defined(__ANDROID__)
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
    Execute(code, state.data(), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(Read64(state.data()) == 0x123456789abcdef0ULL && Read64(state.data() + 8) == 0,
          "empty-header loop preserves invariant on its second iteration");
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
        Execute(code, state.data(), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
        Check(Read64(state.data()) == 0x123456789abcdef0ULL && Read64(state.data() + 8) == 0,
              "reordered acyclic/loop body must preserve the dominating value");
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
    Execute(code, state.data(), 8);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(Read64(state.data()) == 8, "nonzero entry skips the unrelated block-zero body");
#endif
}

u64 Mask(unsigned bits) { return bits == 64 ? ~u64{} : bits ? (u64{1} << bits) - 1 : 0; }

void StoreCarrier(Builder& builder, std::uint32_t offset, Value value) {
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(offset, Type::U64), Operand::ValueOf(value, Type::U64)}));
}

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
    Execute(code, state.data(), expected.back());
#if defined(__aarch64__) && defined(__ANDROID__)
    for (std::size_t i = 0; i < expected.size(); ++i)
        Check(Read64(state.data() + i * 8) == expected[i], "pure integer raw-carrier/width oracle");
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
    Execute(code, reinterpret_cast<std::uint8_t*>(&observed), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(observed.initialized == 0 && observed.poison == 0x55 && observed.fpu.control == 0x037f &&
                  observed.fpu.status == 0x3800 && observed.fpu.tags == 0x3fff &&
                  observed.fpu.result == 0x3800 &&
                  observed.fpu.registers[7].significand == UINT64_C(0x8000000000000000) &&
                  observed.fpu.registers[7].sign_exp == 0x3fff,
          "dead x87 status results retain state effects and live status results remain zero");
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
    Execute(code, state.data(), a * (b + 12));
#if defined(__aarch64__) && defined(__ANDROID__)
    const auto high_unsigned = static_cast<u64>((static_cast<unsigned __int128>(a) * b) >> 64);
    const __int128 signed_a = static_cast<__int128>(a) - (static_cast<__int128>(1) << 64);
    const auto high_signed = static_cast<u64>(static_cast<unsigned __int128>(signed_a * b) >> 64);
    Check(Read64(state.data()) == a * (b + 12) && Read64(state.data() + 8) == b,
          "compound Mul/full-width insert with two spilled inputs");
    Check(Read64(state.data() + 16) == high_unsigned && Read64(state.data() + 24) == high_signed &&
                  Read64(state.data() + 32) == 528,
          "signed/unsigned raw MulHigh and preserved pressure values");
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
        Execute(alias_code, alias_state.data(), expected);
#if defined(__aarch64__) && defined(__ANDROID__)
        Check(Read64(alias_state.data()) == expected && Read64(alias_state.data() + 8) == a,
              "X raw insertion preserves high source bits and W insertion clears the result's high "
              "half");
        for (std::size_t i = 0; i < live.size(); ++i)
            Check(Read64(alias_state.data() + 16 + i * 8) == 100 + i,
                  "insertion last-read reuse preserves the full live bank");
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
    Execute(code, nullptr, 42);
    for (const auto terminal :
         {Terminal::Return(), Terminal::Dispatch(), Terminal::Unreachable()}) {
        Builder single{Scope::Block};
        Need(single.BeginBlock(Create(single)));
        Need(single.EndBlock(terminal));
        auto empty = Submit(single);
        auto native = Compile(empty);
        if (terminal.kind == TerminalKind::Unreachable)
            Check(native.words.back() == 0xd4200000, "Unreachable must trap");
        else
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
    Execute(code, state.data(), 11);
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
    Execute(code, state.data(), 11);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(Read64(state.data()) == 1, "a numeric consumer keeps the complete boolean value");
#endif
}

void PCTailPredicates() {
    const auto source = [](OpCode opcode, u64 argument, unsigned other_use = 0) {
        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        const auto one = other_use == 1 ? Constant(builder, 1) : Value{};
        const auto condition = Need(
                builder.Append(opcode,
                               Type::Bool,
                               {opcode == OpCode::CondSet
                                        ? Operand::Condition(static_cast<std::uint32_t>(argument))
                                        : Operand::Flags(argument)}));
        if (other_use == 1)
            Need(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(one)}));
        if (other_use == 2)
            (void)Need(builder.Emit<OpCode::TestZero>(Type::Bool, {Operand::ValueOf(condition)}));
        Need(builder.Emit<OpCode::StoreUniformSelectImm>(Type::Void,
                                                         {Operand::Uniform(128, Type::U64),
                                                          Operand::ValueOf(condition),
                                                          Operand::Immediate(0x2000, Type::U64),
                                                          Operand::Immediate(0x3000, Type::U64)}));
        Need(builder.EndBlock(Terminal::Dispatch()));
        return std::pair{Submit(builder), condition.node};
    };
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.pc_offset = 128;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.native_nzcv = true;
    for (const auto condition : {Condition::EQ, Condition::AL, Condition::NV}) {
        auto [program, predicate] = source(OpCode::CondSet, static_cast<u64>(condition));
        const auto allocation = arm64::AllocateBlock(program, &options);
        const auto tail = program.Operations().size() - 1;
        Check(bool(allocation) && allocation.homes[predicate.value].terminal_only &&
                      allocation.homes[predicate.value].reg == 0 &&
                      !allocation.homes[predicate.value].spilled &&
                      allocation.homes[predicate.value].last_use ==
                              allocation.homes[tail].definition_position,
              "sole PC-tail predicate retains its actual use without a scalar home");
        const auto code = arm64::Compile(program, options);
        Check(bool(code) && code.links.size() == (condition == Condition::EQ ? 2u : 1u) &&
                      code.links.front().target_pc == 0x2000,
              "PC-tail uses two real edges; AL and NV both select the true edge");
        Check(std::none_of(code.words.begin(),
                           code.words.end(),
                           [](std::uint32_t word) {
                               return (word & 0xffff0fe0u) == 0x9a9f07e0u ||
                                      (word & 0xff00001fu) == 0xb4000010u;
                           }),
              "direct PC predicate has no materialized CSET or copied boolean CBZ");
        const auto ordinary = arm64::AllocateBlock(program);
        Check(bool(ordinary) && !ordinary.homes[predicate.value].terminal_only,
              "ordinary PC stores do not borrow a linked-only predicate");
    }
    for (const auto mask : {FlagBits(Flag::Carry | Flag::Zero),
                            FlagBits(Flag::AuxiliaryCarry),
                            FlagBits(Flag::Parity)}) {
        auto [program, predicate] = source(OpCode::TestFlags, mask);
        const auto allocation = arm64::AllocateBlock(program, &options);
        const auto code = arm64::Compile(program, options);
        Check(bool(allocation) && bool(code) &&
                      allocation.homes[predicate.value].terminal_only ==
                              (mask != FlagBits(Flag::Parity)),
              "native C-or-Z and AF use direct edges; PF retains its numeric reader");
    }
    for (unsigned other_use : {1u, 2u}) {
        auto [program, predicate] =
                source(OpCode::CondSet, static_cast<u64>(Condition::CS), other_use);
        const auto allocation = arm64::AllocateBlock(program, &options);
        Check(bool(allocation) && !allocation.homes[predicate.value].terminal_only &&
                      bool(arm64::Compile(program, options)),
              "intervening flag writers and an extra pure use keep the original predicate "
              "snapshot");
    }
}

void DeferredPCLinks() {
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.pc_offset = 128;
    options.pinned_uniform_offset = 0;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.private_uniforms = options.fault_locations = options.deferred_pc = true;
    options.entry_pc = 0x1000;
    const auto source = [](bool another_use, bool observe_pc, bool odd_prefix = false) {
        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        if (odd_prefix) (void)Constant(builder, 0x55);
        if (observe_pc) Store(builder, 8, Load(builder, 128));
        const auto target = Constant(builder, 0x2000);
        if (another_use) Store(builder, 0, target);
        Store(builder, 128, target);
        Need(builder.EndBlock(Terminal::Dispatch()));
        return std::pair{Submit(builder), target.node};
    };
    auto [program, target] = source(false, false);
    const auto allocation = arm64::AllocateBlock(program, &options);
    const auto code = arm64::Compile(program, options);
    Check(bool(allocation) && !allocation.publish_entry_pc &&
                  allocation.homes[target.value].terminal_only && bool(code) &&
                  code.words.front() == 0x58000050u && code.links.size() == 1,
          "pure static edge has no hot PC literal or state publication");
    Check(sizeof(arm64::LinkSite) == 24 && WordOffset(code.links.front().branch_offset) == 0 &&
                  WordOffset(code.links.front().data_offset) == 2 &&
                  WordOffset(code.links.front().data_offset) + 2 < code.words.size(),
          "cold PC stub follows the original aligned DATA without a second relocation");
    auto [odd_program, odd_target] = source(false, false, true);
    (void)odd_target;
    const auto odd = arm64::Compile(odd_program, options);
    Check(bool(odd) && odd.links.size() == 1 && WordOffset(odd.links.front().branch_offset) == 1 &&
                  WordOffset(odd.links.front().data_offset) == 4 && odd.words[1] == 0x58000070u &&
                  odd.words[2] == 0xd61f0200u && odd.words[3] == 0xd503201fu,
          "odd tail jumps directly through LDR/BR while padding remains unreachable before aligned "
          "DATA");
#if defined(__aarch64__) && defined(__ANDROID__)
    namespace execution = swift::runtime::execution;
    execution::CodeHeap arena{TestBackendOps(), static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    for (const auto* layout : {&code, &odd}) {
        const auto published = arena.Publish(layout->words, {}, layout->links);
        const auto& site = layout->links.front();
        const auto back_words = WordOffset(site.data_offset) - WordOffset(site.branch_offset);
        const auto entry = reinterpret_cast<std::uintptr_t>(published.entry);

        struct Ready {
            u64 pc;
            std::uintptr_t entry;
            unsigned calls{};
        } ready{site.target_pc, entry};

        const auto lookup = [](void* opaque, u64 pc, std::uintptr_t) noexcept -> std::uintptr_t {
            auto& target = *static_cast<Ready*>(opaque);
            ++target.calls;
            return pc == target.pc ? target.entry : 0;
        };
        const auto prelinked = arena.Publish(layout->words, {}, layout->links, lookup, &ready);
        const auto linked_entry = reinterpret_cast<std::uintptr_t>(prelinked.entry);
        const auto linked_delta = static_cast<std::int64_t>(
                entry - (linked_entry + WordOffset(site.branch_offset) * 4));
        const auto* linked_words = reinterpret_cast<const std::uint32_t*>(prelinked.entry);
        Check(linked_words[WordOffset(site.branch_offset)] ==
                              (prelinked.writable ? 0x14000000u | (static_cast<std::uint32_t>(
                                                                           linked_delta / 4) &
                                                                   0x03ffffffu)
                                                  : 0x58000010u | (back_words << 5)) &&
                      Read64(linked_words + WordOffset(site.data_offset)) ==
                              (prelinked.writable
                                       ? entry
                                       : linked_entry + (WordOffset(site.data_offset) + 2) * 4),
              "ready links publish patched with an unlinkable alias; single mappings retain their "
              "cold owner");
        ready.entry = entry + (std::uintptr_t{1} << 28);
        const auto prelinked_far = arena.Publish(layout->words, {}, layout->links, lookup, &ready);
        const auto* far_words = reinterpret_cast<const std::uint32_t*>(prelinked_far.entry);
        Check(ready.calls == (prelinked.writable ? 2u : 0u) &&
                      far_words[WordOffset(site.branch_offset)] ==
                              (0x58000010u | (back_words << 5)) &&
                      Read64(far_words + WordOffset(site.data_offset)) ==
                              (prelinked_far.writable
                                       ? ready.entry
                                       : reinterpret_cast<std::uintptr_t>(prelinked_far.entry) +
                                                 (WordOffset(site.data_offset) + 2) * 4),
              "far ready links keep LDR/BR; a single mapping never creates a link it cannot later "
              "unlink");
        if (!published.writable) continue;
        auto* write = reinterpret_cast<std::uintptr_t*>(published.writable +
                                                        WordOffset(site.data_offset) * 4);
        const auto* execute =
                reinterpret_cast<const std::uintptr_t*>(entry + WordOffset(site.data_offset) * 4);
        const auto* branch =
                reinterpret_cast<const std::uint32_t*>(entry) + WordOffset(site.branch_offset);
        const auto load = 0x58000010u | (back_words << 5);
        const auto near = 0x14000000u | ((0u - WordOffset(site.branch_offset)) & 0x03ffffffu);
        PatchLinkWords(write, execute, entry, back_words);
        Check(*branch == near && Read64(execute) == entry,
              "even and odd links patch B from their actual branch instruction");
        const auto far = entry + (std::uintptr_t{1} << 28);
        PatchLinkWords(write, execute, far, back_words);
        Check(*branch == load && branch[1] == 0xd61f0200u && Read64(execute) == far,
              "far relink restores the explicit DATA displacement without executing alignment "
              "padding");
        PatchLinkWords(write, execute, entry, back_words);
        UnlinkWords(write, execute, back_words);
        Check(*branch == load && Read64(execute) == reinterpret_cast<std::uintptr_t>(execute + 1),
              "even and odd unlink restore the original load and this edge's RX cold stub");
    }
    static unsigned patch_calls{}, unlink_calls{};
    patch_calls = unlink_calls = 0;
    auto counted_backend = TestBackendOps();
    counted_backend.patch_link = [](std::uintptr_t* write,
                                    const std::uintptr_t* execute,
                                    std::uintptr_t target,
                                    std::uint32_t back) noexcept {
        ++patch_calls;
        arm64::PatchNativeLink(write, execute, target, back);
    };
    counted_backend.unlink =
            [](std::uintptr_t* write, const std::uintptr_t* execute, std::uint32_t back) noexcept {
                ++unlink_calls;
                arm64::UnlinkNativeLink(write, execute, back);
            };
    execution::CodeCache self_cache{nullptr, counted_backend};
    const auto& self_site = code.links.front();
    const auto incoming = arena.Publish(code.words, {}, code.links);
    self_cache.InsertReady(options.entry_pc, incoming, {});
    self_cache.AddStaticLink(incoming, self_site);

    struct SelfLookup {
        execution::CodeCache& cache;
        u64 pc;
        unsigned calls{};
    } self_lookup{self_cache, self_site.target_pc};

    const auto self = arena.Publish(
            code.words,
            {},
            code.links,
            [](void* opaque, u64 pc, std::uintptr_t unit) noexcept -> std::uintptr_t {
                auto& lookup = *static_cast<SelfLookup*>(opaque);
                ++lookup.calls;
                if (const auto ready = lookup.cache.Find(pc))
                    return reinterpret_cast<std::uintptr_t>(ready);
                return pc == lookup.pc ? unit : 0;
            },
            &self_lookup);
    self_cache.AddStaticLink(self, self_site);
    Check(self_lookup.calls == (self.writable ? 1u : 0u) &&
                  self_cache.Pending() == (self.writable ? 2u : 0u),
          "an unpublished self target uses its actual RX base while both relocation rows remain "
          "pending until ready registration");
    self_cache.InsertReady(self_site.target_pc, self, {});
    Check(patch_calls == (self.writable ? 1u : 0u) &&
                  self_cache.Patched() == (self.writable ? 2u : 0u) && self_cache.Pending() == 0,
          "ready registration patches the old incoming edge once without resynchronizing the "
          "already prepared self edge");
    self_cache.ResetLinks();
    if (self.writable) {
        const auto base = reinterpret_cast<std::uintptr_t>(self.entry);
        const auto* words = reinterpret_cast<const std::uint32_t*>(self.entry);
        Check(unlink_calls == 2 &&
                      Read64(words + WordOffset(self_site.data_offset)) ==
                              base + self_site.fallback_offset &&
                      words[WordOffset(self_site.branch_offset)] ==
                              (0x58000010u | ((WordOffset(self_site.data_offset) -
                                               WordOffset(self_site.branch_offset))
                                              << 5)),
              "the prelinked self edge keeps its real relocation and restores its cold gate on "
              "unlink");
    }
#endif
    auto [shared, shared_target] = source(true, false);
    const auto shared_home = arm64::AllocateBlock(shared, &options);
    Check(bool(shared_home) && !shared_home.homes[shared_target.value].terminal_only &&
                  bool(arm64::Compile(shared, options)),
          "a real data use retains the PC literal's normal scalar home");
    auto [observed, observed_target] = source(false, true);
    (void)observed_target;
    const auto observed_home = arm64::AllocateBlock(observed, &options);
    const auto observed_code = arm64::Compile(observed, options);
    Check(bool(observed_home) && observed_home.publish_entry_pc && bool(observed_code) &&
                  observed_code.words.front() != 0x58000050u,
          "an actual PC Uniform reader receives the explicit request PC at entry");
    options.deferred_pc = false;
    const auto eager = arm64::AllocateBlock(program, &options);
    Check(bool(eager) && !eager.homes[target.value].terminal_only &&
                  bool(arm64::Compile(program, options)),
          "generic linked ABI retains eager PC publication and literal materialization");
}

void CompleteAdditionalEntries() {
#if defined(__aarch64__) && defined(__ANDROID__)
    namespace execution = swift::runtime::execution;

    struct State {
        swift::runtime::x86::GuestState cpu;
        std::array<u64, 20> inputs;
        u64 seen_pc{};
    };

    constexpr u64 first_pc = 0x4100, other_pc = 0x4200, flags = 0xa5a50000bc000085ULL;
    for (const bool native_nzcv : {false, true}) {
        if (native_nzcv && !(getauxval(AT_HWCAP) & HWCAP_FLAGM)) continue;
        Builder builder{Scope::Function};
        const auto first = Create(builder), other = Create(builder);
        Need(builder.BeginBlock(first));
        Store(builder,
              offsetof(swift::runtime::x86::GuestState, gprs) + swift::runtime::x86::RDX * 8,
              Constant(builder, 123));
        Need(builder.EndBlock(Terminal::Jump(other)));
        Need(builder.BeginBlock(other));
        Need(builder.Emit<OpCode::EntryPoint>(Type::Void,
                                              {Operand::Immediate(other_pc, Type::U64)}));
        std::array<Value, 20> values;
        for (unsigned i = 0; i < values.size(); ++i)
            values[i] = Load(builder, offsetof(State, inputs) + i * 8);
        auto sum = values.front();
        for (unsigned i = 1; i < values.size(); ++i)
            sum = Need(builder.Emit<OpCode::Add>(
                    Type::U64, {Operand::ValueOf(sum), Operand::ValueOf(values[i])}));
        Store(builder,
              offsetof(swift::runtime::x86::GuestState, gprs) + swift::runtime::x86::R8 * 8,
              sum);
        Store(builder,
              offsetof(State, seen_pc),
              Load(builder, offsetof(swift::runtime::x86::GuestState, pc)));
        Need(builder.EndBlock(Terminal::Return(Constant(builder, 42))));
        auto program = Submit(builder);
        arm64::CompileOptions options{offsetof(swift::runtime::x86::GuestState, packed_flags)};
        options.dispatcher = reinterpret_cast<std::uintptr_t>(
                native_nzcv ? &execution::SvmNativeDispatchNZCV : &execution::SvmNativeDispatch);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
        options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
        options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.private_uniforms = options.fault_locations = options.deferred_pc = true;
        options.native_nzcv = native_nzcv;
        options.entry_pc = first_pc;
        const auto compiled = arm64::Compile(program, options);
        Check(bool(compiled) && compiled.entries.size() == 1 &&
                      compiled.entries[0].pc == other_pc && compiled.frame_bytes > 0,
              "additional entry owns the same actual nonzero frame as its original unit");
        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        const auto unit = arena.Publish(compiled.words, compiled.faults, compiled.links);
        const auto alternate = reinterpret_cast<execution::NativeCode::Function>(
                reinterpret_cast<std::uintptr_t>(unit.entry) +
                std::uintptr_t{WordOffset(compiled.entries[0].offset)} * 4);

        struct Resolver {
            execution::NativeCode::Function entry;
            unsigned calls{};
        } resolver{};

        State state;
        std::atomic<const execution::NativeCode*> active{};
        execution::NativeRunContext context;
        context.state = &state.cpu;
        context.opaque = &resolver;
        context.resolve = [](void* p, execution::NativeRunContext*) noexcept -> std::uintptr_t {
            auto& value = *static_cast<Resolver*>(p);
            ++value.calls;
            return reinterpret_cast<std::uintptr_t>(value.entry);
        };
        context.active = &active;
        context.anchor = &unit;
        context.pinned_uniform_offset = options.pinned_uniform_offset;
        context.pinned_uniform_slots = 16;
        context.flags_offset = options.flags_offset;
        context.pc_offset = options.pc_offset;
        context.native_nzcv = native_nzcv;
        for (const bool secondary : {false, true}) {
            state = {};
            state.cpu.pc = secondary ? other_pc : first_pc;
            state.cpu.packed_flags = flags;
            for (unsigned i = 0; i < state.inputs.size(); ++i) state.inputs[i] = i + 1;
            resolver.entry = secondary ? alternate : unit.entry;
            resolver.calls = 0;
            Check(execution::SvmRunNative(&state.cpu, &context) == 42 && resolver.calls == 1 &&
                          state.cpu.gprs[swift::runtime::x86::R8] == 210 &&
                          state.seen_pc == (secondary ? other_pc : first_pc) &&
                          state.cpu.gprs[swift::runtime::x86::RDX] == (secondary ? 0 : 123) &&
                          state.cpu.packed_flags == flags,
                  "external prologue and internal body edge preserve SP, own PC, values and "
                  "complete flags");
        }
    }
#endif
}

void LinkedDynamicPC() {
#if defined(__aarch64__) && defined(__ANDROID__)
    namespace execution = swift::runtime::execution;
    constexpr u64 entry_pc = 0x1000, next_pc = 0x2000, initial = 0xa5a50000bc00003cULL;
    for (const bool native_nzcv : {false, true}) {
        if (native_nzcv && !(getauxval(AT_HWCAP) & HWCAP_FLAGM)) continue;
        arm64::CompileOptions options;
        options.flags_offset = offsetof(swift::runtime::x86::GuestState, packed_flags);
        options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
        options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.private_uniforms = options.fault_locations = options.deferred_pc = true;
        options.native_nzcv = native_nzcv;
        options.entry_pc = entry_pc;
        options.dispatch_mode = arm64::DispatchMode::Inline;
        options.dispatcher =
                native_nzcv ? reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCV)
                            : reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatch);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);

        Builder source{Scope::Block};
        Need(source.BeginBlock(Create(source)));
        Store(source,
              options.pc_offset,
              Load(source, options.pinned_uniform_offset + swift::runtime::x86::RAX * sizeof(u64)));
        Need(source.EndBlock(Terminal::Dispatch()));
        auto source_program = Submit(source);
        const auto source_allocation = arm64::AllocateBlock(source_program, &options);
        const auto source_code = arm64::Compile(source_program, options);
        Check(bool(source_allocation) && !source_allocation.publish_entry_pc && bool(source_code) &&
                      source_code.links.empty(),
              "GPR-sourced PC tail remains dynamic without a static target relocation");

        Builder target{Scope::Block};
        Need(target.BeginBlock(Create(target)));
        Store(target,
              options.pinned_uniform_offset + swift::runtime::x86::RCX * sizeof(u64),
              Load(target, options.pc_offset));
        Need(target.EndBlock(Terminal::Return(Constant(target, 42))));
        auto target_program = Submit(target);
        options.entry_pc = next_pc;
        const auto target_allocation = arm64::AllocateBlock(target_program, &options);
        const auto target_code = arm64::Compile(target_program, options);
        Check(bool(target_allocation) && target_allocation.publish_entry_pc && bool(target_code),
              "dynamic target's actual PC observer requires entry publication");

        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        const auto entry = arena.Publish(source_code.words, {}, source_code.links);
        const auto destination = arena.Publish(target_code.words, {}, target_code.links);
        std::array<execution::DispatchSlot, 4> slots{};
        const auto home =
                (next_pc ^ (next_pc >> arm64::abi::DispatchHashShift)) & (slots.size() - 1);
        slots[home].pc = next_pc + 4;
        slots[home].occupied = true;
        auto& ready = slots[(home + 1) & (slots.size() - 1)];

        struct Resolver {
            const execution::NativeCode* first;
            const execution::NativeCode* second;
            execution::NativeCode anchor;
            execution::DispatchSlot* ready;
            unsigned calls{};
            std::array<u64, 2> observed{};
            u64 target_pc{};
        } resolver{&entry, &destination, entry, &ready};

        resolver.target_pc = next_pc;
        swift::runtime::x86::GuestState state;
        std::atomic<const execution::NativeCode*> active{};
        execution::NativeRunContext context;
        context.opaque = &resolver;
        context.resolve = [](void* opaque,
                             execution::NativeRunContext* current) noexcept -> std::uintptr_t {
            auto& request = *static_cast<Resolver*>(opaque);
            const auto call = request.calls++;
            const auto pc = current->state->pc;
            if (call < request.observed.size()) request.observed[call] = pc;
            if (call == 0 && pc == entry_pc) {
                request.anchor = *request.first;
                return reinterpret_cast<std::uintptr_t>(request.first->entry);
            }
            if (call == 1 && pc == request.target_pc) {
                request.ready->pc = request.target_pc;
                request.ready->entry = request.second->entry;
                request.ready->code_id = 0;
                request.ready->occupied = true;
                request.anchor = *request.second;
                return reinterpret_cast<std::uintptr_t>(request.second->entry);
            }
            return 0;
        };
        context.slots = slots.data();
        context.mask = slots.size() - 1;
        context.active = &active;
        context.anchor = &resolver.anchor;
        context.pinned_uniform_offset = options.pinned_uniform_offset;
        context.pinned_uniform_slots = options.pinned_uniform_slots;
        context.flags_offset = options.flags_offset;
        context.pc_offset = options.pc_offset;
        context.native_nzcv = native_nzcv;
        for (const bool warm : {false, true}) {
            context.slots =
                    warm ? slots.data()
                         : nullptr;  // Public nullable context keeps the original cold guard.
            state = {};
            state.pc = entry_pc;
            state.packed_flags = initial;
            state.gprs[swift::runtime::x86::RAX] = next_pc;
            resolver.calls = 0;
            resolver.observed = {};
            Check(execution::SvmRunNative(&state, &context) == 42,
                  "dynamic Dispatch keeps its PC distinct from the target's Return reason");
            Check(resolver.observed[0] == entry_pc && resolver.calls == (warm ? 1u : 2u) &&
                          (warm || resolver.observed[1] == next_pc),
                  "root resolves its original PC; only a dynamic cold miss resolves the true next "
                  "PC");
            Check(state.pc == next_pc && state.gprs[swift::runtime::x86::RCX] == next_pc &&
                          state.packed_flags == initial,
                  "cold and ready dynamic edges preserve whole packed flags and expose the target "
                  "PC");
        }
        Builder local{Scope::Block};
        Need(local.BeginBlock(Create(local)));
        const auto count =
                Load(local, options.pinned_uniform_offset + swift::runtime::x86::RDX * sizeof(u64));
        Store(local, options.pc_offset, Constant(local, next_pc));
        const auto skip = Need(local.Emit<OpCode::NotGoto>(Type::U64, {Operand::ValueOf(count)}));
        const auto address =
                Load(local, options.pinned_uniform_offset + swift::runtime::x86::R8 * sizeof(u64));
        Need(local.Emit<OpCode::StoreMemoryTSO>(
                Type::Void, {Operand::ValueOf(address), Operand::ValueOf(Constant(local, 0x55))}));
        Store(local,
              options.pinned_uniform_offset + swift::runtime::x86::RCX * sizeof(u64),
              Constant(local, 0x55));
        Store(local, options.pc_offset, Constant(local, 0x3000));
        Need(local.Emit<OpCode::BindLabel>(Type::Void, {Operand::ValueOf(skip)}));
        Store(local, options.pinned_uniform_offset + swift::runtime::x86::R11 * sizeof(u64), skip);
        Need(local.EndBlock(Terminal::Dispatch()));
        auto local_program = Submit(local);
        auto local_options = options;
        local_options.entry_pc = entry_pc;
        const auto local_code = arm64::Compile(local_program, local_options);
        Check(bool(local_code) && local_code.links.empty(),
              "a forward control join reloads the actual PC rather than using the body's constant "
              "target");
        Builder local_target{Scope::Block};
        Need(local_target.BeginBlock(Create(local_target)));
        Need(local_target.EndBlock(Terminal::Return(Constant(local_target, 42))));
        auto local_target_program = Submit(local_target);
        // This one Return body is shared by two real PCs. Keep the PC
        // already published by cold dispatch instead of assigning both
        // entries the source unit's deferred entry_pc.
        auto local_target_options = local_options;
        local_target_options.deferred_pc = false;
        const auto local_target_code = arm64::Compile(local_target_program, local_target_options);
        Check(bool(local_target_code), "forward-control target uses the original Return ABI");
        const auto local_entry = arena.Publish(local_code.words, {}, local_code.links);
        const auto local_destination =
                arena.Publish(local_target_code.words, {}, local_target_code.links);
        execution::DispatchSlot local_slot;
        resolver.first = &local_entry;
        resolver.second = &local_destination;
        resolver.ready = &local_slot;
        for (const u64 count_value : {u64{0}, u64{0x100}}) {
            u64 memory = 0x77;
            const auto selected_pc = count_value ? 0x3000 : next_pc;
            state = {};
            state.pc = entry_pc;
            state.packed_flags = initial;
            state.gprs[swift::runtime::x86::RDX] = count_value;
            state.gprs[swift::runtime::x86::RCX] = 0x11;
            state.gprs[swift::runtime::x86::R8] =
                    count_value ? reinterpret_cast<std::uintptr_t>(&memory) : 0;
            resolver.calls = 0;
            resolver.observed = {};
            resolver.target_pc = selected_pc;
            context.slots = nullptr;
            context.mask = 0;
            const auto returned = execution::SvmRunNative(&state, &context);
            const bool okay = returned == 42 && resolver.calls == 2 &&
                              resolver.observed[0] == entry_pc &&
                              resolver.observed[1] == selected_pc && state.pc == selected_pc &&
                              state.packed_flags == initial &&
                              state.gprs[swift::runtime::x86::RCX] == (count_value ? 0x55 : 0x11) &&
                              state.gprs[swift::runtime::x86::R11] == count_value &&
                              memory == (count_value ? 0x55 : 0x77);
            if (!okay)
                std::fprintf(
                        stderr,
                        "forward-local native=%u count=%llx ret=%llx calls=%u observed=%llx/%llx "
                        "pc=%llx flags=%llx rcx=%llx r11=%llx memory=%llx\n",
                        static_cast<unsigned>(native_nzcv),
                        static_cast<unsigned long long>(count_value),
                        static_cast<unsigned long long>(returned),
                        resolver.calls,
                        static_cast<unsigned long long>(resolver.observed[0]),
                        static_cast<unsigned long long>(resolver.observed[1]),
                        static_cast<unsigned long long>(state.pc),
                        static_cast<unsigned long long>(state.packed_flags),
                        static_cast<unsigned long long>(state.gprs[swift::runtime::x86::RCX]),
                        static_cast<unsigned long long>(state.gprs[swift::runtime::x86::R11]),
                        static_cast<unsigned long long>(memory));
            Check(okay,
                  "zero skips even a null memory operand; nonzero commits body state and both "
                  "paths retain flags, PC and branch value");
        }
        resolver.first = &entry;
        resolver.second = &destination;
        resolver.ready = &ready;
        resolver.target_pc = next_pc;
        context.slots = slots.data();
        context.mask = slots.size() - 1;
        if (native_nzcv)
            for (const auto scope : {Scope::Block, Scope::Function}) {
                constexpr u64 other_pc = 0x3000;
                const auto predicate_source = [&](Condition condition) {
                    Builder builder{scope};
                    Need(builder.BeginBlock(Create(builder)));
                    const auto check = Need(builder.Emit<OpCode::CondSet>(
                            Type::Bool, {Operand::Condition(condition)}));
                    Need(builder.Emit<OpCode::StoreUniformSelectImm>(
                            Type::Void,
                            {Operand::Uniform(options.pc_offset, Type::U64),
                             Operand::ValueOf(check),
                             Operand::Immediate(other_pc, Type::U64),
                             Operand::Immediate(next_pc, Type::U64)}));
                    Need(builder.EndBlock(Terminal::Dispatch()));
                    return Submit(builder);
                };
                auto predicate_options = options;
                predicate_options.entry_pc = entry_pc;
                auto predicate_program = predicate_source(Condition::EQ);
                const auto predicate_code = arm64::Compile(predicate_program, predicate_options);
                Check(bool(predicate_code) && predicate_code.links.size() == 2,
                      "direct PC predicate compiles with two owned edges");
                const auto primary = std::find_if(
                        predicate_code.links.begin(),
                        predicate_code.links.end(),
                        [](const arm64::LinkSite& link) { return link.target_pc == next_pc; });
                Check(primary != predicate_code.links.end() &&
                              (predicate_code.words[WordOffset(primary->branch_offset)] &
                               0xff00001fu) == 0x54000001u,
                      "two forward targets choose predicate-false as a patchable B.NE head");
                auto backward_options = predicate_options;
                backward_options.entry_pc = 0x4000;
                const auto backward = arm64::Compile(predicate_program, backward_options);
                Check(bool(backward) &&
                              std::any_of(
                                      backward.links.begin(),
                                      backward.links.end(),
                                      [&](const arm64::LinkSite& link) {
                                          return link.target_pc == other_pc &&
                                                 (backward.words[WordOffset(link.branch_offset)] &
                                                  0xff00001fu) == 0x54000000u;
                                      }),
                      "a backward yes target has priority over the default no layout");
                auto flip_program = predicate_source(Condition::HI);
                const auto flip = arm64::Compile(flip_program, predicate_options);
                Check(bool(flip) &&
                              std::count(flip.words.begin(), flip.words.end(), 0xd500401fu) == 3 &&
                              std::all_of(flip.links.begin(),
                                          flip.links.end(),
                                          [&](const arm64::LinkSite& link) {
                                              return (flip.words[WordOffset(link.branch_offset)] &
                                                      0xff00001fu) == 0x58000010u;
                                          }),
                      "temporary carry inversion retains the old per-edge restoration and ordinary "
                      "links");

                auto other_options = options;
                other_options.entry_pc = other_pc;
                const auto other_code = arm64::Compile(target_program, other_options);
                Check(bool(other_code),
                      "second predicate target preserves its actual entry PC observer");
                const auto other_destination =
                        arena.Publish(other_code.words, {}, other_code.links);
                auto at = (other_pc ^ (other_pc >> arm64::abi::DispatchHashShift)) &
                          (slots.size() - 1);
                while (slots[at].occupied && slots[at].pc != other_pc)
                    at = (at + 1) & (slots.size() - 1);
                slots[at].pc = other_pc;
                slots[at].entry = other_destination.entry;
                slots[at].occupied = true;

                struct ReadyTargets {
                    std::uintptr_t no, yes;
                    unsigned calls{};
                } targets{reinterpret_cast<std::uintptr_t>(destination.entry),
                          reinterpret_cast<std::uintptr_t>(other_destination.entry)};

                const auto predicate_entry = arena.Publish(
                        predicate_code.words,
                        {},
                        predicate_code.links,
                        [](void* opaque, u64 pc, std::uintptr_t) noexcept -> std::uintptr_t {
                            auto& ready = *static_cast<ReadyTargets*>(opaque);
                            ++ready.calls;
                            return pc == next_pc ? ready.no : pc == other_pc ? ready.yes : 0;
                        },
                        &targets);
                Check(targets.calls == (predicate_entry.writable ? 2u : 0u),
                      "only unlinkable conditional edges resolve during unpublished "
                      "initialization");
                resolver.first = &predicate_entry;
                const auto run_predicate = [&](bool zero) {
                    const auto flags = zero ? initial | (u64{1} << 30) : initial & ~(u64{1} << 30);
                    state = {};
                    state.pc = entry_pc;
                    state.packed_flags = flags;
                    resolver.calls = 0;
                    resolver.observed = {};
                    const auto selected_pc = zero ? other_pc : next_pc;
                    Check(execution::SvmRunNative(&state, &context) == 42 && resolver.calls == 1 &&
                                  state.pc == selected_pc &&
                                  state.gprs[swift::runtime::x86::RCX] == selected_pc &&
                                  state.packed_flags == flags,
                          "both predicate outcomes preserve whole flags/reserved and observe the "
                          "true "
                          "target PC before Return42");
                };
                run_predicate(false);
                run_predicate(true);
                if (predicate_entry.writable) {
                    auto* write = reinterpret_cast<std::uintptr_t*>(
                            predicate_entry.writable + WordOffset(primary->data_offset) * 4);
                    const auto* execute = reinterpret_cast<const std::uintptr_t*>(
                            reinterpret_cast<const std::uint8_t*>(predicate_entry.entry) +
                            WordOffset(primary->data_offset) * 4);
                    const auto back =
                            WordOffset(primary->data_offset) - WordOffset(primary->branch_offset);
                    const auto* words =
                            reinterpret_cast<const std::uint32_t*>(predicate_entry.entry);
                    PatchLinkWords(write,
                                   execute,
                                   reinterpret_cast<std::uintptr_t>(predicate_entry.entry) +
                                           (std::uintptr_t{1} << 21),
                                   back);
                    Check(words[WordOffset(primary->branch_offset)] ==
                                          predicate_code
                                                  .words[WordOffset(primary->branch_offset)] &&
                                  (words[WordOffset(primary->data_offset) - 2] & 0xfc000000u) ==
                                          0x14000000u,
                          "far B.cond target preserves its condition and falls back through the "
                          "real "
                          "B26 gate");
                    UnlinkWords(write, execute, back);
                    Check(words[WordOffset(primary->branch_offset)] ==
                                          predicate_code
                                                  .words[WordOffset(primary->branch_offset)] &&
                                  words[WordOffset(primary->data_offset) - 2] == 0x58000050u &&
                                  Read64(execute) == reinterpret_cast<std::uintptr_t>(execute + 1),
                          "B.cond unlink restores the owned cold stub without changing its "
                          "predicate");
                    run_predicate(false);
                }
            }
        // The same scene owns a callable seed. Packed/plain covers the empty
        // PC-zero tag match; Native/counting covers empty mismatch and pending.
        auto callable_options = options;
        callable_options.entry_pc = entry_pc;
        callable_options.callable_directory = true;
        callable_options.dispatch_mode =
                native_nzcv ? arm64::DispatchMode::InlineCounted : arm64::DispatchMode::Inline;
        if (native_nzcv)
            callable_options.dispatcher =
                    reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCVCounted);
        const auto callable_code = arm64::Compile(source_program, callable_options);
        Check(bool(callable_code) &&
                      (native_nzcv || callable_code.words.size() + 2 == source_code.words.size()),
              "the explicit callable owner removes two nullable ready-path guards without changing "
              "the register bank");
        const u64 callable_pc = native_nzcv ? next_pc : 0;
        auto callable_target_options = callable_options;
        callable_target_options.entry_pc = callable_pc;
        const auto callable_target_code = arm64::Compile(target_program, callable_target_options);
        Check(bool(callable_target_code), "callable cold target preserves its real PC observer");
        const auto callable_entry = arena.Publish(callable_code.words, {}, callable_code.links);
        const auto callable_target =
                arena.Publish(callable_target_code.words, {}, callable_target_code.links);
        execution::DispatchSlot seed;
        const auto cold = reinterpret_cast<execution::NativeCode::Function>(
                native_nzcv ? &execution::SvmNativePendingCounted : &execution::SvmNativePending);
        resolver.first = &callable_entry;
        resolver.second = &callable_target;
        resolver.ready = &seed;
        resolver.target_pc = callable_pc;
        context.slots = &seed;
        context.mask = 0;
        const auto run_callable = [&](bool pending) {
            seed = {};
            seed.entry = cold;
            if (pending) {
                seed.pc = callable_pc;
                seed.occupied = true;
            }
            state = {};
            state.pc = entry_pc;
            state.packed_flags = initial;
            state.gprs[swift::runtime::x86::RAX] = callable_pc;
            resolver.calls = 0;
            resolver.observed = {};
            context.dispatcher_entries = context.probe_steps = 0;
            context.cache_hits = 37;
            Check(execution::SvmRunNative(&state, &context) == 42 && resolver.calls == 2 &&
                          resolver.observed[0] == entry_pc && resolver.observed[1] == callable_pc &&
                          state.pc == callable_pc &&
                          state.gprs[swift::runtime::x86::RCX] == callable_pc &&
                          state.packed_flags == initial,
                  "callable empty/pending paths commit the actual PC and preserve complete "
                  "flags/reserved");
            Check(context.cache_hits == 37 &&
                          (!native_nzcv || (context.dispatcher_entries == (pending ? 1u : 2u) &&
                                            context.probe_steps == (pending ? 1u : 2u))),
                  "only a counted tag-match pending call undoes one hit, while empty mismatch "
                  "follows real cold probes");
        };
        run_callable(false);
        if (native_nzcv) run_callable(true);

        auto cached_options = options;
        cached_options.entry_pc = entry_pc;
        cached_options.local_dispatch_cache = true;
        cached_options.dispatch_mode = arm64::DispatchMode::InlineCounted;
        cached_options.dispatcher = reinterpret_cast<std::uintptr_t>(
                native_nzcv ? &execution::SvmNativeDispatchNZCVCounted
                            : &execution::SvmNativeDispatchCounted);
        const auto cached_code = arm64::Compile(source_program, cached_options);
        Check(bool(cached_code) && cached_code.links.size() == 1 &&
                      cached_code.links[0].kind == arm64::LinkKind::Dynamic,
              "one real dynamic site owns one fixed array relocation in the existing link "
              "collection");
        const auto cached_entry = arena.Publish(cached_code.words, {}, cached_code.links);
        const auto tuple_offset = WordOffset(cached_code.links[0].data_offset) * 4u;
        const auto* tuple =
                reinterpret_cast<const std::uint8_t*>(cached_entry.entry) + tuple_offset;
        const auto cache_fallback = reinterpret_cast<std::uintptr_t>(cached_entry.entry) +
                                    cached_code.links[0].fallback_offset;
        const auto pair_offset = [](u64 pc) {
            return ((pc >> 2) & (arm64::abi::DynamicCacheEntries - 1)) * 16u;
        };
        bool callable_entries = true;
        for (unsigned i = 0; i < arm64::abi::DynamicCacheEntries; ++i)
            callable_entries &= Read64(tuple + i * 16 + 8) == cache_fallback;
        Check(callable_entries &&
                      Read64(tuple + arm64::abi::DynamicCacheEntries * 16) ==
                              (cached_entry.writable ? reinterpret_cast<std::uintptr_t>(
                                                               cached_entry.writable + tuple_offset)
                                                     : 0),
              "publication starts with sixteen callable site fallbacks and the actual writable "
              "DATA base alias");
        execution::DispatchSlot cached_slot;
        cached_slot.pc = callable_pc;
        cached_slot.entry = cold;
        cached_slot.occupied = true;
        // Both counted variants use the counted pending thunk; it removes
        // only the preceding directory tag-match hit, never a local hit.
        cached_slot.entry = reinterpret_cast<execution::NativeCode::Function>(
                &execution::SvmNativePendingCounted);
        resolver.first = &cached_entry;
        resolver.second = &callable_target;
        resolver.ready = &cached_slot;
        resolver.target_pc = callable_pc;
        const auto run_cached = [&](unsigned calls, u64 probes, u64 hits, u64 target_pc) {
            state = {};
            state.pc = entry_pc;
            state.packed_flags = initial;
            state.gprs[swift::runtime::x86::RAX] = target_pc;
            resolver.calls = 0;
            resolver.observed = {};
            resolver.target_pc = target_pc;
            context.dispatcher_entries = context.probe_steps = context.cache_hits = 0;
            Check(execution::SvmRunNative(&state, &context) == 42 && resolver.calls == calls &&
                          state.pc == target_pc &&
                          state.gprs[swift::runtime::x86::RCX] == target_pc &&
                          state.packed_flags == initial && context.dispatcher_entries == 1 &&
                          context.probe_steps == probes && context.cache_hits == hits,
                  "local and directory hits preserve PC/Return/whole flags and count one dispatch "
                  "with only real directory probes");
        };
        context.slots = &cached_slot;
        context.mask = 0;
        run_cached(2, 1, 0, callable_pc);
        const auto first_offset = pair_offset(callable_pc);
        Check(Read64(tuple + first_offset + 8) == cache_fallback,
              "a callable pending payload leaves the local site fallback intact, including PC "
              "zero");
        run_cached(1, 1, 1, callable_pc);
        if (cached_entry.writable) {
            Check(Read64(tuple + first_offset) == callable_pc &&
                          Read64(tuple + first_offset + 8) ==
                                  reinterpret_cast<std::uintptr_t>(callable_target.entry),
                  "only the ready code_id directory result warms this actual site's selected pair");
            context.slots = nullptr;  // A real local hit does not consult even the nullable public
                                      // directory.
            run_cached(1, 0, 1, callable_pc);
            for (unsigned i = 0; i < arm64::abi::DynamicCacheEntries; ++i)
                __atomic_store_n(reinterpret_cast<std::uintptr_t*>(cached_entry.writable +
                                                                   tuple_offset + i * 16 + 8),
                                 cache_fallback,
                                 __ATOMIC_RELEASE);
            context.slots = &cached_slot;
            run_cached(1, 1, 1, callable_pc);
            const auto changed_pc = callable_pc + 4;
            auto changed_options = callable_target_options;
            changed_options.entry_pc = changed_pc;
            const auto changed_code = arm64::Compile(target_program, changed_options);
            Check(bool(changed_code),
                  "the different-index ready target preserves its actual PC observer");
            const auto changed_target = arena.Publish(changed_code.words, {}, changed_code.links);
            cached_slot.pc = changed_pc;
            cached_slot.entry = changed_target.entry;
            cached_slot.code_id = 1;
            resolver.second = &changed_target;
            run_cached(1, 1, 1, changed_pc);
            const auto second_offset = pair_offset(changed_pc);
            Check(first_offset != second_offset && Read64(tuple + first_offset) == callable_pc &&
                          Read64(tuple + second_offset) == changed_pc &&
                          Read64(tuple + second_offset + 8) ==
                                  reinterpret_cast<std::uintptr_t>(changed_target.entry),
                  "different indices retain both full-PC ready targets");
            context.slots = nullptr;
            run_cached(1, 0, 1, callable_pc);
            run_cached(1, 0, 1, changed_pc);

            const auto collision_pc = callable_pc + arm64::abi::DynamicCacheEntries * 4;
            auto collision_options = callable_target_options;
            collision_options.entry_pc = collision_pc;
            const auto collision_code = arm64::Compile(target_program, collision_options);
            Check(bool(collision_code),
                  "the same-index replacement preserves its actual PC observer");
            const auto collision_target =
                    arena.Publish(collision_code.words, {}, collision_code.links);
            context.slots = &cached_slot;
            cached_slot.pc = collision_pc;
            cached_slot.entry = collision_target.entry;
            cached_slot.code_id = 2;
            resolver.second = &collision_target;
            run_cached(1, 1, 1, collision_pc);
            Check(pair_offset(collision_pc) == first_offset &&
                          Read64(tuple + first_offset) == collision_pc &&
                          Read64(tuple + first_offset + 8) ==
                                  reinterpret_cast<std::uintptr_t>(collision_target.entry) &&
                          Read64(tuple + second_offset) == changed_pc,
                  "a full-PC collision probes the directory and replaces only its selected pair");
            context.slots = nullptr;
            run_cached(1, 0, 1, collision_pc);
            run_cached(1, 0, 1, changed_pc);
            context.slots = &cached_slot;
            cached_slot.pc = callable_pc;
            cached_slot.entry = callable_target.entry;
            cached_slot.code_id = 0;
            resolver.second = &callable_target;
            run_cached(1, 1, 1, callable_pc);
            context.slots = nullptr;
            run_cached(1, 0, 1, callable_pc);
            Check(Read64(tuple + second_offset) == changed_pc,
                  "returning to an evicted target can refill without disabling the other index");
        } else
            Check(Read64(tuple + first_offset + 8) == cache_fallback,
                  "single-mapping publication retains its callable fallback and uses the "
                  "directory");
    }
#endif
}

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
    Execute(code, state.data(), 0xa5);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(Read64(state.data()) == 0xc5 && Read64(state.data() + 8) == 0xc5 &&
                  Read64(state.data() + 16) == 0x95ef && Read64(state.data() + 24) == 0xa5,
          "narrow AND keeps complete canonical values after dirty raw inputs and fallback masks");
    constexpr auto expected = (initial & ~u64{0xf40000ff}) | (u64{1} << 31) | 0xc5;
    Check(Read64(state.data() + 32) == expected && Read64(state.data() + 40) == expected &&
                  Read64(state.data() + 128) == expected,
          "narrow logical SF/ZF/PF/AF/CV and unrelated operations preserve the original flag "
          "contract");
#endif
}

void VectorSignedWidenMultiply() {
    using Vector = std::array<u64, 2>;

    struct Case {
        unsigned bits;
        Vector left, right, expected;
        bool square;
    };

    constexpr std::array cases{
            Case{8,
                 {0x7f80ff010200feffULL, 0x1111222233334444ULL},
                 {0x807f02fffe050103ULL, 0xffff80007fff0001ULL},
                 {0xfffc0000fffefffdULL, 0xc080c080fffeffffULL},
                 false},
            Case{16,
                 {0x00027fff8000ffffULL, 0xabcdef0123456789ULL},
                 {0xfffdfffe8000ffffULL, 0x13579bdf2468ace0ULL},
                 {0x4000000000000001ULL, 0xfffffffaffff0002ULL},
                 false},
            Case{32,
                 {0x7fffffff80000000ULL, 0xdeadbeef01234567ULL},
                 {},
                 {0x4000000000000000ULL, 0x3fffffff00000001ULL},
                 true},
    };
    std::array<std::uint8_t, cases.size() * 32> state{};
    std::array<Value, cases.size()> left{}, products{};
    Builder builder{Scope::Block, arm64::KnownTargetInfo()};
    Need(builder.BeginBlock(Create(builder)));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto offset = static_cast<std::uint32_t>(i * 32);
        std::memcpy(state.data() + offset, cases[i].left.data(), sizeof(Vector));
        std::memcpy(state.data() + offset + 16, cases[i].right.data(), sizeof(Vector));
        left[i] = Need(builder.Emit<OpCode::LoadUniform>(Type::V128,
                                                         {Operand::Uniform(offset, Type::V128)}));
        const auto right =
                cases[i].square ? left[i]
                                : Need(builder.Emit<OpCode::LoadUniform>(
                                          Type::V128, {Operand::Uniform(offset + 16, Type::V128)}));
        products[i] = Need(builder.Emit<OpCode::VecSignedMulWidenLow>(
                Type::V128,
                {Operand::ValueOf(left[i]),
                 Operand::ValueOf(right),
                 Operand::Immediate(cases[i].bits, Type::U8)}));
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(offset, Type::V128), Operand::ValueOf(products[i])}));
    }
    Need(builder.EndBlock(Terminal::Return(Constant(builder, 0))));
    auto program = Submit(builder);
    Check(program.Target().Has(TargetFeature::VectorSignedMulWidenLow),
          "known backend advertises the neutral signed low-half widening product");
    const auto code = ::Compile(program);
    Check(static_cast<std::size_t>(std::count_if(code.words.begin(),
                                                 code.words.end(),
                                                 [](std::uint32_t word) {
                                                     return (word & 0xff20fc00u) == 0x0e20c000u;
                                                 })) == cases.size(),
          "each supported lane width selects one complete SMULL result");

    arm64::CompileOptions pinned;
    pinned.pinned_vector_offset = 0;
    pinned.private_uniforms = true;
    const auto allocation = arm64::AllocateBlock(program, &pinned);
    Check(bool(allocation), "signed widening product accepts the existing vector pin allocation");
    for (std::size_t i = 0; i < cases.size(); ++i)
        Check(!allocation.homes[left[i].node.value].spilled &&
                      allocation.homes[left[i].node.value].reg ==
                              allocation.homes[products[i].node.value].reg,
              "low-half inputs are read before their shared full-result pin is overwritten");
    Execute(code, state.data(), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
    for (std::size_t i = 0; i < cases.size(); ++i)
        Check(Read64(state.data() + i * 32) == cases[i].expected[0] &&
                      Read64(state.data() + i * 32 + 8) == cases[i].expected[1],
              "signed low lanes widen exactly, ignore upper noise, and retain the complete "
              "product");
#endif
}

void PackedFloatingArithmetic() {
    using Vector = std::array<u64, 2>;
    const auto singles = [](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        return Vector{u64{a} | (u64{b} << 32), u64{c} | (u64{d} << 32)};
    };

    struct State {
        u64 flags;
        std::uint32_t mxcsr, padding{};
        std::array<Vector, 4> inputs;
        std::array<Vector, 8> outputs{};
        std::array<Vector, 4> denormals;
        std::array<Vector, 4> flushed{};
        std::array<Vector, 2> comparison_inputs;
        std::array<Vector, 5> comparison_outputs{};
        std::array<Vector, 2> pack_inputs;
        Vector minimum{};
    } state{};

    constexpr u64 initial = 0xa5a50000bc00003cULL;
    state.flags = initial;
    state.mxcsr = 0x1f81;  // Raw sticky bit is software-owned and preserved.
    state.inputs = {singles(0x3fc00000, 0xc0000000, 0, 0x41000000),
                    singles(0x3f000000, 0x40800000, 0x40000000, 0x40000000),
                    Vector{0x4010000000000000ULL, 0xc020000000000000ULL},
                    Vector{0x4000000000000000ULL, 0x3fe0000000000000ULL}};
    [[maybe_unused]] const std::array expected{
            singles(0x40000000, 0x40000000, 0x40000000, 0x41200000),
            singles(0x3f800000, 0xc0c00000, 0xc0000000, 0x40c00000),
            singles(0x3f400000, 0xc1000000, 0, 0x41800000),
            singles(0x40400000, 0xbf000000, 0, 0x40800000),
            Vector{0x4018000000000000ULL, 0xc01e000000000000ULL},
            Vector{0x4000000000000000ULL, 0xc021000000000000ULL},
            Vector{0x4020000000000000ULL, 0xc010000000000000ULL},
            Vector{0x4000000000000000ULL, 0xc030000000000000ULL},
    };
    state.denormals = {singles(1, 0x80000001, 0x00800000, 0x80800000),
                       singles(0x7e800000, 0x7e800000, 0x3f000000, 0x3f000000),
                       Vector{1, 0x8010000000000000ULL},
                       Vector{0x7fd0000000000000ULL, 0x3fe0000000000000ULL}};
    [[maybe_unused]] const std::array flush_expected{
            singles(0, 0x80000000, 0x00400000, 0x80400000),
            Vector{0, 0x8008000000000000ULL},
            singles(0x34000000, 0xb4000000, 0, 0x80000000),
            Vector{0x3cb0000000000000ULL, 0x8000000000000000ULL},
    };
    state.comparison_inputs = {singles(0xbf800000, 0, 0x40000000, 0x7fc01234),
                               singles(0, 0x80000000, 0x3f800000, 0x3f800000)};
    [[maybe_unused]] const std::array comparison_expected{
            singles(0xffffffff, 0, 0, 0),
            singles(0, 0xffffffff, 0, 0),
            singles(0xffffffff, 0, 0xffffffff, 0xffffffff),
            singles(0xffffffff, 0xffffffff, 0xffffffff, 0),
            singles(0xffffffff, 0xffffffff, 0, 0),
    };
    state.pack_inputs = {Vector{0x00010000ffff8000ULL, 0x7fff010000ff00feULL},
                         Vector{0x0080007f0002fffeULL, 0x0400fed4012c00ffULL}};
    const std::array ops{OpCode::VecFAdd, OpCode::VecFSub, OpCode::VecFMul, OpCode::VecFDiv};
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto load_vector = [&](std::uint32_t offset) {
        return Need(builder.Emit<OpCode::LoadUniform>(Type::V128,
                                                      {Operand::Uniform(offset, Type::V128)}));
    };
    const auto store_vector = [&](std::uint32_t offset, Value value) {
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value)}));
    };
    for (unsigned type = 0; type < 2; ++type) {
        const auto left = load_vector(offsetof(State, inputs) + type * 32);
        const auto right = load_vector(offsetof(State, inputs) + type * 32 + 16);
        for (unsigned op = 0; op < ops.size(); ++op) {
            const auto result =
                    Need(builder.Append(ops[op],
                                        Type::V128,
                                        {Operand::ValueOf(left),
                                         Operand::ValueOf(right),
                                         Operand::Immediate(type ? 64 : 32, Type::U8)}));
            store_vector(offsetof(State, outputs) + (type * 4 + op) * 16, result);
        }
    }
    const auto compare_left = load_vector(offsetof(State, comparison_inputs));
    const auto compare_right = load_vector(offsetof(State, comparison_inputs) + 16);
    constexpr std::array relations{1u, 2u, 13u, 7u};  // LT, EQ, NEQ_UQ and ORD.
    for (unsigned i = 0; i < relations.size(); ++i)
        store_vector(
                offsetof(State, comparison_outputs) + i * 16,
                Need(builder.Emit<OpCode::VecFCmpMask>(Type::V128,
                                                       {Operand::ValueOf(compare_left),
                                                        Operand::ValueOf(compare_right),
                                                        Operand::Immediate(32, Type::U8),
                                                        Operand::Immediate(relations[i], Type::U8),
                                                        Operand::Immediate(0, Type::U8)})));
    const auto denormal32 = load_vector(offsetof(State, denormals)),
               factor32 = load_vector(offsetof(State, denormals) + 16);
    const auto denormal64 = load_vector(offsetof(State, denormals) + 32),
               factor64 = load_vector(offsetof(State, denormals) + 48);
    const auto zeros = Need(builder.Emit<OpCode::VecSharedZero>(Type::V128, {}));
    for (unsigned output_flush = 0; output_flush < 2; ++output_flush) {
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(offsetof(State, mxcsr), Type::U32),
                 Operand::ValueOf(Constant(builder, output_flush ? 0x9f81 : 0x1fc1), Type::U32)}));
        store_vector(offsetof(State, flushed) + output_flush * 32,
                     Need(builder.Emit<OpCode::VecFMul>(Type::V128,
                                                        {Operand::ValueOf(denormal32),
                                                         Operand::ValueOf(factor32),
                                                         Operand::Immediate(32, Type::U8)})));
        store_vector(offsetof(State, flushed) + output_flush * 32 + 16,
                     Need(builder.Emit<OpCode::VecFMul>(Type::V128,
                                                        {Operand::ValueOf(denormal64),
                                                         Operand::ValueOf(factor64),
                                                         Operand::Immediate(64, Type::U8)})));
        if (!output_flush)
            store_vector(
                    offsetof(State, comparison_outputs) + 4 * 16,
                    Need(builder.Emit<OpCode::VecFCmpMask>(Type::V128,
                                                           {Operand::ValueOf(denormal32),
                                                            Operand::ValueOf(zeros),
                                                            Operand::Immediate(32, Type::U8),
                                                            Operand::Immediate(2, Type::U8),
                                                            Operand::Immediate(0, Type::U8)})));
    }
    const auto pack_left = load_vector(offsetof(State, pack_inputs));
    const auto pack_right = load_vector(offsetof(State, pack_inputs) + 16);
    store_vector(offsetof(State, pack_inputs) + 16,
                 Need(builder.Emit<OpCode::VecPack>(Type::V128,
                                                    {Operand::ValueOf(pack_left),
                                                     Operand::ValueOf(pack_right),
                                                     Operand::Immediate(16, Type::U8),
                                                     Operand::Immediate(1, Type::U8)})));
    const auto minimum_left = Need(builder.Emit<OpCode::VecLoadConst>(
            Type::V128,
            {Operand::Immediate(0x0080ff1020304050ULL, Type::U64),
             Operand::Immediate(0xff80017f00fe0203ULL, Type::U64)}));
    const auto minimum_right = Need(builder.Emit<OpCode::VecLoadConst>(
            Type::V128,
            {Operand::Immediate(0x0181fe2020108060ULL, Type::U64),
             Operand::Immediate(0x7fff027f01ff0104ULL, Type::U64)}));
    store_vector(offsetof(State, minimum),
                 Need(builder.Emit<OpCode::VecMin>(Type::V128,
                                                   {Operand::ValueOf(minimum_left),
                                                    Operand::ValueOf(minimum_right),
                                                    Operand::Immediate(8, Type::U8),
                                                    Operand::Immediate(0, Type::U8)})));
    Need(builder.EndBlock(Terminal::Return(Constant(builder, 42))));
    auto program = Submit(builder);
    arm64::CompileOptions options{offsetof(State, flags)};
    options.mxcsr_flush_offset = offsetof(State, mxcsr);
    const auto code = ::Compile(program, options);
    for (const auto opcode : {0x4e20d400u,
                              0x4ea0d400u,
                              0x6e20dc00u,
                              0x6e20fc00u,
                              0x4e60d400u,
                              0x4ee0d400u,
                              0x6e60dc00u,
                              0x6e60fc00u})
        Check(std::any_of(code.words.begin(),
                          code.words.end(),
                          [&](std::uint32_t word) { return (word & 0xffe0fc00u) == opcode; }),
              "packed IEEE arithmetic uses the actual 4S/2D operation");
    Check(std::any_of(code.words.begin(),
                      code.words.end(),
                      [](std::uint32_t word) { return (word & 0xfffffc00u) == 0x2e212800u; }) &&
                  std::any_of(
                          code.words.begin(),
                          code.words.end(),
                          [](std::uint32_t word) { return (word & 0xfffffc00u) == 0x6e212800u; }),
          "signed16 to unsigned8 packing uses saturating SQXTUN and SQXTUN2");
#if defined(__aarch64__) && defined(__ANDROID__)
    // Standalone compiled units borrow their caller's FP environment. Set
    // it explicitly; the separate runtime Root owns its guest/host switches.
    struct RestoreFP {
        u64 fpcr{}, fpsr{};

        ~RestoreFP() { asm volatile("msr fpcr,%0\nmsr fpsr,%1" ::"r"(fpcr), "r"(fpsr) : "memory"); }
    } restore;

    asm volatile("mrs %0,fpcr\nmrs %1,fpsr" : "=r"(restore.fpcr), "=r"(restore.fpsr));
    asm volatile("msr fpcr,xzr\nmsr fpsr,xzr" ::: "memory");
#endif
    Execute(code, reinterpret_cast<std::uint8_t*>(&state), 42);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(state.outputs == expected && state.flushed == flush_expected,
          "packed arithmetic computes every lane and independently applies DAZ inputs or FTZ "
          "outputs with signed zeros");
    Check(state.comparison_outputs == comparison_expected,
          "four-lane IEEE compare masks distinguish LT/EQ/GT/unordered and apply the actual DAZ "
          "control");
    Check(state.pack_inputs[0] == Vector{0x00010000ffff8000ULL, 0x7fff010000ff00feULL} &&
                  state.pack_inputs[1] == Vector{0xfffffffe01000000ULL, 0xff00ffff807f0200ULL},
          "packing clamps signed negatives and overflow to 0..255 while publishing over the "
          "original right field");
    Check(state.minimum == Vector{0x0080fe1020104050ULL, 0x7f80017f00fe0103ULL},
          "unsigned byte minimum compares all sixteen complete lanes including values above "
          "signed-byte range");
    Check(state.flags == initial && state.mxcsr == 0x9f81,
          "packed arithmetic preserves whole guest flags and the raw software-owned MXCSR "
          "status/control word");
#endif
    // This separate finite-only unit never changes its software flush mode.
    // The preceding mixed-control unit deliberately retains the unknown API.
    Builder finite{Scope::Block};
    Need(finite.BeginBlock(Create(finite)));
    for (unsigned type = 0; type < 2; ++type) {
        const auto left = Need(finite.Emit<OpCode::LoadUniform>(
                Type::V128, {Operand::Uniform(offsetof(State, inputs) + type * 32, Type::V128)}));
        const auto right = Need(finite.Emit<OpCode::LoadUniform>(
                Type::V128,
                {Operand::Uniform(offsetof(State, inputs) + type * 32 + 16, Type::V128)}));
        for (unsigned op = 0; op < ops.size(); ++op) {
            const auto result = Need(finite.Append(ops[op],
                                                   Type::V128,
                                                   {Operand::ValueOf(left),
                                                    Operand::ValueOf(right),
                                                    Operand::Immediate(type ? 64 : 32, Type::U8)}));
            Need(finite.Emit<OpCode::StoreUniform>(
                    Type::Void,
                    {Operand::Uniform(offsetof(State, outputs) + (type * 4 + op) * 16, Type::V128),
                     Operand::ValueOf(result)}));
        }
    }
    Need(finite.EndBlock(Terminal::Return(Constant(finite, 42))));
    auto finite_program = Submit(finite);
    auto known_options = options;
    known_options.mxcsr_flush_mode = 0;
    const auto known_default = ::Compile(finite_program, known_options);
    auto direct_options = options;
    direct_options.mxcsr_flush_offset = UINT32_MAX;
    const auto direct = ::Compile(finite_program, direct_options);
    Check(known_default.words == direct.words,
          "known default software controls emit exactly the direct packed arithmetic path without "
          "copies, loads or control branches");
    state.mxcsr = 0x1f81;
    state.outputs = {};
    Execute(known_default, reinterpret_cast<std::uint8_t*>(&state), 42);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(state.outputs == expected && state.flags == initial && state.mxcsr == 0x1f81,
          "known default packed arithmetic preserves every finite lane, whole flags and raw "
          "status");
#endif
    state.inputs = state.denormals;
    for (const std::uint8_t mode : {std::uint8_t{1}, std::uint8_t{2}}) {
        known_options.mxcsr_flush_mode = mode;
        const auto known = ::Compile(finite_program, known_options);
        state.mxcsr = mode == 1 ? 0x1fc1 : 0x9f81;
        state.outputs = {};
        Execute(known, reinterpret_cast<std::uint8_t*>(&state), 42);
#if defined(__aarch64__) && defined(__ANDROID__)
        const auto output = mode == 1 ? 0u : 2u;
        Check(state.outputs[2] == flush_expected[output] &&
                      state.outputs[6] == flush_expected[output + 1] && state.flags == initial &&
                      state.mxcsr == (mode == 1 ? 0x1fc1u : 0x9f81u),
              "known DAZ-only and FTZ-only modes retain their independent signed subnormal "
              "semantics without raw control reads");
#endif
    }
}

void VectorLaneMasks() {
    std::array<std::uint8_t, 128> state{};
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto value = Need(builder.Emit<OpCode::VecLoadConst>(
            Type::V128,
            {Operand::Immediate(0xaa5500ff01817f80ULL, Type::U64),
             Operand::Immediate(0x01ff8055007f8001ULL, Type::U64)}));
    const std::array<unsigned, 4> lanes{8, 16, 32, 64};
    Value last;
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        last = Need(builder.Emit<OpCode::VecMovMask>(
                i & 1 ? Type::U64 : Type::U32,
                {Operand::ValueOf(value), Operand::Immediate(lanes[i], Type::U8)}));
        Store(builder, static_cast<std::uint32_t>(i * 8), last);
    }
    const auto memory_base = Constant(builder, reinterpret_cast<std::uintptr_t>(state.data() + 80));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x4450, Type::U64)}));
    Need(builder.Emit<OpCode::StoreMemoryTSO>(Type::Void,
                                              {Operand::Expression(OperandOp::Add, 2),
                                               Operand::ValueOf(memory_base),
                                               Operand::Immediate(16, Type::U64),
                                               Operand::ValueOf(value)}));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x4460, Type::U64)}));
    Need(builder.Emit<OpCode::StoreMemoryTSO>(Type::Void,
                                              {Operand::Expression(OperandOp::Sub, 2),
                                               Operand::ValueOf(memory_base),
                                               Operand::Immediate(16, Type::U64),
                                               Operand::ValueOf(value)}));
    Store(builder, 32, Snapshot(builder, last));
    const auto carry =
            Need(builder.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::Carry)}));
    Need(builder.EndBlock(Terminal::Return(carry)));
    auto program = Submit(builder);
    arm64::CompileOptions options{48};
    options.fault_locations = true;
#if defined(__aarch64__) && defined(__ANDROID__)
    options.native_nzcv = (getauxval(AT_HWCAP) & HWCAP_FLAGM) != 0;
#else
    options.native_nzcv = true;
#endif
    const auto code = ::Compile(program, options);
    Check(std::any_of(code.words.begin(),
                      code.words.end(),
                      [](std::uint32_t word) {
                          return (word & 0xffc00000u) == 0x3d800000u && ((word >> 10) & 4095u) == 1;
                      }) &&
                  code.faults.size() == 2 &&
                  std::all_of(code.faults.begin(),
                              code.faults.end(),
                              [](const arm64::FaultSite& site) {
                                  return WordOffset(site.unaligned_offset) == UINT32_MAX;
                              }),
          "V128 TSO store uses STR Q scaled16 without RCpc and retains ordinary fault sites");
    constexpr u64 initial = 0xa5a50000bc00003cULL;
    Write64(state.data() + 48, initial);
    Execute(code, state.data(), 1);
#if defined(__aarch64__) && defined(__ANDROID__)
    Check(Read64(state.data()) == 0x6295 && Read64(state.data() + 8) == 0x58 &&
                  Read64(state.data() + 16) == 2 && Read64(state.data() + 24) == 1,
          "lane masks extract arbitrary input sign bits in low-lane order and clear high bits");
    Check(Read64(state.data() + 32) == initial && Read64(state.data() + 48) == initial,
          "lane masks preserve guest flags");
    Check(Read64(state.data() + 64) == 0xaa5500ff01817f80ULL &&
                  Read64(state.data() + 72) == 0x01ff8055007f8001ULL &&
                  Read64(state.data() + 96) == 0xaa5500ff01817f80ULL &&
                  Read64(state.data() + 104) == 0x01ff8055007f8001ULL &&
                  Read64(state.data() + 80) == 0 && Read64(state.data() + 88) == 0,
          "positive vector offset and negative full-EA fallback preserve the original base and all "
          "data bits");
#endif
}

struct QueryPair {
    u64 lower, upper;
};

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
    options.native_nzcv = (getauxval(AT_HWCAP) & HWCAP_FLAGM) != 0;
#else
    options.native_nzcv = true;
#endif
    const auto code = ::Compile(program, options);
    constexpr u64 initial = 0xa5a50000bc00003cULL;
    std::array<std::uint8_t, 64> state{};
    Write64(state.data() + 48, initial);
    Execute(code, state.data(), 1);
#if defined(__aarch64__) && defined(__ANDROID__)
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
        const auto capabilities = getauxval(AT_HWCAP);
        if (lse && !(capabilities & HWCAP_ATOMICS)) continue;
        options.native_nzcv = (capabilities & HWCAP_FLAGM) != 0;
#else
        options.native_nzcv = true;
#endif
        const auto code = ::Compile(program, options);
        std::array<std::uint8_t, 144> state{};
        Write64(state.data() + 128, initial);
        memory = 5;
        Execute(code, state.data(), 1);
#if defined(__aarch64__) && defined(__ANDROID__)
        for (std::size_t i = 0; i < expected.size(); ++i)
            Check(Read64(state.data() + i * 8) == expected[i],
                  "atomic sequence returns observed old values");
        Check(memory == 0x1270, "mixed-width atomics preserve untouched bytes and explicit carry");
        Check(Read64(state.data() + 112) == initial && Read64(state.data() + 128) == initial,
              "internal atomic predicates preserve guest NZCV, AF and parity");
#endif
    }
}

void LinkedPinnedUniforms() {
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto old = Load(builder, 0), changed = Constant(builder, 0xabcdef01000000aaULL);
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(1, Type::U8), Operand::ValueOf(changed, Type::U8)}));
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(8, Type::U64), Operand::ValueOf(changed, Type::U32)}));
    StoreCarrier(builder, 80, old);  // A pin write must not retroactively change this SSA read.
    const auto deferred =
            Need(builder.Emit<OpCode::LoadImm>(Type::U8, {Operand::Immediate(0x77, Type::U8)}));
    const auto unrelated = Load(builder, 96);
    Store(builder, 16, deferred);
    const auto later = Need(builder.Emit<OpCode::Add>(
            Type::U64, {Operand::ValueOf(deferred), Operand::ValueOf(unrelated)}));
    StoreCarrier(builder, 104, later);
    Need(builder.Emit<OpCode::UniformBarrier>(Type::Void, {}));
    const auto target = Constant(builder, 0x1234567890ULL);
    Store(builder, 128, target);
    Need(builder.EndBlock(Terminal::Dispatch()));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.pc_offset = 128;
    options.pinned_uniform_offset = 0;
    options.private_uniforms = true;
    const auto locations = arm64::AllocateBlock(program, &options);
    Check(bool(locations) && locations.homes[deferred.node.value].reg == 22 &&
                  !locations.homes[deferred.node.value].narrow_pin,
          "actual private ABI coalesces a non-adjacent numeric publication through its later uses");
    const auto code = arm64::Compile(program, options);
    Check(bool(code) && code.frame_bytes == 0 && code.spill_slots == 0 &&
                  code.words.front() == 0xaa1403e2u,
          "linked unit copies pinned state to an ordinary SSA home without a root prologue");
    Check(std::any_of(code.words.begin(),
                      code.words.end(),
                      [](std::uint32_t word) {
                          return (word & 0xffe0ffffu) ==
                                 0x2a0003f5u;  // MOV W21,Wn zeroes the high half.
                      }),
          "U32 input into a full U64 pinned field clears its high half");
    for (unsigned i = 0; i < 8; ++i)
        Check(std::count(code.words.begin(),
                         code.words.end(),
                         0xf9000000u | (i << 10) | (19u << 5) | (20u + i)) == 1,
              "UniformBarrier publishes each actual pinned slot once");
    Check(code.links.size() == 1 && code.links[0].target_pc == 0x1234567890ULL,
          "only the real local full-width PC store creates a direct link");
    const auto pointer = WordOffset(code.links[0].data_offset);
    const auto branch = WordOffset(code.links[0].branch_offset);
    Check(!(pointer & 1) && branch < pointer && pointer + 1 < code.words.size() &&
                  pointer - branch == 2 + (branch & 1) &&
                  code.words[branch] == (0x58000010u | ((pointer - branch) << 5)) &&
                  code.words[branch + 1] == 0xd61f0200u &&
                  (u64{code.words[pointer]} | (u64{code.words[pointer + 1]} << 32)) ==
                          options.dispatcher,
          "direct link names an aligned dispatcher pointer DATA relocation");
    Check(std::find(code.words.begin(), code.words.end(), 0xf9404661u) == code.words.end() &&
                  std::find(code.words.begin(), code.words.end(), 0xf9004661u) == code.words.end(),
          "private linked entry/dispatch keep the live flags carrier without state "
          "reload/publication");
}

void LinkedExitAndCrossSlot() {
    Builder builder{Scope::Function};
    const auto entry = Create(builder), dispatch = Create(builder), exit = Create(builder);
    Need(builder.BeginBlock(entry));
    const auto target = Constant(builder, 0x87654321);
    const auto condition = Load(builder, 80);
    Need(builder.EndBlock(Terminal::Branch(condition, dispatch, exit)));
    Need(builder.BeginBlock(dispatch));
    Store(builder, 128, target);
    Need(builder.EndBlock(Terminal::Dispatch()));
    Need(builder.BeginBlock(exit));
    const auto crossing = Load(builder, 7);
    const auto reason = Constant(builder, 1);
    Need(builder.EndBlock(Terminal::Return(reason)));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.pc_offset = 128;
    options.pinned_uniform_offset = 0;
    options.private_uniforms = true;
    const auto rejected = arm64::Compile(program, options);
    Check(!rejected && rejected.error == arm64::CompileError::UnsupportedOperand &&
                  rejected.words.empty() && rejected.links.empty(),
          "the explicit pin ABI rejects a field that crosses slot boundaries");
    auto editor = Builder::Edit(std::move(program));
    Need(editor.Rewrite(
            crossing.node, OpCode::LoadUniform, Type::U64, {Operand::Uniform(80, Type::U64)}));
    auto admitted = Submit(editor);
    const auto code = arm64::Compile(admitted, options);
    Check(bool(code) && code.links.empty() && code.frame_bytes == 0,
          "foreign-block PC literals retain dispatcher lookup without a guessed direct target");
    bool exit_pointer{}, dispatcher_pointer{};
    for (std::size_t at = 0; at + 1 < code.words.size(); ++at) {
        if ((code.words[at] & 0xff00001fu) != 0x58000010u || code.words[at + 1] != 0xd61f0200u)
            continue;
        const auto encoded = (code.words[at] >> 5) & 0x7ffffu;
        const auto distance =
                encoded < 0x40000u ? std::int64_t{encoded} : std::int64_t{encoded} - 0x80000;
        const auto data = static_cast<std::int64_t>(at) + distance;
        if (data < 0 || static_cast<std::uint64_t>(data) + 1 >= code.words.size()) continue;
        const auto pointer = u64{code.words[data]} | (u64{code.words[data + 1]} << 32);
        exit_pointer |= pointer == options.exit_handler;
        dispatcher_pointer |= pointer == options.dispatcher;
    }
    Check(exit_pointer && dispatcher_pointer &&
                  std::find(code.words.begin(), code.words.end(), 0xd65f03c0u) == code.words.end(),
          "linked Return and Dispatch target their separate runtime services without a unit RET");
}

void LinkedRawAuxiliaryCarry() {
#if defined(__aarch64__) && defined(__ANDROID__)
    if (!(getauxval(AT_HWCAP) & HWCAP_FLAGM)) return;
    namespace execution = swift::runtime::execution;

    struct ObservedState {
        swift::runtime::x86::GuestState state;
        std::array<u64, 17> comparisons{};
    };

    constexpr auto comparisons = offsetof(ObservedState, comparisons);
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
    options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCVCounted);
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
            Type::U64, {Operand::ValueOf(fifteen), Operand::Immediate(1, Type::U64)}, metadata));
    const auto no_auxiliary = Need(
            first.Emit<OpCode::TestNotFlags>(Type::Bool, {Operand::Flags(Flag::AuxiliaryCarry)}));
    Need(first.Emit<OpCode::StoreUniformSelectImm>(Type::Void,
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
    (void)Need(
            second.Emit<OpCode::Xor>(Type::U64,
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
    const auto narrow = Need(second.Emit<OpCode::Adc>(
            Type::U8,
            {Operand::ValueOf(Constant(second, 0x7f), Type::U8), Operand::Immediate(0, Type::U8)},
            metadata));
    Store(second, offsetof(swift::runtime::x86::GuestState, gprs) + 5 * sizeof(u64), narrow);
    Store(second,
          offsetof(swift::runtime::x86::GuestState, gprs) + 6 * sizeof(u64),
          Snapshot(second, narrow));
    const auto result = Need(second.Emit<OpCode::Adc>(
            Type::U64, {Operand::ValueOf(adc_left), Operand::Immediate(1, Type::U64)}, metadata));
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
    const auto raw_alias =
            Need(second.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(raw_source, Type::U8)}));
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
          Need(second.Emit<OpCode::Xor>(
                  Type::U8,
                  {Operand::ValueOf(raw_alias, Type::U8), Operand::Immediate(0xab, Type::U8)})));
    Store(second,
          observations + 24,
          Need(second.Emit<OpCode::Or>(
                  Type::U8, {Operand::ValueOf(raw_alias), Operand::Immediate(0x80, Type::U8)})));
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
    const auto byte_snapshot =
            Need(second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
    const auto pin_word_snapshot =
            Need(second.Emit<OpCode::LoadUniform>(Type::U16, {Operand::Uniform(pin15, Type::U16)}));
    const auto address_base = Constant(second, 0x1000);
    Store(second,
          observations + 120,
          Need(second.Emit<OpCode::GetOperand>(Type::U64,
                                               {Operand::Expression(OperandOp::AddShift, 2),
                                                Operand::ValueOf(address_base),
                                                Operand::ValueOf(byte_snapshot, Type::U64)})));
    Store(second,
          observations + 128,
          Need(second.Emit<OpCode::GetOperand>(Type::U64,
                                               {Operand::Expression(OperandOp::AddShift, 2),
                                                Operand::ValueOf(address_base),
                                                Operand::ValueOf(pin_word_snapshot, Type::U32)})));
    const auto zero_byte_snapshot =
            Need(second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin15, Type::U8)}));
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
    const auto numeric_word =
            Need(second.Emit<OpCode::LoadImm>(Type::U16, {Operand::Immediate(0x1000, Type::U16)}));
    (void)Need(second.Emit<OpCode::Sub>(
            Type::U16,
            {Operand::ValueOf(numeric_word), Operand::Immediate(0x100001000ULL, Type::U64)},
            {FlagBits(Flag::Carry | Flag::Zero), FlagProfile::ArithmeticCanonicalCarry, false}));
    Store(second, observations + 208, Snapshot(second, raw_source));
    (void)Need(second.Emit<OpCode::Sub>(
            Type::U16,
            {Operand::ValueOf(byte_snapshot, Type::Bool), Operand::ValueOf(pin_word_snapshot)},
            {FlagBits(Flag::Carry | Flag::Zero), FlagProfile::ArithmeticCanonicalCarry, false}));
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
    const OperationMetadata full_logical{FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false};
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
    const auto cross_byte =
            Need(second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
    const auto ah_snapshot = Need(
            second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14 + 1, Type::U8)}));
    Store(second, pin14, Constant(second, 0xaaaaaaaaaaaaaa55ULL));
    Store(second,
          observations + 80,
          Need(second.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(cross_byte)})));
    Store(second, observations + 104, ah_snapshot);
    const auto cross_word =
            Need(second.Emit<OpCode::LoadUniform>(Type::U16, {Operand::Uniform(pin15, Type::U16)}));
    const auto full_byte =
            Need(second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin15, Type::U8)}));
    Store(second, pin15, full_byte);
    Store(second,
          observations + 96,
          Need(second.Emit<OpCode::BitCast>(Type::U64, {Operand::ValueOf(cross_word)})));
    Need(second.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(pin14, Type::U8), Operand::ValueOf(live_byte_xor)}));
    Need(second.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(pin15, Type::U16), Operand::ValueOf(live_word_xor)}));
    const auto partial_left =
            Need(second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
    const auto partial_right =
            Need(second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin15, Type::U8)}));
    const auto partial_xor = Need(second.Emit<OpCode::Xor>(
            Type::U8,
            {Operand::ValueOf(partial_left), Operand::ValueOf(partial_right)},
            full_logical));
    Need(second.Emit<OpCode::StoreUniform>(
            Type::Void,
            {Operand::Uniform(pin14, Type::U8), Operand::ValueOf(partial_xor, Type::U8)}));
    Store(second, comparisons + 10 * sizeof(u64), Snapshot(second, raw_source));
    const auto partial_input =
            Need(second.Emit<OpCode::LoadUniform>(Type::U8, {Operand::Uniform(pin14, Type::U8)}));
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
    const auto madd_accumulator =
            Need(second.Emit<OpCode::LoadUniform>(Type::U32, {Operand::Uniform(pin14, Type::U32)}));
    const auto product = Need(second.Emit<OpCode::Mul>(
            Type::U32,
            {Operand::ValueOf(madd_left, Type::U32), Operand::ValueOf(madd_right, Type::U32)}));
    const auto sum = Need(second.Emit<OpCode::Add>(
            Type::U32,
            {Operand::ValueOf(product, Type::U32), Operand::ValueOf(madd_accumulator, Type::U32)}));
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
                          second_allocation.homes[live_byte_xor.node.value].definition_position &&
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
          "the adjacent U32 multiply/add uses actual unspilled numeric inputs and result homes");
    const auto second_code = arm64::Compile(second_program, options);
    Check(bool(second_code) && !second_code.words.empty(), "RawAF linked ADC bridge compilation");
    Check(std::count(second_code.words.begin(), second_code.words.end(), 0x3a00082du) == 3 &&
                  std::count(second_code.words.begin(), second_code.words.end(), 0x3a00482du) == 1,
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
    Check(std::count_if(second_code.words.begin(),
                        second_code.words.end(),
                        [](std::uint32_t word) { return (word & 0xfffffc1fu) == 0xf1000001u; }) >=
                  2,
          "full-width self TEST uses SUBS into the PF carrier rather than ANDS plus carry "
          "inversion");
    const auto partial_add_reg = second_allocation.homes[partial_add.node.value].reg;
    const std::array partial_add_words{
            0x51000000u | (0x3au << 10) |
                    (second_allocation.homes[partial_input.node.value].reg << 5) | partial_add_reg,
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
    const auto flagged_mul_word =
            multiply_word | (31u << 10) | second_allocation.homes[flagged_product.node.value].reg;
    Check(std::find(second_code.words.begin(), second_code.words.end(), madd_word) !=
                          second_code.words.end() &&
                  std::find(second_code.words.begin(), second_code.words.end(), flagged_mul_word) !=
                          second_code.words.end(),
          "the unflagged adjacent U32 pair uses MADD while the flags-producing Add retains its "
          "separate multiply");

    execution::CodeHeap arena{TestBackendOps(), static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
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
          "initial relocation addresses the RX cold stub before the mapping becomes executable");
    auto* writable = entry.writable
                             ? reinterpret_cast<std::uintptr_t*>(
                                       entry.writable + WordOffset(selected->data_offset) * 4)
                             : nullptr;
    const auto back_words = WordOffset(selected->data_offset) - WordOffset(selected->branch_offset);
    if (writable)
        PatchLinkWords(
                writable, executable, reinterpret_cast<std::uintptr_t>(ready.entry), back_words);
    ObservedState observed_state;
    auto& state = observed_state.state;
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
    execution::NativeRunContext context;
    context.opaque = &resolver;
    context.resolve = [](void* opaque,
                         execution::NativeRunContext* current) noexcept -> std::uintptr_t {
        auto& request = *static_cast<Resolver*>(opaque);
        ++request.calls;
        return request.calls == 1 && current->state->pc == entry_pc
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
    Check(execution::SvmRunNative(&state, &context) == 16, "RawAF linked ADC numeric result");
    Check(resolver.calls == 1 && context.dispatcher_entries == (writable ? 0u : 1u) &&
                  context.cache_hits == (writable ? 0u : 1u),
          "a resolved edge bypasses PC setup; W^X cold stub keeps the ordinary ready lookup");
    Check(state.gprs[13] == next_pc,
          "target's actual PC observer materializes entry PC after a direct link");
    Check(state.gprs[0] == 1 && state.gprs[1] == 0, "RawAF hot-edge observation and masked import");
    Check(state.gprs[8] == 0xbb && state.gprs[9] == 0xcc && state.gprs[10] == 0xaa &&
                  state.gprs[11] == 0x1111,
          "delayed pins respect intervening reads and partial writes across a real linked edge");
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
    Check(state.gprs[7] == 0x7fffffff && state.gprs[12] == ((initial & ~written) | 0x140000ffULL),
          "raw SBB32 publishes canonical carry/overflow and keeps an older SSA alive across "
          "snapshots");
    Check(Read64(state.xmm) == ((initial & ~written) | 0xd40000a5ULL) &&
                  Read64(state.xmm[0] + 8) == ((initial & ~written) | 0x940000a5ULL),
          "byte/word eager zero TEST preserves N/C/V, raw AF/PF and whole-word reserved flags");
    Check(Read64(state.xmm[1]) == 0xab && Read64(state.xmm[1] + 8) == 0x80 &&
                  Read64(state.xmm[2]) == 0x135,
          "narrow bitwise outputs retain raw-view cuts and wide/compound fallback normalization");
    Check(Read64(state.xmm[4]) == 0x80 && Read64(state.xmm[4] + 8) == 0x80 &&
                  Read64(state.xmm[7]) == 0x80,
          "borrowed bytes expose canonical SSA carriers to raw/wide reads and cut a physical "
          "bitwise result once");
    Check(Read64(state.xmm[7] + 8) == 0x1080 && Read64(state.xmm[8]) == 0xf100,
          "low8/16 address inputs cannot become an unmasked UXTW index through wider views");
    const auto aligned_add_flags = ReferenceArithmetic(
            OpCode::Add, 0x80, 0x80, 8, metadata.profile, metadata.flag_outputs, initial);
    const auto aligned_sub_flags = ReferenceArithmetic(
            OpCode::Sub, 0, 0xe100, 16, metadata.profile, metadata.flag_outputs, aligned_add_flags);
    Check(Read64(state.xmm[9]) == 0 && Read64(state.xmm[9] + 8) == aligned_add_flags &&
                  Read64(state.xmm[10]) == 0x1f00 && Read64(state.xmm[10] + 8) == aligned_sub_flags,
          "aligned byte/word ALU cuts dirty pin inputs, keeps a Bool read narrow and publishes "
          "exact AF/PF/NZCV/reserved bits");
    const auto zero_sub_flags = ReferenceArithmetic(
            OpCode::Sub, 0x80, 0, 8, metadata.profile, metadata.flag_outputs, aligned_sub_flags);
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
    Check(Read64(state.xmm[12]) == byte_cmp_flags && Read64(state.xmm[12] + 8) == raw_cmp_flags &&
                  Read64(state.xmm[13]) == word_cmp_flags &&
                  Read64(state.xmm[13] + 8) == borrowed_cmp_flags,
          "dead narrow CMP preserves PF/AF/reserved and N/V, handles carry profiles and retains "
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
          "full dead byte/word TEST preserves raw PF/AF/NZCV/reserved and respects a narrower Bool "
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
    Check(observed_state.comparisons[15] == (observed_state.comparisons[14] & ~(u64{1} << 29)) &&
                  observed_state.comparisons[16] ==
                          (observed_state.comparisons[14] | (u64{1} << 29)),
          "adjacent nonzero predicates set only CF from their logical byte view and preserve "
          "NZV, raw PF/AF and reserved bits");
    Check(Read64(state.xmm[14]) == 0x81 &&
                  Read64(state.xmm[14] + 8) == ((initial & ~written) | 0x80000081ULL) &&
                  Read64(state.xmm[15]) == 0xe110 &&
                  Read64(state.xmm[15] + 8) == ((initial & ~written) | 0x80000010ULL),
          "full byte/word XOR keeps canonical numeric results while its signed PF carrier supplies "
          "exact flags");
    Check(Read64(state.xmm[8] + 8) == ((initial & ~written) | 0xd40000a5ULL),
          "a wider zero predicate tests only the borrowed byte and preserves every other flag");
    Check(Read64(state.xmm[5]) == 0x80 && Read64(state.xmm[5] + 8) == byte_carrier &&
                  Read64(state.xmm[6]) == 0xe100 && Read64(state.xmm[6] + 8) == 0x43 &&
                  state.gprs[14] == 0xaaaaaaaaaaaaaa57ULL && state.gprs[15] == 0xe110,
          "borrowed reads and full publications preserve snapshots; later partial XOR writes keep "
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
        Check(execution::SvmRunNative(&state, &context) == 16 && resolver.calls == 1 &&
                      context.dispatcher_entries == 1 && context.cache_hits == 1 &&
                      state.gprs[13] == next_pc && state.packed_flags == expected,
              "unlinked edge republishes its PC and preserves the raw ABI through the existing "
              "dispatcher");
    }
#endif
}

void FunctionDeferredTestFaults() {
#if defined(__aarch64__) && defined(__ANDROID__)
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
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(load_pc, Type::U64)}));
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
    const auto nonzero =
            Need(builder.Emit<OpCode::CondSet>(Type::Bool, {Operand::Condition(Condition::NE)}));
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
    execution::CodeHeap arena{TestBackendOps(), static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    const auto code = arena.Publish(compiled.words, compiled.faults, compiled.links);

    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<std::uint8_t*>(
            mmap(nullptr, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
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
        swift::runtime::x86::GuestState state;
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
    scene->native.resolve = [](void* opaque,
                               execution::NativeRunContext* context) noexcept -> std::uintptr_t {
        auto& value = *static_cast<Scene*>(opaque);
        ++value.resolves;
        return value.resolves == 1 && context->state->pc == entry_pc
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
        execution::FaultScope scope(scene->faults, scene->state);
        const auto run = [&](u64 input, u64 expected_pc, u64 expected_flags) {
            scene->state = {};
            scene->state.pc = entry_pc;
            scene->state.packed_flags = initial;
            scene->state.gprs[swift::runtime::x86::R10] = input;
            scene->resolves = 0;
            scene->faults.signal_number = 0;
            if (sigsetjmp(scene->faults.jump, 1) == 0) {
                Check(execution::SvmRunNative(&scene->state, &scene->native) == 0 && !expected_pc,
                      "zero TEST exit returns through the real native ABI");
            } else
                Check(expected_pc && scene->faults.signal_number == SIGSEGV,
                      "tagged TEST faults return through the registered signal owner");
            Check(scene->resolves == 1 && scene->state.packed_flags == expected_flags &&
                          (!expected_pc || scene->state.pc == expected_pc),
                  "TEST fault/exit publishes exact flags, reserved bits and PC");
        };
        run(missing, load_pc, test_flags(0));  // First entry is still Complete XOR-zero.
        Write64(memory, missing);
        run(head, load_pc, test_flags(missing));
        Write64(memory, readonly);
        run(head, store_pc, test_flags(readonly));
        Check(scene->state.gprs[swift::runtime::x86::R10] == loaded_before_store_fault &&
                      scene->state.gprs[swift::runtime::x86::R12] == readonly,
              "store fault uses the entry snapshot after the load overwrote the current pin");
        Write64(memory, 0);
        run(head, 0, test_flags(0));
        Check(scene->state.gprs[swift::runtime::x86::R10] == 0 &&
                      scene->state.gprs[swift::runtime::x86::R11] == head,
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
        Check(scene->faults.host_pc == reinterpret_cast<std::uintptr_t>(code.entry) +
                                               WordOffset(compiled.faults[0].fallback_offset) * 4,
              "cold load faults retain the original guest PC and TEST snapshot");
        Write64(memory, readonly);
        run(head, store_pc, test_flags(readonly));
        Check(scene->faults.host_pc ==
                              reinterpret_cast<std::uintptr_t>(code.entry) +
                                      (WordOffset(compiled.faults[1].fallback_offset) + 1) * 4 &&
                      scene->state.gprs[swift::runtime::x86::R10] == loaded_before_store_fault &&
                      scene->state.gprs[swift::runtime::x86::R12] == readonly,
              "cold store faults use the entry snapshot after the load overwrites its destination "
              "pin");
    }
    Check(munmap(memory, page * 3) == 0, "TEST fault scene release");
#endif
}

void BlockDeferredTestBoundaries() {
#if defined(__aarch64__) && defined(__ANDROID__)
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
    options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCVCounted);
    options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
    const OperationMetadata logical{FlagBits(Flag::All), FlagProfile::LogicalZeroCVA, false};

    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto snapshot = Load(builder, field(swift::runtime::x86::R10));
    Store(builder, field(swift::runtime::x86::R12), snapshot);
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(load_pc, Type::U64)}));
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
    const auto condition =
            Need(builder.Emit<OpCode::CondSet>(Type::Bool, {Operand::Condition(Condition::NE)}));
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
    Check(program.Operations().size() == 12 && bool(allocation) && !allocation.publish_entry_pc &&
                  bool(compiled) && compiled.frame_bytes == 0 && compiled.spill_slots == 0 &&
                  compiled.faults.size() == 2 &&
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
    execution::CodeHeap arena{TestBackendOps(), static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
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
    auto* memory = static_cast<std::uint8_t*>(
            mmap(nullptr, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(memory != MAP_FAILED, "Block TEST boundary mapping");
    const auto head = reinterpret_cast<std::uintptr_t>(memory);
    const auto readonly = head + page, missing = head + page * 2;
    const auto loaded_before_store_fault = head + 0x38;
    Write64(memory + page, loaded_before_store_fault);
    Check(mprotect(memory + page, page, PROT_READ) == 0 &&
                  mprotect(memory + page * 2, page, PROT_NONE) == 0,
          "Block TEST boundary permissions");

    struct Scene {
        swift::runtime::x86::GuestState state;
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
    scene->self_execute =
            reinterpret_cast<const std::uintptr_t*>(begin + WordOffset(self_link->data_offset) * 4);
    scene->self_back_words =
            WordOffset(self_link->data_offset) - WordOffset(self_link->branch_offset);
    auto* write_base = code.writable ? code.writable : reinterpret_cast<std::uint8_t*>(begin);
    scene->self_write =
            reinterpret_cast<std::uintptr_t*>(write_base + WordOffset(self_link->data_offset) * 4);
    const auto* exit_execute =
            reinterpret_cast<const std::uintptr_t*>(begin + WordOffset(exit_link->data_offset) * 4);
    auto* exit_write =
            reinterpret_cast<std::uintptr_t*>(write_base + WordOffset(exit_link->data_offset) * 4);
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
          "far gate preserves the original LDR/BR DATA transfer without narrowing link support");
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
                               (((0u - WordOffset(self_link->branch_offset)) & 0x7ffffu) << 5)) &&
                      words[self_gate] == (0x14000000u | ((0u - self_gate) & 0x03ffffffu)),
              "Block ready self links its actual CBNZ directly to native entry while retaining the "
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
    scene->native.resolve = [](void* opaque,
                               execution::NativeRunContext* context) noexcept -> std::uintptr_t {
        auto& value = *static_cast<Scene*>(opaque);
        ++value.resolves;
        if (value.resolves == 1 && context->state->pc == entry_pc)
            return reinterpret_cast<std::uintptr_t>(value.code->entry);
        value.cold_published = value.expect_cold && value.resolves == 2 &&
                               context->state->pc == entry_pc &&
                               context->state->packed_flags == value.expected_flags;
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
        return true;  // The real fault owner retries this protected store without consuming TEST
                      // state.
    };
    const auto test_flags = [](u64 value) {
        return (initial & ~dynamic) | (value & 255) | (u64{value == 0} << 30) |
               ((value >> 63) << 31);
    };
    {
        BindFaultCallbacks(scene->faults);
        execution::FaultScope scope(scene->faults, scene->state);
        const auto run =
                [&](u64 input, u64 fault_pc, u64 flags, bool cold_stop = false, bool smc = false) {
                    scene->state = {};
                    scene->state.pc = entry_pc;
                    scene->state.packed_flags = initial;
                    scene->state.gprs[swift::runtime::x86::R10] = input;
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
                        const auto returned =
                                execution::SvmRunNative(&scene->state, &scene->native);
                        Check(!fault_pc && returned == (scene->expect_cold ? 2u : 0u),
                              "Block TEST reaches its actual zero/cold exit");
                    } else
                        Check(fault_pc && scene->faults.signal_number == SIGSEGV,
                              "Block TEST fault uses the actual signal owner");
                    Check(scene->state.pc == (fault_pc             ? fault_pc
                                              : scene->expect_cold ? entry_pc
                                                                   : exit_pc) &&
                                  scene->state.packed_flags == flags &&
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
        Check(scene->state.gprs[swift::runtime::x86::R10] == loaded_before_store_fault &&
                      scene->state.gprs[swift::runtime::x86::R12] == readonly,
              "Block tagged store fault reads R12 after the load has already replaced R10");
        Write64(memory, 0);
        run(head, 0, test_flags(0));
        Check(scene->state.gprs[swift::runtime::x86::R10] == 0 &&
                      scene->state.gprs[swift::runtime::x86::R11] == head,
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
                      scene->state.gprs[swift::runtime::x86::R10] == loaded_before_store_fault &&
                      scene->state.gprs[swift::runtime::x86::R12] == readonly,
              "protected tagged store retries, retires the ready self edge and stops in the real "
              "cold resolver");
    }
    Check(munmap(memory, page * 3) == 0, "Block TEST boundary release");
#endif
}

void LocalFlagsFaults() {
#if defined(__aarch64__) && defined(__ANDROID__)
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
    options.private_uniforms = options.native_nzcv = options.fault_locations = options.local_flags =
            true;
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
            Type::U64, {Operand::ValueOf(incoming), Operand::Immediate(1, Type::U64)}, metadata));
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
    const auto extended =
            Need(builder.Emit<OpCode::SignExtend>(Type::U32, {Operand::ValueOf(half, Type::U16)}));
    Store(builder, field(swift::runtime::x86::RDX), extended);
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void,
                                             {Operand::Immediate(third_pc, Type::U64)}));
    const auto word = Need(builder.Emit<OpCode::LoadMemoryTSO>(
            Type::U32, {Operand::ValueOf(Load(builder, field(swift::runtime::x86::R11)))}));
    const auto extended_word =
            Need(builder.Emit<OpCode::SignExtend>(Type::U64, {Operand::ValueOf(word, Type::U32)}));
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
    execution::CodeHeap arena{TestBackendOps(), static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    const auto code = arena.Publish(compiled.words, compiled.faults, compiled.links);
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<std::uint8_t*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(memory != MAP_FAILED, "local flags fault scene mapping");
    Check(mprotect(memory + page, page, PROT_NONE) == 0, "local flags fault scene permissions");
    const auto valid = reinterpret_cast<std::uintptr_t>(memory), missing = valid + page;
    Write64(memory, 0x8001);
    Write64(memory + 15, 0x8001);
    Write64(memory + 32, 0x80010001);
    Write64(memory + 47, 0x80010001);

    struct Scene {
        swift::runtime::x86::GuestState state;
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
    scene->native.resolve = [](void* opaque,
                               execution::NativeRunContext* context) noexcept -> std::uintptr_t {
        auto& value = *static_cast<Scene*>(opaque);
        ++value.resolves;
        return value.resolves == 1 && context->state->pc == entry_pc
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
        execution::FaultScope scope(scene->faults, scene->state);
        const auto run = [&](u64 first_address,
                             u64 second_address,
                             u64 third_address,
                             u64 fault_pc,
                             u64 expected_rax) {
            scene->state = {};
            scene->state.pc = entry_pc;
            scene->state.packed_flags = initial;
            scene->state.gprs[swift::runtime::x86::RAX] = ~u64{};
            scene->state.gprs[swift::runtime::x86::RDX] = 0x1234567887654321ULL;
            scene->state.gprs[swift::runtime::x86::RCX] = 0xabcdef0011223344ULL;
            scene->state.gprs[swift::runtime::x86::R8] = first_address;
            scene->state.gprs[swift::runtime::x86::R9] = second_address;
            scene->state.gprs[swift::runtime::x86::R11] = third_address;
            scene->resolves = 0;
            scene->faults.signal_number = 0;
            if (sigsetjmp(scene->faults.jump, 1) == 0) {
                const auto returned = execution::SvmRunNative(&scene->state, &scene->native);
                Check(!fault_pc && returned == 42,
                      "local flags complete before the real Return42 exit");
            } else
                Check(fault_pc && scene->faults.signal_number == SIGSEGV,
                      "local PF/carry faults use the registered signal owner");
            Check(scene->resolves == 1 && scene->state.pc == (fault_pc ? fault_pc : entry_pc) &&
                          scene->state.gprs[swift::runtime::x86::RAX] == expected_rax &&
                          scene->state.packed_flags == expected_flags &&
                          scene->state.gprs[swift::runtime::x86::RDX] ==
                                  ((fault_pc == first_pc || fault_pc == second_pc)
                                           ? 0x1234567887654321ULL
                                           : 0xffff8001ULL) &&
                          scene->state.gprs[swift::runtime::x86::RCX] ==
                                  (fault_pc ? 0xabcdef0011223344ULL : 0xffffffff80010001ULL),
                  "local flags fault/exit preserves exact PC, current RAX, whole packed flags and "
                  "reserved bits");
        };
        run(missing, valid, valid + 32, first_pc, 0);
        run(valid, missing, valid + 32, second_pc, 0x55);
        run(valid, valid, valid + 32, 0, 0x55);
        if (options.rcpc_imm9) {
            run(valid, valid + 15, valid + 47, 0, 0x55);
            Check(scene->faults.unaligned_patches == 2,
                  "signed ordered loads patch only their real fast instructions to LDURSH/LDURSW "
                  "plus DMB");
            run(valid, missing, valid + 32, second_pc, 0x55);
            Check(scene->faults.host_pc ==
                          reinterpret_cast<std::uintptr_t>(code.entry) +
                                  WordOffset(compiled.faults[1].fallback_offset) * 4,
                  "the signed cold load retains precise flags, old destination pin and original "
                  "guest PC on SIGSEGV");
            run(valid, valid, missing, third_pc, 0x55);
            Check(scene->faults.host_pc ==
                          reinterpret_cast<std::uintptr_t>(code.entry) +
                                  WordOffset(compiled.faults[2].fallback_offset) * 4,
                  "the cold signed word fault preserves its old full pin after the earlier signed "
                  "halfword committed");
        }
    }
    Check(munmap(memory, page * 2) == 0, "local flags fault scene release");
#endif
}

void VectorPinBoundaries() {
#if defined(__aarch64__) && defined(__ANDROID__)
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
    options.private_uniforms = options.native_nzcv = options.fault_locations = options.deferred_pc =
            true;
    options.entry_pc = entry_pc;
    options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCV);
    options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);

    Builder builder{Scope::Function};
    const auto first = Create(builder), second = Create(builder);
    const auto vector_constant = [&](Vector bits) {
        return Need(builder.Emit<OpCode::VecLoadConst>(
                Type::V128,
                {Operand::Immediate(bits[0], Type::U64), Operand::Immediate(bits[1], Type::U64)}));
    };
    const auto store_vector = [&](std::uint32_t offset, Value value) {
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value)}));
    };
    Need(builder.BeginBlock(first));
    const auto old = Need(
            builder.Emit<OpCode::LoadUniform>(Type::V128, {Operand::Uniform(xmm(2), Type::V128)}));
    Need(builder.EndBlock(Terminal::Jump(second)));
    Need(builder.BeginBlock(second));
    Store(builder, xmm(2) + 8, Constant(builder, partial));
    store_vector(xmm(4), old);
    const auto original3 = Need(
            builder.Emit<OpCode::LoadUniform>(Type::V128, {Operand::Uniform(xmm(3), Type::V128)}));
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
    execution::CodeHeap arena{TestBackendOps(), static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    const auto code = arena.Publish(compiled.words, compiled.faults, compiled.links);
    const auto target_entry =
            arena.Publish(target_code.words, target_code.faults, target_code.links);
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<std::uint8_t*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(memory != MAP_FAILED && mprotect(memory + page, page, PROT_NONE) == 0,
          "vector boundary fault scene mapping");

    struct Scene {
        swift::runtime::x86::GuestState state;
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
    scene->native.resolve = [](void* opaque,
                               execution::NativeRunContext* context) noexcept -> std::uintptr_t {
        auto& value = *static_cast<Scene*>(opaque);
        ++value.resolves;
        if (value.resolves == 1 && context->state->pc == entry_pc) {
            value.root_published =
                    std::memcmp(value.state.xmm, value.initial.data(), sizeof(value.state.xmm)) ==
                    0;
            value.anchor = value.source;
            return reinterpret_cast<std::uintptr_t>(value.source.entry);
        }
        if (value.resolves != 2 || context->state->pc != next_pc) return 0;
        value.cold_published =
                std::memcmp(value.state.xmm, value.expected.data(), sizeof(value.state.xmm)) == 0;
        std::memcpy(value.state.xmm[0], value.replacement.data(), sizeof(Vector));
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
        execution::FaultScope scope(scene->faults, scene->state);
        for (unsigned mode = 0; mode < 3; ++mode) {
            scene->state = {};
            scene->state.pc = entry_pc;
            scene->state.packed_flags = flags;
            std::memcpy(scene->state.xmm, initial.data(), sizeof(scene->state.xmm));
            scene->state.gprs[swift::runtime::x86::RAX] = next_pc;
            scene->state.gprs[swift::runtime::x86::RDI] =
                    reinterpret_cast<std::uintptr_t>(scene->observed.data());
            scene->state.gprs[swift::runtime::x86::R8] =
                    reinterpret_cast<std::uintptr_t>(memory + (mode ? 0 : page));
            scene->expected = expected;
            scene->observed = {};
            scene->resolves = 0;
            scene->fail_next = mode == 2;
            scene->root_published = scene->cold_published = false;
            scene->faults.signal_number = 0;
            if (sigsetjmp(scene->faults.jump, 1) == 0) {
                const auto returned = execution::SvmRunNative(&scene->state, &scene->native);
                Check(mode && returned == (mode == 2 ? 2u : 42u),
                      "vector pins survive cold success and failure Return reasons");
            } else
                Check(!mode && scene->faults.signal_number == SIGSEGV,
                      "vector pins recover through the actual fatal owner");
            Check(scene->root_published && scene->resolves == (mode ? 2u : 1u) &&
                          (!mode || scene->cold_published) &&
                          scene->state.pc == (mode ? next_pc : fault_pc) &&
                          scene->state.packed_flags == flags,
                  "root/cold/fatal boundaries publish the real PC and preserve whole packed flags");
            Check(std::memcmp(scene->state.xmm, scene->expected.data(), sizeof(scene->state.xmm)) ==
                          0,
                  "all XMM bits survive old SSA, partial/cross-slot aliases, helper clobbers and "
                  "cold reload");
            Check(scene->observed == live_bits,
                  "helper preserves low and high halves of all live dynamic vector homes");
        }
    }
    Check(munmap(memory, page * 2) == 0, "vector boundary scene release");
#endif
}

void Unsupported() {
    for (const bool flags : {false, true}) {
        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        const auto left = Constant(builder, 9);
        OperationMetadata metadata;
        if (flags) metadata.flag_outputs = 1;
        const auto value =
                Need(builder.Append(flags ? OpCode::Add : OpCode::Div,
                                    Type::U64,
                                    {Operand::ValueOf(left), Operand::Immediate(3, Type::U64)},
                                    metadata));
        Need(builder.EndBlock(Terminal::Return(value)));
        auto program = Submit(builder);
        const auto code = arm64::Compile(program);
        Check(!code && code.words.empty() && code.node == value.node &&
                      code.error == (flags ? arm64::CompileError::UnsupportedFlags
                                           : arm64::CompileError::UnsupportedOpcode),
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

int main() {
    try {
        ConstantsAndViews();
        StateAndMemory();
        Spills();
        CrossingLoops();
        EmptyHeaderCollision();
        ReorderedBodies();
        EntryAndTerminals();
        Unsupported();
        ArithmeticFlags();
        FlagsQueriesAndAliases();
        FlagsWithSpilledResult();
        LegacyFlags();
        ExplicitEntry();
        PureIntegers();
        X87StatusResultUses();
        PureIntegerSpills();
        InvalidIntegerShapes();
        DirectTerminalCondition();
        DistantForwardEmission();
        TerminalReadBoundaries();
        PCTailPredicates();
        DeferredPCLinks();
        CompleteAdditionalEntries();
        LinkedDynamicPC();
        FaultMetadataAndRecovery();
        FaultLocationBoundaries();
        ScalarAtomicSequence();
        CpuQueryPairLifetime();
        VectorLaneMasks();
        VectorSignedWidenMultiply();
        PackedFloatingArithmetic();
        NarrowLogicalImmediates();
        LinkedPinnedUniforms();
        LinkedExitAndCrossSlot();
        LinkedRawAuxiliaryCarry();
        FunctionDeferredTestFaults();
        BlockDeferredTestBoundaries();
        LocalFlagsFaults();
        VectorPinBoundaries();
#if defined(__aarch64__) && defined(__ANDROID__)
        constexpr bool executed = true;
#else
        constexpr bool executed = false;
#endif
        std::printf(
                "arm64 scalar contracts: checks=%zu native_execution=%s; subset only, no "
                "full-VM/FEX qualification\n",
                checks,
                executed ? "true" : "false");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "arm64 scalar contract failed: %s\n", error.what());
        return 1;
    }
}
