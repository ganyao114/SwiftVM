#include "runtime/arch/arm64/numeric.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/backend/x64/allocate.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/helpers.h"
#include "runtime/backend/x64/patch.h"
#include "runtime/backend/x64/recovery.h"
#include "runtime/backend/x64/relocate.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#endif
#include "runtime/externals/vixl/aarch64/constants-aarch64.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "runtime/execution/fault.h"
#include "runtime/exit.h"
#include "runtime/ir/builder.h"
#include "tests/support/check.h"
#include "tests/support/guest_memory.h"
#include "tests/support/ir_builder.h"
#include "tests/support/native_code.h"
#include "tests/support/test_mode.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__x86_64__) && defined(__linux__)
#include <asm/unistd.h>
#include <cpuid.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
using swift::tests::Check;
using swift::tests::NativeMode;
using swift::tests::ir_builder::Block;
using swift::tests::ir_builder::Need;
using swift::tests::ir_builder::Submit;
using namespace swift::runtime::ir;
namespace x64 = swift::runtime::backend::x64;
namespace flags = swift::runtime::x86::flags;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

struct Pair {
    u64 low{}, high{};
    friend bool operator==(Pair, Pair) = default;
};

struct alignas(16) State {
    std::array<u64, 32> scalars{};
    std::array<Pair, 16> vectors{};
    u64 flags{}, pc{};
    u32 mxcsr{0x1f80};
    u32 fpcr{}, fpsr{};
};

const char* current_case = "setup";

Value Emit(Builder& builder,
           OpCode opcode,
           Type type,
           std::initializer_list<Operand> arguments,
           OperationMetadata metadata = {}) {
    return Need(builder.Append(opcode, type, arguments, metadata));
}

void Begin(Builder& builder) { Need(builder.BeginBlock(Block(builder))); }

Program Return(Builder& builder, Value value = {}) {
    Need(builder.EndBlock(value ? Terminal::Return(value) : Terminal::Return()));
    return Submit(builder);
}

Operand Imm(u64 value, Type type = Type::U64) { return Operand::Immediate(value, type); }

Value Literal(Builder& builder, u64 value, Type type = Type::U64) {
    return Emit(builder, OpCode::LoadImm, type, {Imm(value, type)});
}

Value Vector(Builder& builder, Pair value) {
    return Emit(builder, OpCode::VecLoadConst, Type::V128, {Imm(value.low), Imm(value.high)});
}

Value Load(Builder& builder, unsigned slot) {
    return Emit(builder,
                OpCode::LoadUniform,
                Type::U64,
                {Operand::Uniform(offsetof(State, scalars) + slot * 8, Type::U64)});
}

void Store(Builder& builder, unsigned slot, Value value) {
    Emit(builder,
         OpCode::StoreUniform,
         Type::Void,
         {Operand::Uniform(offsetof(State, scalars) + slot * 8, Type::U64),
          Operand::ValueOf(value)});
}

Value LoadVector(Builder& builder, unsigned slot) {
    return Emit(builder,
                OpCode::LoadUniform,
                Type::V128,
                {Operand::Uniform(offsetof(State, vectors) + slot * 16, Type::V128)});
}

void StoreVector(Builder& builder, unsigned slot, Value value) {
    Emit(builder,
         OpCode::StoreUniform,
         Type::Void,
         {Operand::Uniform(offsetof(State, vectors) + slot * 16, Type::V128),
          Operand::ValueOf(value)});
}

Pair Lanes32(u32 a, u32 b, u32 c, u32 d) {
    return {u64{a} | (u64{b} << 32), u64{c} | (u64{d} << 32)};
}

thread_local bool test_local_flags{}, test_lahf{};

x64::CompileOptions Options() {
    x64::CompileOptions options;
    options.flags_offset = offsetof(State, flags);
    options.pc_offset = offsetof(State, pc);
    options.mxcsr_offset = offsetof(State, mxcsr);
    options.fault_locations = true;
    options.native_fp_environment = true;
    options.local_flags = test_local_flags;
    options.lahf = test_lahf;
    return options;
}

swift::runtime::backend::CompileResult Compile(const Program& program) {
    auto code = x64::Compile(program, Options());
    if (!code)
        std::fprintf(stderr,
                     "%s: compile error=%u node=%u block=%u\n",
                     current_case,
                     unsigned(code.error),
                     code.node.value,
                     code.block.value);
    Check(bool(code) && !code.words.empty(), "X64 compile rejected supported IR");
    Check(code.frame_bytes >= 72 && code.frame_bytes % 16 == 8, "native C ABI frame is aligned");
    for (const auto& fault : code.faults)
        Check(fault.first_offset < fault.last_offset && fault.last_offset <= code.words.size() * 4,
              "fault range belongs to actual emitted words");
    return code;
}

#if defined(__x86_64__) && defined(__linux__)
class NativeBlock {
public:
    explicit NativeBlock(const swift::runtime::backend::CompileResult& source)
            : code(source.words, swift::tests::NativeISA::X64) {}

    u64 Run(State& state) const {
        unsigned previous;
        asm volatile("stmxcsr %0" : "=m"(previous));
        const unsigned host = 0x7fa1;
        asm volatile("ldmxcsr %0" ::"m"(host));
        const auto answer = code.Run(reinterpret_cast<std::uint8_t*>(&state));
        unsigned after;
        asm volatile("stmxcsr %0" : "=m"(after));
        asm volatile("ldmxcsr %0" ::"m"(previous));
        Check(after == host, "native return restores complete caller MXCSR");
        return answer;
    }

private:
    swift::tests::NativeCode code;
};
#endif

bool Run(const swift::runtime::backend::CompileResult& code, State& state, u64 expected = 0) {
#if defined(__x86_64__) && defined(__linux__)
    if (NativeMode()) {
        NativeBlock native{code};
        const auto answer = native.Run(state);
        Check(answer == expected, "native return value");
        return true;
    }
#else
    (void)code;
    (void)state;
    (void)expected;
#endif
    return false;  // Compile-only has no native numerical result.
}

void ArithmeticFlags() {
    constexpr u64 reserved = (u64{1} << 55) | flags::Direction;
    for (const auto profile : {FlagProfile::ArithmeticRaw, FlagProfile::ArithmeticCanonicalCarry}) {
        current_case = profile == FlagProfile::ArithmeticRaw ? "raw subtraction flags"
                                                             : "canonical subtraction flags";
        Builder builder{Scope::Block};
        Begin(builder);
        const auto zero = Literal(builder, 0, Type::U8);
        const auto one = Literal(builder, 1, Type::U8);
        const auto difference = Emit(builder,
                                     OpCode::Sub,
                                     Type::U8,
                                     {Operand::ValueOf(zero), Operand::ValueOf(one)},
                                     {FlagBits(Flag::All), profile});
        const auto snapshot = Emit(builder,
                                   OpCode::GetFlags,
                                   Type::U64,
                                   {Operand::ValueOf(difference), Operand::Flags(Flag::All)});
        Store(builder, 0, snapshot);
        auto program = Return(builder, difference);
        auto code = Compile(program);
        State state;
        state.flags = reserved;
        if (Run(code, state, 255)) {
            const auto expected =
                    reserved | flags::Sign | flags::Auxiliary | 255 |
                    (profile == FlagProfile::ArithmeticCanonicalCarry ? flags::Carry : 0);
            Check(state.flags == expected && state.scalars[0] == expected,
                  "borrow polarity, AF/PF and reserved flags");
        }
    }
    current_case = "producer snapshot and ADC";
    Builder builder{Scope::Function};
    Begin(builder);
    const auto ff = Literal(builder, 255, Type::U8), one = Literal(builder, 1, Type::U8);
    const OperationMetadata full{FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry};
    // A later named snapshot needs all producer flags even when its immediate
    // architectural write selected only ZF.
    const auto sum = Emit(builder,
                          OpCode::Add,
                          Type::U8,
                          {Operand::ValueOf(ff), Operand::ValueOf(one)},
                          {FlagBits(Flag::Zero), FlagProfile::ArithmeticCanonicalCarry});
    const auto zero = Literal(builder, 0, Type::U8);
    Emit(builder, OpCode::Sub, Type::U8, {Operand::ValueOf(zero), Operand::ValueOf(one)}, full);
    Emit(builder,
         OpCode::SaveFlags,
         Type::Void,
         {Operand::ValueOf(sum), Operand::Flags(Flag::All)});
    const auto snapshot = Emit(builder,
                               OpCode::GetFlags,
                               Type::U64,
                               {Operand::ValueOf(sum), Operand::Flags(Flag::All)});
    Store(builder, 0, snapshot);
    const auto ten = Literal(builder, 10), twenty = Literal(builder, 20);
    const auto answer = Emit(
            builder, OpCode::Adc, Type::U64, {Operand::ValueOf(ten), Operand::ValueOf(twenty)});
    auto program = Return(builder, answer);
    auto code = Compile(program);
    State state;
    state.flags = reserved;
    if (Run(code, state, 31))
        Check(state.scalars[0] == (reserved | flags::Carry | flags::Zero | flags::Auxiliary),
              "SaveFlags consumes original producer snapshot after a later clobber");
}

void ArithmeticFlagBoundaries() {
    constexpr u64 reserved = (u64{1} << 55) | flags::Direction;
    constexpr std::pair<u64, u64> selections[]{
            {FlagBits(Flag::All),
             flags::Carry | flags::Overflow | flags::Zero | flags::Sign | flags::Auxiliary | 255},
            {FlagBits(Flag::NZCV), flags::Carry | flags::Overflow | flags::Zero | flags::Sign},
            {FlagBits(Flag::Carry), flags::Carry},
            {FlagBits(Flag::Overflow), flags::Overflow},
            {FlagBits(Flag::Zero), flags::Zero},
            {FlagBits(Flag::Negate), flags::Sign},
            {FlagBits(Flag::Parity), 255},
            {FlagBits(Flag::AuxiliaryCarry), flags::Auxiliary},
            {FlagBits(Flag::Carry | Flag::Zero), flags::Carry | flags::Zero},
            {0, 0},
    };
    for (const auto type : {Type::U8, Type::U16, Type::U32, Type::U64}) {
        const unsigned bits = TypeBits(type);
        const u64 mask = UINT64_MAX >> (64 - bits), sign = u64{1} << (bits - 1);
        const u64 values[]{0, 1, 15, 16, mask, sign - 1, sign, mask / 3, mask / 3 * 2};
        for (const auto opcode : {OpCode::Add, OpCode::Sub, OpCode::Adc, OpCode::Sbb}) {
            const bool subtract = opcode == OpCode::Sub || opcode == OpCode::Sbb;
            const bool with_carry = opcode == OpCode::Adc || opcode == OpCode::Sbb;
            for (const auto profile :
                 {FlagProfile::ArithmeticRaw, FlagProfile::ArithmeticCanonicalCarry}) {
                for (const auto [selected, packed_mask] : selections) {
                    Builder builder{Scope::Block};
                    Begin(builder);
                    const auto lhs = Load(builder, 0), rhs = Load(builder, 1);
                    const auto result =
                            Emit(builder,
                                 opcode,
                                 type,
                                 {Operand::ValueOf(lhs, type), Operand::ValueOf(rhs, type)},
                                 {selected, profile});
                    auto program = Return(builder, result);
                    const auto code = Compile(program);
                    for (const auto a : values) {
                        for (const auto b : values) {
                            for (unsigned carry = 0; carry < 2; ++carry) {
                                const u64 incoming = with_carry ? carry : 0;
                                const u64 answer =
                                        (subtract ? a - b - incoming : a + b + incoming) & mask;
                                const bool cf = subtract ? a < b || (incoming && a == b)
                                                         : answer < a || (incoming && answer == a);
                                const bool of =
                                        ((subtract ? a ^ b : ~(a ^ b)) & (a ^ answer) & sign) != 0;
                                const bool packed_carry =
                                        cf ^ (subtract && profile == FlagProfile::ArithmeticRaw);
                                const u64 expected =
                                        reserved | (answer & 255) |
                                        (packed_carry ? flags::Carry : 0) |
                                        (of ? flags::Overflow : 0) | (!answer ? flags::Zero : 0) |
                                        ((answer & sign) ? flags::Sign : 0) |
                                        (((a ^ b ^ answer) & 16) ? flags::Auxiliary : 0);
                                State state;
                                state.scalars[0] = a;
                                state.scalars[1] = b;
                                const u64 initial = reserved | flags::Overflow | flags::Zero |
                                                    flags::Sign | flags::Auxiliary | 0xa5 |
                                                    (carry ? flags::Carry : 0);
                                state.flags = initial;
                                if (Run(code, state, answer))
                                    CHECK(state.flags ==
                                          ((initial & ~packed_mask) | (expected & packed_mask)));
                            }
                        }
                    }
                }
            }
        }
    }
}

void ImmediateOperandBoundaries() {
    constexpr u64 reserved = (u64{1} << 55) | flags::Direction;
    constexpr u64 constants[]{0,
                              1,
                              127,
                              128,
                              255,
                              0x7fffffff,
                              0x80000000,
                              0xffffffff,
                              0x100000000,
                              UINT64_C(0xffffffff80000000),
                              UINT64_MAX};
    for (const auto scope : {Scope::Block, Scope::Function})
        for (const auto type : {Type::U32, Type::U64}) {
            const unsigned bits = TypeBits(type);
            const u64 mask = UINT64_MAX >> (64 - bits), sign = u64{1} << (bits - 1);
            for (const auto opcode :
                 {OpCode::Add, OpCode::Sub, OpCode::And, OpCode::Or, OpCode::Xor})
                for (const auto view : {Type::U8, Type::U32, Type::U64})
                    for (const auto constant : constants)
                        for (const bool literal : {false, true}) {
                            Builder builder{scope};
                            Begin(builder);
                            const auto left = Load(builder, 0), right = Load(builder, 1);
                            const bool subtract = opcode == OpCode::Sub;
                            const bool arithmetic = opcode == OpCode::Add || subtract;
                            const auto result = Emit(
                                    builder,
                                    opcode,
                                    type,
                                    {Operand::ValueOf(left, type),
                                     literal ? Imm(constant & (UINT64_MAX >> (64 - TypeBits(view))),
                                                   view)
                                             : Operand::ValueOf(right, view)},
                                    {FlagBits(Flag::All),
                                     arithmetic ? FlagProfile::ArithmeticCanonicalCarry
                                                : FlagProfile::LogicalZeroCVA});
                            const auto program = Return(builder, result);
                            const auto code = Compile(program);
                            const u64 b = constant & (UINT64_MAX >> (64 - TypeBits(view))) & mask;
                            for (const u64 a : {u64{0}, u64{1}, mask, sign, sign - 1, mask / 3}) {
                                u64 answer;
                                switch (opcode) {
                                    case OpCode::Add:
                                        answer = (a + b) & mask;
                                        break;
                                    case OpCode::Sub:
                                        answer = (a - b) & mask;
                                        break;
                                    case OpCode::And:
                                        answer = a & b;
                                        break;
                                    case OpCode::Or:
                                        answer = a | b;
                                        break;
                                    default:
                                        answer = a ^ b;
                                        break;
                                }
                                const bool cf = arithmetic && (subtract ? a < b : answer < a);
                                const bool of = arithmetic && (((subtract ? a ^ b : ~(a ^ b)) &
                                                                (a ^ answer) & sign) != 0);
                                const u64 expected =
                                        reserved | (answer & 255) | (cf ? flags::Carry : 0) |
                                        (of ? flags::Overflow : 0) | (!answer ? flags::Zero : 0) |
                                        ((answer & sign) ? flags::Sign : 0) |
                                        (arithmetic && ((a ^ b ^ answer) & 16) ? flags::Auxiliary
                                                                               : 0);
                                State state;
                                state.scalars[0] = a | (~mask & UINT64_C(0x95f0abcdef012345));
                                state.scalars[1] = constant;
                                state.flags = reserved | flags::Arithmetic;
                                if (Run(code, state, answer)) CHECK(state.flags == expected);
                            }
                        }
        }
}

u64 NarrowHelper(u64 first, u64 second) {
#if defined(__x86_64__) && defined(__linux__)
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d" ::: "r8", "r9");
#endif
    return (first << 32) | second;
}

void NarrowReadsAndHelperABI() {
    for (const auto scope : {Scope::Block, Scope::Function}) {
        Builder builder{scope};
        Begin(builder);
        std::array<Value, 10> live;
        for (unsigned i = 0; i < live.size(); ++i) live[i] = Load(builder, i);
        const auto vector = LoadVector(builder, 0);
        std::array<Value, 16> vectors;
        for (auto& value : vectors) value = LoadVector(builder, 0);
        const auto call = Emit(builder,
                               OpCode::CallLambda,
                               Type::U64,
                               {Operand::Lambda(),
                                Imm(reinterpret_cast<std::uintptr_t>(&NarrowHelper)),
                                Operand::ValueOf(live[4], Type::U32),
                                Operand::ValueOf(live[5], Type::U32)});
        for (const unsigned i : {4u, 5u, 0u, 1u, 2u, 3u, 6u, 7u, 8u, 9u}) {
            const auto low = Emit(builder,
                                  OpCode::ZeroExtend64,
                                  Type::U64,
                                  {Operand::ValueOf(live[i], Type::U32)});
            Store(builder, 10 + i, low);
            const auto raw = Emit(
                    builder, OpCode::BitCast, Type::U32, {Operand::ValueOf(live[i], Type::U32)});
            const auto widened =
                    Emit(builder, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(raw)});
            Store(builder, i, widened);
        }
        const auto vector_low = Emit(builder,
                                     OpCode::GetResult,
                                     Type::U32,
                                     {Operand::ValueOf(vector), Operand::ValueOf(vector)});
        Store(builder, 20, vector_low);
        for (unsigned i = 0; i < 2; ++i) {
            const auto low =
                    Emit(builder,
                         OpCode::ZeroExtend64,
                         Type::U64,
                         {Operand::ValueOf(i ? vectors.back() : vectors.front(), Type::U32)});
            Store(builder, 21 + i, low);
        }
        for (unsigned i = 0; i < vectors.size(); ++i) StoreVector(builder, i, vectors[i]);
        Store(builder, 24, call);
        const auto program = Return(builder);
        const auto allocation = scope == Scope::Block ? x64::AllocateBlock(program)
                                                      : x64::AllocateFunction(program);
        bool saved_r8{}, saved_r9{}, scalar_spill{}, vector_spill{};
        for (const auto value : live) {
            const auto& home = allocation.homes[value.node.value];
            scalar_spill |= home.spilled;
            saved_r8 |= !home.spilled && home.reg == 8;
            saved_r9 |= !home.spilled && home.reg == 9;
        }
        for (const auto value : vectors) vector_spill |= allocation.homes[value.node.value].spilled;
        INFO("scalar spill=" << scalar_spill << " vector spill=" << vector_spill
                             << " saved r8=" << saved_r8 << " saved r9=" << saved_r9);
        CHECK(scalar_spill && vector_spill && saved_r8 && saved_r9);
        const auto code = Compile(program);
        State state;
        for (unsigned i = 0; i < live.size(); ++i)
            state.scalars[i] = UINT64_C(0xfedcba9800000000) | (0x80000010 + i);
        for (auto& value : state.vectors)
            value = {UINT64_C(0xabcdef0123456789), UINT64_C(0xfedcba9876543210)};
        state.flags = (u64{1} << 55) | flags::Direction | flags::Carry | 0xa5;
        const auto previous_flags = state.flags;
        if (Run(code, state)) {
            for (unsigned i = 0; i < live.size(); ++i)
                CHECK(state.scalars[i] == 0x80000010 + i &&
                      state.scalars[10 + i] == 0x80000010 + i);
            CHECK(state.scalars[24] == UINT64_C(0x8000001480000015));
            CHECK(state.scalars[20] == 0x23456789);
            CHECK(state.scalars[21] == 0x23456789 && state.scalars[22] == 0x23456789);
            for (const auto value : state.vectors)
                CHECK(value == Pair{UINT64_C(0xabcdef0123456789), UINT64_C(0xfedcba9876543210)});
            CHECK(state.flags == previous_flags);
        }
    }
}

void UniformStoreViews() {
    for (const auto scope : {Scope::Block, Scope::Function})
        for (const auto view : {Type::Bool, Type::U8, Type::U16, Type::U32, Type::U64})
            for (const auto width : {Type::Bool, Type::U8, Type::U16, Type::U32, Type::U64}) {
                Builder builder{scope};
                Begin(builder);
                const auto input = Load(builder, 0);
                Emit(builder,
                     OpCode::StoreUniform,
                     Type::Void,
                     {Operand::Uniform(offsetof(State, scalars) + 8, width),
                      Operand::ValueOf(input, view)});
                const auto program = Return(builder, input);
                const auto code = Compile(program);
                for (const u64 value :
                     {u64{0}, u64{1}, u64{2}, UINT64_C(0x95f0abcdef012345), UINT64_MAX}) {
                    State state;
                    state.scalars[0] = value;
                    state.scalars[1] = UINT64_C(0xfedcba9876543210);
                    const u64 read_mask = UINT64_MAX >> (64 - TypeBits(view));
                    const unsigned store_bits = std::max(8u, TypeBits(width));
                    const u64 store_mask = UINT64_MAX >> (64 - store_bits);
                    const u64 expected =
                            (state.scalars[1] & ~store_mask) | (value & read_mask & store_mask);
                    if (Run(code, state, value))
                        CHECK(state.scalars[1] == expected && state.scalars[0] == value);
                }
            }
    for (const auto opcode : {OpCode::Add, OpCode::Sub}) {
        Builder builder{Scope::Block};
        Begin(builder);
        const auto input = Load(builder, 0), other = Load(builder, 1);
        const auto result = Emit(builder,
                                 opcode,
                                 Type::Bool,
                                 {Operand::ValueOf(input, Type::Bool), Operand::ValueOf(other)});
        const auto program = Return(builder, result);
        const auto code = Compile(program);
        for (const u64 left : {u64{0}, u64{1}})
            for (const u64 right : {u64{0}, u64{1}, u64{2}, UINT64_MAX}) {
                State state;
                state.scalars[0] = left;
                state.scalars[1] = right;
                state.flags = (u64{1} << 55) | flags::Arithmetic;
                const auto previous = state.flags;
                if (Run(code, state, (left ^ right) & 1)) CHECK(state.flags == previous);
            }
    }
}

void ImmediateShiftBoundaries() {
    constexpr std::pair<OpCode, OpCode> shifts[]{
            {OpCode::LslImm, OpCode::LslValue},
            {OpCode::LsrImm, OpCode::LsrValue},
            {OpCode::AsrImm, OpCode::AsrValue},
            {OpCode::RorImm, OpCode::RorValue},
    };
    for (const auto scope : {Scope::Block, Scope::Function})
        for (const auto type : {Type::U8, Type::U16, Type::U32, Type::U64}) {
            const unsigned bits = TypeBits(type);
            const u64 mask = UINT64_MAX >> (64 - bits), sign = u64{1} << (bits - 1);
            for (const auto [immediate, variable] : shifts)
                for (const u64 count : {0u, 1u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 65u, 255u})
                    for (const bool literal : {false, true}) {
                        Builder builder{scope};
                        Begin(builder);
                        const auto value = Load(builder, 0), amount = Load(builder, 1);
                        const auto result = Emit(builder,
                                                 literal ? immediate : variable,
                                                 type,
                                                 {Operand::ValueOf(value, type),
                                                  literal ? Operand::Immediate(count, Type::U64)
                                                          : Operand::ValueOf(amount)});
                        const auto program = Return(builder, result);
                        const auto code = Compile(program);
                        for (const u64 input :
                             {u64{0}, u64{1}, mask, sign, sign - 1, mask / 3, mask / 3 * 2}) {
                            const unsigned n = count & (bits - 1);
                            u64 expected = input;
                            if (n) {
                                if (immediate == OpCode::LslImm)
                                    expected = (input << n) & mask;
                                else if (immediate == OpCode::RorImm)
                                    expected = ((input >> n) | (input << (bits - n))) & mask;
                                else {
                                    expected = input >> n;
                                    if (immediate == OpCode::AsrImm && (input & sign))
                                        expected |= mask ^ (mask >> n);
                                }
                            }
                            State state;
                            state.scalars[0] = input | (~mask & UINT64_C(0x95f0abcdef012345));
                            state.scalars[1] = count;
                            state.flags = (u64{1} << 55) | flags::Direction | flags::Carry | 0xa5;
                            const auto before = state.flags;
                            if (Run(code, state, expected)) CHECK(state.flags == before);
                        }
                    }
        }
}

void ArmFPBinaryBoundaries() {
    namespace arm = swift::runtime::arm64;
    current_case = "exact ARM binary FP fast and slow paths";
    for (auto scope : {Scope::Block, Scope::Function}) {
        for (unsigned bits : {32u, 64u}) {
            const unsigned bytes = bits / 8;
            const u64 sign = u64{1} << (bits - 1);
            const u64 one = bits == 32 ? 0x3f800000ULL : 0x3ff0000000000000ULL;
            const u64 minimum = bits == 32 ? 0x00800000ULL : 0x0010000000000000ULL;
            const u64 infinity = bits == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
            const u64 quiet = bits == 32 ? 0x00400000ULL : 0x0008000000000000ULL;
            const u64 low = u64{bits == 32 ? 65u : 513u} << (bits == 32 ? 23 : 52);
            const u64 high = u64{bits == 32 ? 190u : 1534u} << (bits == 32 ? 23 : 52);
            const std::array<std::array<u64, 2>, 18> inputs{
                    {{one, one},
                     {one + 1, one + 1},
                     {one, sign | one},
                     {sign, 0},
                     {sign, sign},
                     {one, 1},
                     {minimum, one - 1},
                     {minimum, one / 2},
                     {infinity - 1, one + 1},
                     {infinity, 0},
                     {infinity | 0x42, one},
                     {infinity | quiet | 0x42, infinity | 1},
                     {low, low},
                     {low - 1, low},
                     {high - 1, high - 1},
                     {high, one},
                     {minimum - 1, sign | minimum},
                     {0, 1}}};
            for (unsigned kind = 0; kind < 3; ++kind) {
                for (unsigned lanes : {1u, 2u, 4u}) {
                    if (lanes * bits > 128) continue;
                    for (unsigned indexed = 0; indexed < 2; ++indexed) {
                        const unsigned index = indexed ? 128 / bits - 1 : 0;
                        const auto mode = arm::ArmFPBinaryMode(kind, bits, lanes, indexed, index);
                        Builder builder{scope};
                        Begin(builder);
                        const auto left = LoadVector(builder, 0), right = LoadVector(builder, 1);
                        // Keep caller-saved scalar and all vector homes live over a slow call.
                        std::array<Value, 14> live;
                        for (unsigned i = 0; i < live.size(); ++i) live[i] = LoadVector(builder, i);
                        const auto result = Emit(builder,
                                                 OpCode::ArmFPBinary,
                                                 Type::V128,
                                                 {Operand::ValueOf(left),
                                                  Operand::ValueOf(right),
                                                  Imm(offsetof(State, fpcr)),
                                                  Imm(offsetof(State, fpsr)),
                                                  Imm(mode)});
                        StoreVector(builder, 15, result);
                        auto checksum = live[0];
                        for (unsigned i = 1; i < live.size(); ++i)
                            checksum =
                                    Emit(builder,
                                         OpCode::VecXor,
                                         Type::V128,
                                         {Operand::ValueOf(checksum), Operand::ValueOf(live[i])});
                        StoreVector(builder, 14, checksum);
                        auto program = Return(builder);
                        auto options = Options();
                        options.relocatable = true;
                        auto code = x64::Compile(program, options);
                        Check(bool(code) && code.relocatable,
                              "ARM FP units retain a relocation contract");
                        Check(x64::Relocate({reinterpret_cast<std::uint8_t*>(code.words.data()),
                                             code.words.size() * 4},
                                            code.relocations,
                                            options),
                              "ARM numerical slow path rebinds by stable helper symbol");
#if defined(__x86_64__) && defined(__linux__)
                        if (!NativeMode()) continue;
                        NativeBlock native{code};
                        for (unsigned control = 0; control < 16; ++control) {
                            for (unsigned sample = 0; sample < inputs.size(); ++sample) {
                                State state;
                                state.fpcr = ((control & 3) << 22) |
                                             ((control & 4) ? 1u << 24 : 0) |
                                             ((control & 8) ? 1u << 25 : 0);
                                state.fpsr = 0x08000000 | (sample & 1 ? 0x9f : 0);
                                state.mxcsr = 0x1f80 | (sample & 1 ? 0x3f : 0);
                                for (unsigned i = 0; i < 14; ++i)
                                    state.vectors[i] = {u64{i + 1} * 0x123456789abcdefULL,
                                                        ~u64{i * 17}};
                                auto* lhs = reinterpret_cast<std::uint8_t*>(&state.vectors[0]);
                                auto* rhs = reinterpret_cast<std::uint8_t*>(&state.vectors[1]);
                                for (unsigned lane = 0; lane < 128 / bits; ++lane) {
                                    const auto& pair = inputs[(sample + lane) % inputs.size()];
                                    std::memcpy(lhs + lane * bytes, &pair[0], bytes);
                                    std::memcpy(rhs + lane * bytes, &pair[1], bytes);
                                }
                                Pair expected{}, checksum{};
                                auto exceptions = state.fpsr;
                                constexpr unsigned operations[]{2, 3, 0};
                                for (unsigned lane = 0; lane < lanes; ++lane) {
                                    u64 a{}, b{};
                                    std::memcpy(&a, lhs + lane * bytes, bytes);
                                    std::memcpy(&b, rhs + (indexed ? index : lane) * bytes, bytes);
                                    const auto result = arm::EvaluateBinaryFP(
                                            a, b, bits, operations[kind], state.fpcr);
                                    std::memcpy(reinterpret_cast<std::uint8_t*>(&expected) +
                                                        lane * bytes,
                                                &result.value,
                                                bytes);
                                    exceptions |= result.exceptions;
                                }
                                for (unsigned i = 0; i < 14; ++i) {
                                    checksum.low ^= state.vectors[i].low;
                                    checksum.high ^= state.vectors[i].high;
                                }
                                Check(native.Run(state) == 0, "ARM FP test returns normally");
                                Check(state.vectors[15] == expected && state.fpsr == exceptions,
                                      "ARM FP results, inactive lanes and complete sticky status "
                                      "are exact");
                                Check(state.vectors[14] == checksum,
                                      "slow calls preserve live XMM and spilled inputs");
                            }
                        }
#endif
                    }
                }
            }
        }
    }
}

void ArmFPUnalignedState() {
    current_case = "unaligned explicit ARM FP control and status fields";
    constexpr auto control = offsetof(State, scalars) + 209, status = control + 4;
    Builder builder{Scope::Block};
    Begin(builder);
    const auto left = Vector(builder, {0x3f800000, UINT64_MAX});
    const auto right = Vector(builder, {0x33800000, UINT64_MAX});
    const auto result = Emit(builder,
                             OpCode::ArmFPBinary,
                             Type::V128,
                             {Operand::ValueOf(left),
                              Operand::ValueOf(right),
                              Imm(control),
                              Imm(status),
                              Imm(swift::runtime::arm64::ArmFPBinaryMode(0, 32, 1))});
    StoreVector(builder, 0, result);
    const auto code = Compile(Return(builder));
    for (unsigned upward : {0u, 1u}) {
        State state;
        const u32 fpcr = upward << 22, fpsr = 0x08000004;
        std::memcpy(reinterpret_cast<std::uint8_t*>(&state) + control, &fpcr, 4);
        std::memcpy(reinterpret_cast<std::uint8_t*>(&state) + status, &fpsr, 4);
        if (Run(code, state)) {
            u32 actual;
            std::memcpy(&actual, reinterpret_cast<std::uint8_t*>(&state) + status, 4);
            Check(actual == 0x08000014 && state.vectors[0] == Pair{0x3f800000 + upward, 0},
                  "both native and exact paths support unaligned architectural uniform fields");
        }
    }
}

void MemoryHomeOperands() {
    current_case = "resident and spilled guest memory operands";
    for (bool pressure : {false, true}) {
        Builder builder{Scope::Block};
        Begin(builder);
        const auto address = Load(builder, 0);
        std::array<Value, 14> live;
        for (unsigned i = 0; i < (pressure ? live.size() : 2); ++i) live[i] = Load(builder, i + 1);
        const auto value = pressure ? live.back() : live[1];
        unsigned slot = 16;
        for (auto type : {Type::U8, Type::U16, Type::U32, Type::U64}) {
            Emit(builder,
                 OpCode::StoreMemory,
                 Type::Void,
                 {Operand::ValueOf(address), Operand::ValueOf(value, type)});
            const auto read = Emit(builder, OpCode::LoadMemory, type, {Operand::ValueOf(address)});
            Store(builder, slot++, read);
        }
        const auto vector = LoadVector(builder, 0);
        Emit(builder,
             OpCode::StoreMemory,
             Type::Void,
             {Operand::ValueOf(address), Operand::ValueOf(vector)});
        const auto read =
                Emit(builder, OpCode::LoadMemory, Type::V128, {Operand::ValueOf(address)});
        StoreVector(builder, 1, read);
        auto checksum = live[0];
        for (unsigned i = 1; i < (pressure ? live.size() : 2); ++i)
            checksum = Emit(builder,
                            OpCode::Xor,
                            Type::U64,
                            {Operand::ValueOf(checksum), Operand::ValueOf(live[i])});
        Store(builder, 20, checksum);
        const auto code = Compile(Return(builder));
        std::array<std::uint8_t, 32> memory{};
        State state;
        state.scalars[0] = reinterpret_cast<std::uintptr_t>(memory.data() + 1);
        u64 expected{};
        for (unsigned i = 1; i <= (pressure ? live.size() : 2); ++i) {
            state.scalars[i] = u64{i} * 0x123456789abcdef;
            expected ^= state.scalars[i];
        }
        state.vectors[0] = {0x1122334455667788, 0x99aabbccddeeff00};
        const auto original = state.scalars[pressure ? 14 : 2];
        if (Run(code, state)) {
            for (unsigned i = 0; i < 4; ++i) {
                const auto bits = 8u << i;
                Check(state.scalars[16 + i] ==
                              (original & (bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1)),
                      "narrow memory reads normalize the complete destination and stores use the "
                      "view");
            }
            Check(state.vectors[1] == state.vectors[0] && state.scalars[20] == expected,
                  "unaligned vector memory and live scalar homes survive direct or spilled "
                  "operands");
        }
    }
}

void TerminalFlagConditions() {
    current_case = "terminal packed-flag tests";
    for (unsigned mask : {0u, 1u, 2u, 4u, 8u, 5u, 15u, 16u, 64u, 128u}) {
        for (bool inverse : {false, true}) {
            for (bool numeric_user : {false, true}) {
                Builder builder{Scope::Function};
                const auto entry = Block(builder), yes = Block(builder), no = Block(builder);
                Need(builder.SetEntry(entry));
                Need(builder.BeginBlock(entry));
                const auto condition = Emit(builder,
                                            inverse ? OpCode::TestNotFlags : OpCode::TestFlags,
                                            Type::Bool,
                                            {Operand::Flags(mask)});
                if (numeric_user) Store(builder, 0, condition);
                Need(builder.EndBlock(Terminal::Branch(condition, yes, no)));
                Need(builder.BeginBlock(yes));
                Need(builder.EndBlock(Terminal::Return(Literal(builder, 1))));
                Need(builder.BeginBlock(no));
                auto program = Return(builder, Literal(builder, 0));
                const auto allocation = x64::AllocateFunction(program);
                Check(bool(allocation) && allocation.homes[condition.node.value].terminal_only ==
                                                  (!numeric_user && !(mask & 16)),
                      "only a final condition without numerical users can skip its Bool home");
                const auto code = Compile(program);
#if defined(__x86_64__) && defined(__linux__)
                if (!NativeMode()) continue;
                NativeBlock native{code};
                for (unsigned pattern = 0; pattern < 256; ++pattern) {
                    State state;
                    state.flags = (u64{1} << 55) | ((u64{pattern & 15}) << 28) |
                                  ((pattern & 16) ? flags::Auxiliary : 0) |
                                  ((pattern & 32) ? flags::Direction : 0) | (pattern >> 6);
                    bool match = (mask & 1 && flags::GetCF(state.flags)) ||
                                 (mask & 2 && flags::GetOF(state.flags)) ||
                                 (mask & 4 && flags::GetZF(state.flags)) ||
                                 (mask & 8 && flags::GetSF(state.flags)) ||
                                 (mask & 16 && flags::GetPF(state.flags)) ||
                                 (mask & 64 && (state.flags & flags::Auxiliary)) ||
                                 (mask & 128 && (state.flags & flags::Direction));
                    const auto before = state.flags;
                    const bool expected = match != inverse;
                    Check(native.Run(state) == expected && state.flags == before,
                          "fused terminal preserves packed flags and both branch outcomes");
                    if (numeric_user)
                        Check(state.scalars[0] == expected,
                              "numerical Bool users retain materialization");
                }
#endif
            }
        }
    }
}

void FunctionLoop() {
    current_case = "Function backedge homes";
    Builder builder{Scope::Function};
    const auto exit = Block(builder), loop = Block(builder), entry = Block(builder);
    Need(builder.SetEntry(entry));
    Need(builder.BeginBlock(entry));
    const auto seed = Load(builder, 1), marker = LoadVector(builder, 0);
    const auto one = Literal(builder, 1);
    Need(builder.EndBlock(Terminal::Jump(loop)));
    Need(builder.BeginBlock(loop));
    const auto count = Load(builder, 0), accumulator = Load(builder, 2);
    const auto total = Emit(builder,
                            OpCode::Add,
                            Type::U64,
                            {Operand::ValueOf(accumulator), Operand::ValueOf(seed)});
    Store(builder, 2, total);
    const auto next =
            Emit(builder, OpCode::Sub, Type::U64, {Operand::ValueOf(count), Operand::ValueOf(one)});
    Store(builder, 0, next);
    const auto again = Emit(builder, OpCode::TestNotZero, Type::Bool, {Operand::ValueOf(next)});
    Need(builder.EndBlock(Terminal::Branch(again, loop, exit)));
    Need(builder.BeginBlock(exit));
    StoreVector(builder, 1, marker);
    const auto answer = Load(builder, 2);
    auto program = Return(builder, answer);
    auto code = Compile(program);
    State state;
    state.scalars[0] = 5;
    state.scalars[1] = 17;
    state.vectors[0] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    if (Run(code, state, 85))
        Check(!state.scalars[0] && state.vectors[1] == state.vectors[0],
              "RPO differs from block IDs and loop-invariant scalar/vector homes survive "
              "backedges");
}

void PairSpills() {
    current_case = "V128 register-bank spills";
    Builder builder{Scope::Block};
    Begin(builder);
    std::array<Value, 14> values;
    for (unsigned i = 0; i < values.size(); ++i) values[i] = LoadVector(builder, i);
    // Overwrite the backing slots so this fixture requires actual stack spills.
    for (unsigned i = 0; i < values.size(); ++i) StoreVector(builder, i, Vector(builder, {}));
    auto answer = values[0];
    for (unsigned i = 1; i < values.size(); ++i)
        answer = Emit(builder,
                      OpCode::VecXor,
                      Type::V128,
                      {Operand::ValueOf(answer), Operand::ValueOf(values[i])});
    StoreVector(builder, 15, answer);
    auto program = Return(builder);
    auto code = Compile(program);
    Check(code.spill_slots > 0, "simultaneous V128 values exceed the physical XMM bank");
    State state;
    Pair expected;
    for (unsigned i = 0; i < values.size(); ++i) {
        state.vectors[i] = {0x0101010101010101ULL * (i + 1),
                            0xfedcba9876543210ULL ^ (u64{i} << 33)};
        expected.low ^= state.vectors[i].low;
        expected.high ^= state.vectors[i].high;
    }
    if (Run(code, state))
        Check(state.vectors[15] == expected, "both halves reload from real spill homes");
}

void ShuffleBoundaries() {
    current_case = "V128 shuffle and 64-bit window boundary";
    Builder builder{Scope::Block};
    Begin(builder);
    constexpr Pair bytes{0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};
    constexpr Pair indices{0x08090a0b0c0d0e0fULL, 0x8001020304050607ULL};
    const auto input = Vector(builder, bytes), control = Vector(builder, indices);
    const auto lookup = Emit(builder,
                             OpCode::VecTableLookup8,
                             Type::V128,
                             {Operand::ValueOf(input), Operand::ValueOf(control)});
    StoreVector(builder, 0, lookup);
    const auto extract = Emit(builder,
                              OpCode::VecExtractBytes,
                              Type::V128,
                              {Operand::ValueOf(input), Operand::ValueOf(control), Imm(8)});
    StoreVector(builder, 1, extract);
    const auto zero = Emit(builder, OpCode::VecSharedZero, Type::V128, {});
    const auto shift = Emit(builder,
                            OpCode::VecByteShift,
                            Type::V128,
                            {Operand::ValueOf(input), Operand::ValueOf(zero), Imm(8), Imm(1)});
    StoreVector(builder, 2, shift);
    const auto zip = Emit(builder,
                          OpCode::VecZip,
                          Type::V128,
                          {Operand::ValueOf(input), Operand::ValueOf(control), Imm(32), Imm(1)});
    StoreVector(builder, 3, zip);
    auto program = Return(builder);
    auto code = Compile(program);
    State state;
    if (Run(code, state)) {
        Check(state.vectors[0] == Pair{0x08090a0b0c0d0e0fULL, 0x0001020304050607ULL},
              "PSHUFB high-bit zeroing");
        Check(state.vectors[1] == Pair{bytes.high, indices.low},
              "concatenated byte extraction across qwords");
        Check(state.vectors[2] == Pair{0, bytes.low}, "whole-vector left shift by exactly 64");
        Check(state.vectors[3] == Lanes32(0x0b0a0908, 0x04050607, 0x0f0e0d0c, 0x80010203),
              "high-half dword zip");
    }
}

void SaturationAndExtension() {
    current_case = "signed saturation, extension and widening";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto a = Vector(builder, {0x7f7f7f7f7f7f7f7fULL, 0x7f7f7f7f7f7f7f7fULL});
    const auto b = Vector(builder, {0x0101010101010101ULL, 0x0101010101010101ULL});
    const auto saturated = Emit(builder,
                                OpCode::VecSatAdd,
                                Type::V128,
                                {Operand::ValueOf(a), Operand::ValueOf(b), Imm(8), Imm(1)});
    StoreVector(builder, 0, saturated);
    const auto source = Vector(builder, {0x00040003fffeffffULL, 0x1122334455667788ULL});
    const auto extended = Emit(builder,
                               OpCode::VecExtend,
                               Type::V128,
                               {Operand::ValueOf(source), Imm(16), Imm(64), Imm(1)});
    StoreVector(builder, 1, extended);
    const auto multiplied = Emit(builder,
                                 OpCode::VecSignedMulWidenLow,
                                 Type::V128,
                                 {Operand::ValueOf(source), Operand::ValueOf(source), Imm(16)});
    StoreVector(builder, 2, multiplied);
    auto program = Return(builder);
    auto code = Compile(program);
    State state;
    if (Run(code, state)) {
        Check(state.vectors[0] == Pair{0x7f7f7f7f7f7f7f7fULL, 0x7f7f7f7f7f7f7f7fULL},
              "signed byte saturation");
        Check(state.vectors[1] == Pair{UINT64_MAX, UINT64_MAX - 1}, "signed low-lane widening");
        Check(state.vectors[2] == Lanes32(1, 4, 9, 16),
              "low contiguous source lanes, not even lanes");
    }
}

void WideIntegerEdges() {
    current_case = "64-bit vector average, saturation and packing boundaries";
    constexpr u64 minimum = UINT64_C(0x8000000000000000);
    constexpr u64 maximum = UINT64_C(0x7fffffffffffffff);

    struct Example {
        OpCode operation;
        Pair left, right;
        bool selector;
        Pair expected;
    };

    // Independent extremal constants: no wrapped host sum or reproduction of
    // the emitter's overflow formula is used to calculate the expected bits.
    // The final selector means signed arithmetic, or unsigned Pack destination.
    const std::array examples{
            Example{OpCode::VecAvg,
                    {UINT64_MAX, 0},
                    {UINT64_MAX, UINT64_MAX},
                    false,
                    {UINT64_MAX, minimum}},
            Example{OpCode::VecAvg, {UINT64_MAX - 1, 1}, {UINT64_MAX, 0}, false, {UINT64_MAX, 1}},
            Example{OpCode::VecSatAdd,
                    {UINT64_MAX, UINT64_MAX - 1},
                    {1, 1},
                    false,
                    {UINT64_MAX, UINT64_MAX}},
            Example{OpCode::VecSatSub, {0, UINT64_MAX}, {1, UINT64_MAX}, false, {0, 0}},
            Example{OpCode::VecSatAdd,
                    {maximum, minimum},
                    {1, UINT64_MAX},
                    true,
                    {maximum, minimum}},
            Example{OpCode::VecSatSub,
                    {minimum, maximum},
                    {1, UINT64_MAX},
                    true,
                    {minimum, maximum}},
            Example{OpCode::VecSatAdd,
                    {maximum, minimum},
                    {UINT64_MAX, 1},
                    true,
                    {maximum - 1, minimum + 1}},
            Example{OpCode::VecSatAdd,
                    {maximum, minimum},
                    {minimum, maximum},
                    true,
                    {UINT64_MAX, UINT64_MAX}},
            Example{OpCode::VecSatSub,
                    {maximum, minimum},
                    {minimum, maximum},
                    true,
                    {maximum, minimum}},
            Example{OpCode::VecPack,
                    {minimum, maximum},
                    {0xffffffff80000000ULL, 0x7fffffff},
                    false,
                    Lanes32(0x80000000, 0x7fffffff, 0x80000000, 0x7fffffff)},
            Example{OpCode::VecPack,
                    {UINT64_MAX, minimum},
                    {0x100000000ULL, 0xffffffff},
                    true,
                    Lanes32(0, 0, 0xffffffff, 0xffffffff)},
            Example{OpCode::VecPack,
                    {0x80000000, 0xffffffff7fffffffULL},
                    {0, UINT64_MAX},
                    false,
                    Lanes32(0x7fffffff, 0x80000000, 0, 0xffffffff)},
    };
    Builder builder{Scope::Block};
    Begin(builder);
    for (unsigned i = 0; i < examples.size(); ++i) {
        const auto& example = examples[i];
        const auto left = Vector(builder, example.left), right = Vector(builder, example.right);
        const auto value =
                example.operation == OpCode::VecAvg
                        ? Emit(builder,
                               example.operation,
                               Type::V128,
                               {Operand::ValueOf(left), Operand::ValueOf(right), Imm(64)})
                        : Emit(builder,
                               example.operation,
                               Type::V128,
                               {Operand::ValueOf(left),
                                Operand::ValueOf(right),
                                Imm(64),
                                Imm(example.selector)});
        StoreVector(builder, i, value);
    }
    auto program = Return(builder);
    auto code = Compile(program);
    State state;
    if (Run(code, state))
        for (unsigned i = 0; i < examples.size(); ++i)
            Check(state.vectors[i] == examples[i].expected,
                  "full-width carry/overflow and signed 64-to-32 saturation remain defined");
}

void LocalControlResults() {
    constexpr u64 sentinel = 0x1122334455667788ULL;
    constexpr u64 changed = 0xfedcba9876543210ULL;
    constexpr std::array conditions{u64{0}, u64{0x800000000000002aULL}};
    for (const auto opcode : {OpCode::Goto, OpCode::NotGoto}) {
        current_case = opcode == OpCode::Goto ? "Goto spilled result at local join"
                                              : "NotGoto spilled result at local join";
        Builder builder{Scope::Block};
        Begin(builder);
        std::array<Value, 8> prefix;
        for (unsigned i = 0; i < prefix.size(); ++i) prefix[i] = Load(builder, i);
        const auto condition = Load(builder, 8);
        const auto gate = Emit(builder, opcode, Type::U64, {Operand::ValueOf(condition)});
        const auto body = Literal(builder, changed);
        Store(builder, 9, body);
        Emit(builder, OpCode::BindLabel, Type::Void, {Operand::ValueOf(gate)});
        auto sum = prefix.front();
        for (unsigned i = 1; i < prefix.size(); ++i)
            sum = Emit(builder,
                       OpCode::Add,
                       Type::U64,
                       {Operand::ValueOf(sum), Operand::ValueOf(prefix[i])});
        Store(builder, 10, sum);
        auto program = Return(builder, gate);
        x64::Allocation allocation;
        swift::runtime::backend::CompileResult code;
        x64::Compile(program, Options(), code, allocation);
        Check(bool(code) && !code.words.empty() && bool(allocation), "local-control compilation");
        Check(allocation.homes[gate.node.value].spilled && code.spill_slots,
              "eight live prefix values force the local branch result into a spill home");
        for (const auto input : conditions) {
            State state;
            for (unsigned i = 0; i < prefix.size(); ++i) state.scalars[i] = i + 1;
            state.scalars[8] = input;
            state.scalars[9] = sentinel;
            if (Run(code, state, input)) {
                const bool skip = opcode == OpCode::Goto ? input != 0 : input == 0;
                Check(state.scalars[9] == (skip ? sentinel : changed),
                      "local branch polarity controls the body store");
                Check(state.scalars[10] == 36,
                      "all prefix values remain valid after both local paths join");
            }
        }
    }

    current_case = "local join forgets conditional static PC";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto condition = Load(builder, 0);
    const auto gate = Emit(builder, OpCode::Goto, Type::U64, {Operand::ValueOf(condition)});
    const auto destination = Literal(builder, 0x400123);
    Emit(builder,
         OpCode::StoreUniform,
         Type::Void,
         {Operand::Uniform(offsetof(State, pc), Type::U64), Operand::ValueOf(destination)});
    Emit(builder, OpCode::BindLabel, Type::Void, {Operand::ValueOf(gate)});
    Need(builder.EndBlock(Terminal::Dispatch()));
    auto program = Submit(builder);
    auto options = Options();
    options.dispatcher = 0x1000;
    options.exit_handler = 0x2000;
    const auto code = x64::Compile(program, options);
    Check(bool(code) && !code.words.empty() && code.links.empty(),
          "a PC literal set only in the conditional body cannot become an unconditional static "
          "link");
    // The callback addresses are placeholders for emission only. This case
    // checks publication metadata and never invokes its generated code.
}

void DynamicRounding() {
    current_case = "dynamic FP rounding";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto left = Vector(builder, {0x112233444b800000ULL, 0x8877665544332211ULL});
    const auto right = Vector(builder, {0x3f800000, 0});
    const auto added = Emit(builder,
                            OpCode::VecFAddScalar32,
                            Type::V128,
                            {Operand::ValueOf(left), Operand::ValueOf(right)});
    StoreVector(builder, 0, added);
    const auto integer = Literal(builder, 16777217);
    const auto converted = Emit(builder,
                                OpCode::VecFCvtIntToFloat,
                                Type::F32,
                                {Operand::ValueOf(integer), Imm(32), Imm(32)});
    Store(builder, 0, converted);
    const auto real = Literal(builder, 0x3ff8000000000000ULL);
    const auto rounded = Emit(builder,
                              OpCode::VecFCvtFloatToInt,
                              Type::U32,
                              {Operand::ValueOf(real), Imm(64), Imm(32), Imm(0)});
    Store(builder, 1, rounded);
    const auto numerator = Vector(builder, {0x3ff0000000000000ULL, 0xdeadbeefcafefeedULL});
    const auto denominator = Vector(builder, {0x4024000000000000ULL, 0});
    const auto divided = Emit(builder,
                              OpCode::VecFDivScalar64,
                              Type::V128,
                              {Operand::ValueOf(numerator), Operand::ValueOf(denominator)});
    StoreVector(builder, 1, divided);
    auto program = Return(builder);
    auto code = Compile(program);
    constexpr std::array single{0x4b800000u, 0x4b800000u, 0x4b800001u, 0x4b800000u};
    constexpr std::array integral{2u, 1u, 2u, 1u};
    constexpr std::array tenth{0x3fb999999999999aULL,
                               0x3fb9999999999999ULL,
                               0x3fb999999999999aULL,
                               0x3fb9999999999999ULL};
    for (unsigned mode = 0; mode < 4; ++mode) {
        State state;
        state.mxcsr |= mode << 13;
        if (Run(code, state)) {
            Check(state.vectors[0] ==
                          Pair{0x1122334400000000ULL | single[mode], 0x8877665544332211ULL},
                  "scalar add obeys guest MXCSR and preserves upper bits");
            Check(state.scalars[0] == single[mode] && state.scalars[1] == integral[mode],
                  "integer/float conversions share the selected guest rounding");
            Check(state.vectors[1] == Pair{tenth[mode], 0xdeadbeefcafefeedULL},
                  "double arithmetic observes guest rounding");
        }
    }
}

void FlushModes() {
    current_case = "independent DAZ and FTZ";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto tiny = Vector(builder, {1, 0}), zero = Vector(builder, {});
    const auto added = Emit(builder,
                            OpCode::VecFAddScalar32,
                            Type::V128,
                            {Operand::ValueOf(tiny), Operand::ValueOf(zero)});
    StoreVector(builder, 0, added);
    const auto minimum = Vector(builder, {0x00800000, 0}), half = Vector(builder, {0x3f000000, 0});
    const auto multiplied = Emit(builder,
                                 OpCode::VecFMulScalar32,
                                 Type::V128,
                                 {Operand::ValueOf(minimum), Operand::ValueOf(half)});
    StoreVector(builder, 1, multiplied);
    const auto one = Vector(builder, {0x3f800000, 0});
    const auto selected =
            Emit(builder,
                 OpCode::VecFMinMax,
                 Type::V128,
                 {Operand::ValueOf(tiny), Operand::ValueOf(one), Imm(32), Imm(0), Imm(1)});
    StoreVector(builder, 2, selected);
    auto program = Return(builder);
    auto code = Compile(program);
    for (unsigned mode = 0; mode < 4; ++mode) {
        State state;
        const bool daz = mode & 1, ftz = mode & 2;
        state.mxcsr |= (u32{daz} << 6) | (u32{ftz} << 15);
        if (Run(code, state)) {
            Check(state.vectors[0].low == (daz || ftz ? 0 : 1), "input/output subnormal controls");
            Check(state.vectors[1].low == (ftz ? 0 : 0x00400000),
                  "FTZ independently flushes generated subnormals");
            Check(state.vectors[2].low == (daz ? 0 : 1),
                  "min/max uses DAZ and preserves selected raw value: mode=" +
                          std::to_string(mode) + " actual=" + std::to_string(state.vectors[2].low));
        }
    }
}

void FusedArithmeticBoundaries() {
    current_case = "FMA exact zero, one rounding and NaN policy";
    for (const unsigned bits : {32u, 64u}) {
        const unsigned fraction = bits == 32 ? 23 : 52, bias = bits == 32 ? 127 : 1023;
        const u64 sign = u64{1} << (bits - 1),
                  infinity = bits == 32 ? 0x7f800000 : UINT64_C(0x7ff0000000000000),
                  quiet = u64{1} << (fraction - 1), one = u64{bias} << fraction,
                  two = u64{bias + 1} << fraction, half = u64{bias - 1} << fraction,
                  quarter = u64{bias - 2} << fraction, minimum = u64{1} << fraction,
                  maximum = infinity - 1, half_ulp = u64{bias - fraction - 1} << fraction;
        for (unsigned mode = 0; mode < 4; ++mode) {
            const unsigned control = 0x1f80 | (mode << 13);
            const auto fma = [&](u64 a, u64 b, u64 c, unsigned flags = 0, unsigned extra = 0) {
                return x64::FusedMultiplyAdd(a, b, c, bits, flags, control | extra);
            };
            const u64 cancelled = mode == 1 ? sign : 0;
            Check(fma(one, one, one | sign) == cancelled,
                  "exact cancellation zero follows directed rounding");
            Check(fma(0, one, sign) == cancelled && fma(sign, one, 0) == cancelled,
                  "opposite signed zeros obey active rounding");
            Check(fma(sign, one, sign) == sign && fma(0, one, 0) == 0,
                  "equal signed zeros retain their sign");
            Check(fma(one, one, one, 1) == cancelled && fma(one, one, one, 2) == cancelled &&
                          fma(one, one, one, 3) == (two | sign),
                  "product/addend negation is applied before the fused operation");
            Check(fma(one, one, half_ulp) == one + (mode == 2),
                  "positive halfway result is rounded once");
            Check(fma(one | sign, one, half_ulp | sign) == (one | sign) + (mode == 1),
                  "negative halfway result is rounded once");
            const u64 below_one = one - 2;
            Check(fma(one + 1, below_one, one | sign) ==
                          (sign | (u64{bias - 2 * fraction} << fraction)),
                  "unrounded product survives exact near cancellation");
            Check(fma(maximum, two, 0) == ((mode == 1 || mode == 3) ? maximum : infinity),
                  "positive overflow obeys rounding direction");
            Check(fma(maximum | sign, two, 0) ==
                          (sign | ((mode == 2 || mode == 3) ? maximum : infinity)),
                  "negative overflow obeys rounding direction");
            Check(fma(1, quarter, 0) == (mode == 2 ? 1 : 0),
                  "positive tiny result rounds toward the requested direction");
            Check(fma(sign | 1, quarter, 0) == (sign | (mode == 1 ? 1 : 0)),
                  "negative tiny result retains the correct zero sign");
            Check(fma(1, one, 0) == 1 && fma(1, one, 0, 0, 64) == 0,
                  "DAZ flushes inputs independently of output FTZ");
            Check(fma(minimum, half, 0) == (minimum >> 1) && fma(minimum, half, 0, 0, 0x8000) == 0,
                  "FTZ flushes subnormal fused outputs");
            const u64 first_nan = sign | infinity | 0x123, second_nan = infinity | quiet | 0x456;
            Check(fma(first_nan, second_nan, one, 3) == (first_nan | quiet),
                  "first source NaN retains original sign and payload despite arithmetic negation");
            Check(fma(one, first_nan, second_nan) == (first_nan | quiet) &&
                          fma(one, one, first_nan, 2) == (first_nan | quiet),
                  "NaNs in multiplicand and addend are quieted without losing payload");
            Check(fma(0, infinity, one) == (sign | infinity | quiet) &&
                          fma(infinity, one, infinity | sign) == (sign | infinity | quiet),
                  "invalid arithmetic returns the canonical negative quiet NaN");
        }
    }
}

void FusedDirectedZeroNative() {
    current_case = "native FMA helper preserves directed zero and upper lanes";
    Builder builder{Scope::Block};
    Begin(builder);
    const Pair merge32{0x556677883f800000ULL, 0x123456789abcdef0ULL};
    const Pair merge64{0x3ff0000000000000ULL, 0xfedcba9876543210ULL};
    const auto emit = [&](Pair merge, u64 one, u64 minus_one, unsigned bits, unsigned slot) {
        const auto a = Vector(builder, merge), b = Vector(builder, {one, 0}),
                   c = Vector(builder, {minus_one, 0});
        StoreVector(builder,
                    slot,
                    Emit(builder,
                         OpCode::VecFMulAdd,
                         Type::V128,
                         {Operand::ValueOf(a),
                          Operand::ValueOf(b),
                          Operand::ValueOf(c),
                          Imm(bits),
                          Imm(4)}));
    };
    emit(merge32, 0x3f800000, 0xbf800000, 32, 0);
    emit(merge64, 0x3ff0000000000000ULL, 0xbff0000000000000ULL, 64, 1);
    auto program = Return(builder);
    auto code = Compile(program);
    for (unsigned mode = 0; mode < 4; ++mode) {
        State state;
        state.mxcsr |= mode << 13;
        if (Run(code, state)) {
            Check(state.vectors[0] == Pair{0x5566778800000000ULL | (mode == 1 ? 0x80000000ULL : 0),
                                           merge32.high},
                  "F32 directed zero survives actual helper return and scalar merge");
            Check(state.vectors[1] == Pair{mode == 1 ? 0x8000000000000000ULL : 0, merge64.high},
                  "F64 directed zero survives actual helper return and scalar merge");
        }
    }
}

void FloatingCompareStatus() {
    current_case = "native FP comparison signaling and scalar active lanes";
    for (unsigned bits : {32u, 64u}) {
        const u64 one = bits == 32 ? 0x3f800000 : UINT64_C(0x3ff0000000000000),
                  two = bits == 32 ? 0x40000000 : UINT64_C(0x4000000000000000),
                  snan = bits == 32 ? 0x7f800123 : UINT64_C(0x7ff0000000000123),
                  quiet = u64{1} << (bits == 32 ? 22 : 51),
                  mask = bits == 32 ? UINT32_MAX : UINT64_MAX;
        const auto lanes = [&](u64 low) {
            return bits == 32 ? Lanes32(low, snan, snan, snan) : Pair{low, snan};
        };
        for (bool signaling : {false, true}) {
            Builder flags_builder{Scope::Block};
            Begin(flags_builder);
            auto a = LoadVector(flags_builder, 0), b = LoadVector(flags_builder, 1);
            auto flags = Emit(
                    flags_builder,
                    OpCode::VecFCmp,
                    Type::U32,
                    {Operand::ValueOf(a), Operand::ValueOf(b), Imm(bits | (signaling ? 128 : 0))});
            Store(flags_builder, 0, flags);
            auto p = Return(flags_builder);
            auto code = Compile(p);
            State state;
            state.vectors[0] = lanes(one);
            state.vectors[1] = lanes(snan | quiet);
            if (Run(code, state)) {
                Check(state.scalars[0] == 7 && (state.mxcsr & 63) == (signaling ? 1 : 0),
                      "COMIS vs UCOMIS differ for QNaN without observing upper lanes");
            }
            for (unsigned relation = 0; relation < 16; ++relation) {
                Builder builder{Scope::Block};
                Begin(builder);
                a = LoadVector(builder, 0);
                b = LoadVector(builder, 1);
                const unsigned predicate = relation | (signaling ? 16 : 0);
                const auto result = Emit(builder,
                                         OpCode::VecFCmpMask,
                                         Type::V128,
                                         {Operand::ValueOf(a),
                                          Operand::ValueOf(b),
                                          Imm(bits),
                                          Imm(predicate),
                                          Imm(1)});
                StoreVector(builder, 2, result);
                auto program = Return(builder);
                auto options = Options();
                options.relocatable = true;
                auto compiled = x64::Compile(program, options);
                Check(bool(compiled) && compiled.relocatable,
                      "all quiet/signaling comparison shapes compile");
                const bool has_leaf = std::any_of(
                        compiled.relocations.begin(),
                        compiled.relocations.end(),
                        [](const auto& r) {
                            return r.symbol ==
                                   swift::runtime::backend::HostSymbol::X64FloatingCompare;
                        });
                Check(has_leaf == x64::FloatingCompareNeedsCall(predicate),
                      "only unavailable SSE2 predicates use a numerical call");
                auto bytes = std::span<std::uint8_t>{
                        reinterpret_cast<std::uint8_t*>(compiled.words.data()),
                        compiled.words.size() * 4};
                Check(x64::Relocate(bytes, compiled.relocations, options),
                      "floating compare relocations resolve semantically");
                state = {};
                state.vectors[0] = lanes(one);
                state.vectors[1] = lanes(snan | quiet);
                if (Run(compiled, state)) {
                    const auto low = relation & 8 ? mask : 0;
                    Check(state.vectors[2] == lanes(low) &&
                                  (state.mxcsr & 63) == (signaling ? 1 : 0),
                          "QNaN predicate result and signaling are independent");
                }
                state = {};
                state.vectors[0] = lanes(one);
                state.vectors[1] = lanes(snan);
                if (Run(compiled, state))
                    Check((state.mxcsr & 63) == 1, "even constant predicates observe SNaN");
                state = {};
                state.vectors[0] = lanes(one);
                state.vectors[1] = lanes(two);
                if (Run(compiled, state)) {
                    const auto low = relation & 1 ? mask : 0;
                    Check(state.vectors[2] == lanes(low) && (state.mxcsr & 63) == 0,
                          "ordered predicate preserves inactive scalar lanes and their exceptions");
                }
            }
        }
    }
}

void FloatingRoundFusedStatus() {
    current_case = "native ROUND suppression and fused helper exception status";
    bool sse41_available = true;
#if defined(__x86_64__) && defined(__linux__)
    unsigned ca, cb, cc, cd;
    sse41_available = __get_cpuid(1, &ca, &cb, &cc, &cd) && (cc & (1u << 19));
#endif
    for (unsigned bits : {32u, 64u}) {
        const u64 one = bits == 32 ? 0x3f800000 : UINT64_C(0x3ff0000000000000),
                  half = bits == 32 ? 0x3f000000 : UINT64_C(0x3fe0000000000000),
                  two = bits == 32 ? 0x40000000 : UINT64_C(0x4000000000000000),
                  snan = bits == 32 ? 0x7f800123 : UINT64_C(0x7ff0000000000123);
        const auto lanes = [&](u64 low) {
            return bits == 32 ? Lanes32(low, snan, snan, snan) : Pair{low, snan};
        };
        for (bool sse41 : {false, true}) {
            if (sse41 && NativeMode() && !sse41_available) continue;
            for (unsigned mode : {0u, 8u}) {
                Builder builder{Scope::Block};
                Begin(builder);
                const auto a = LoadVector(builder, 0), b = LoadVector(builder, 1);
                const auto rounded = Emit(
                        builder,
                        OpCode::VecFRoundInt,
                        Type::V128,
                        {Operand::ValueOf(a), Operand::ValueOf(b), Imm(bits), Imm(mode), Imm(1)});
                StoreVector(builder, 2, rounded);
                auto program = Return(builder);
                auto options = Options();
                options.sse41 = sse41;
                auto code = x64::Compile(program, options);
                Check(bool(code), "ROUND flags compile with and without SSE4.1");
                State state;
                state.vectors[0] = lanes(one);
                state.vectors[1] = lanes(one + (one - half) / 2);
                if (Run(code, state)) {
                    Check(state.vectors[2] == lanes(two) && (state.mxcsr & 63) == (mode ? 0 : 32),
                          "ROUND suppresses precision only when requested");
                }
                state = {};
                state.vectors[0] = lanes(one);
                state.vectors[1] = lanes(snan);
                if (Run(code, state))
                    Check((state.mxcsr & 63) == 1, "ROUND suppression never suppresses invalid");
            }
        }
        Builder builder{Scope::Block};
        Begin(builder);
        const auto a = Vector(builder, lanes(one)), zero = Vector(builder, {});
        const auto invalid = Emit(builder,
                                  bits == 32 ? OpCode::VecFDivScalar32 : OpCode::VecFDivScalar64,
                                  Type::V128,
                                  {Operand::ValueOf(zero), Operand::ValueOf(zero)});
        StoreVector(builder, 0, invalid);
        const auto half_ulp =
                Vector(builder, lanes(bits == 32 ? 0x33800000 : UINT64_C(0x3ca0000000000000)));
        const auto fused = Emit(builder,
                                OpCode::VecFMulAdd,
                                Type::V128,
                                {Operand::ValueOf(a),
                                 Operand::ValueOf(a),
                                 Operand::ValueOf(half_ulp),
                                 Imm(bits),
                                 Imm(4)});
        StoreVector(builder, 1, fused);
        const auto selected =
                Emit(builder,
                     OpCode::VecFMinMax,
                     Type::V128,
                     {Operand::ValueOf(a), Operand::ValueOf(a), Imm(bits), Imm(0), Imm(1)});
        StoreVector(builder, 2, selected);
        auto program = Return(builder);
        auto code = Compile(program);
        State state;
        if (Run(code, state)) {
            Check((state.mxcsr & 63) == (1 | 32),
                  "native sticky survives a helper and receives FMA precision");
            Check(state.vectors[1] == lanes(one) && state.vectors[2] == lanes(one),
                  "FMA/min scalar upper SNaN is preserved without contributing exceptions");
        }
    }
}

void CryptoFallbackABI() {
    current_case = "SysV crypto fallback aggregate rollback";
    Builder builder{Scope::Block};
    Begin(builder);
    std::array<Value, 8> live;
    for (unsigned i = 0; i < live.size(); ++i) live[i] = Load(builder, i);
    const auto a = Vector(builder, {1, 2}), b = Vector(builder, {4, 8}), zero = Vector(builder, {});
    constexpr std::array<unsigned, 4> controls{0, 1, 0x10, 0x11};
    for (unsigned i = 0; i < controls.size(); ++i)
        StoreVector(builder,
                    i,
                    Emit(builder,
                         OpCode::VecPclMul,
                         Type::V128,
                         {Operand::ValueOf(a), Operand::ValueOf(b), Imm(controls[i])}));
    for (unsigned i = 0; i < 2; ++i)
        StoreVector(builder,
                    4 + i,
                    Emit(builder,
                         OpCode::VecAesKeygenAssist,
                         Type::V128,
                         {Operand::ValueOf(zero), Imm(i ? 0x1b : 1)}));
    const Pair sha_a{0x0123456789abcdefULL, 0x76543210fedcba98ULL},
            sha_b{0x1122334455667788ULL, 0x99aabbccddeeff00ULL},
            sha_key{0x428a2f9871374491ULL, 0xb5c0fbcfe9b5dba5ULL};
    const auto sa = Vector(builder, sha_a), sb = Vector(builder, sha_b),
               sk = Vector(builder, sha_key);
    StoreVector(builder,
                6,
                Emit(builder,
                     OpCode::VecSha256Rnds2,
                     Type::V128,
                     {Operand::ValueOf(sa), Operand::ValueOf(sb), Operand::ValueOf(sk)}));
    auto sum = live.front();
    for (unsigned i = 1; i < live.size(); ++i)
        sum = Emit(builder,
                   OpCode::Add,
                   Type::U64,
                   {Operand::ValueOf(sum), Operand::ValueOf(live[i])});
    auto program = Return(builder, sum);
    auto options = Options();
    options.aes = options.pclmul = options.sha = false;
    auto code = x64::Compile(program, options);
    Check(bool(code), "crypto fallback compilation");
    State state;
    for (unsigned i = 0; i < live.size(); ++i) state.scalars[i] = i + 1;
    if (Run(code, state, 36)) {
        constexpr std::array<u64, 4> products{4, 8, 8, 16};
        for (unsigned i = 0; i < products.size(); ++i)
            Check(state.vectors[i] == Pair{products[i], 0},
                  "fallback PCLMUL receives its actual nonzero selector");
        Check(state.vectors[4] == Pair{0x6363636263636363ULL, 0x6363636263636363ULL} &&
                      state.vectors[5] == Pair{0x6363637863636363ULL, 0x6363637863636363ULL},
              "fallback AESKEYGENASSIST receives the actual round constant");
        const auto expected = swift::runtime::x86::crypto::Execute(
                swift::runtime::x86::crypto::Operation::Sha256Rounds2,
                {sha_a.low, sha_a.high},
                {sha_b.low, sha_b.high},
                {sha_key.low, sha_key.high});
        Check(state.vectors[6] == Pair{expected.low, expected.high},
              "fallback SHA receives the complete third aggregate in C ABI order");
    }
}

void NaNsAndFusedArithmetic() {
    current_case = "NaN payloads and fused arithmetic";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto a = Vector(builder, Lanes32(0x7f812345, 0x3f800000, 0x7f800000, 0x80000000));
    const auto b = Vector(builder, Lanes32(0x3f800000, 0xffc54321, 0xff800000, 0x80000000));
    const auto sum = Emit(builder,
                          OpCode::VecFAdd,
                          Type::V128,
                          {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32)});
    StoreVector(builder, 0, sum);
    const auto first = Vector(builder, {0x556677883f800001ULL, 0x123456789abcdef0ULL});
    const auto second = Vector(builder, {0x3f7ffffe, 0}), addend = Vector(builder, {0xbf800000, 0});
    const auto fused = Emit(builder,
                            OpCode::VecFMulAdd,
                            Type::V128,
                            {Operand::ValueOf(first),
                             Operand::ValueOf(second),
                             Operand::ValueOf(addend),
                             Imm(32),
                             Imm(4)});
    StoreVector(builder, 1, fused);
    const auto ones = Vector(builder, Lanes32(0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000));
    const auto twos = Vector(builder, Lanes32(0x40000000, 0x40000000, 0x40000000, 0x40000000));
    const auto threes = Vector(builder, Lanes32(0x40400000, 0x40400000, 0x40400000, 0x40400000));
    const auto alternating = Emit(builder,
                                  OpCode::VecFMulAdd,
                                  Type::V128,
                                  {Operand::ValueOf(ones),
                                   Operand::ValueOf(twos),
                                   Operand::ValueOf(threes),
                                   Imm(32),
                                   Imm(8)});
    StoreVector(builder, 2, alternating);
    auto program = Return(builder);
    auto code = Compile(program);
    State state;
    if (Run(code, state)) {
        Check(state.vectors[0] == Lanes32(0x7fc12345, 0xffc54321, 0xffc00000, 0x80000000),
              "source NaNs retain payload; invalid arithmetic uses guest negative QNaN");
        // (1+2^-23)*(1-2^-23)-1 is exactly -2^-46. Separate MUL/ADD gives zero.
        Check(state.vectors[1] == Pair{0x55667788a8800000ULL, 0x123456789abcdef0ULL},
              "FMA rounds once and scalar mode preserves upper bits");
        Check(state.vectors[2] == Lanes32(0xbf800000, 0x40a00000, 0xbf800000, 0x40a00000),
              "alternating FMA addend signs");
    }
}

u64 helper_environment{};

swift::runtime::x86::CpuQueryResult Query(u64 leaf, u64 subleaf) noexcept {
#if defined(__x86_64__) && defined(__linux__)
    if (NativeMode()) {
        unsigned control;
        asm volatile("stmxcsr %0" : "=m"(control));
        helper_environment = control;
        control = 0x1f80;
        asm volatile("ldmxcsr %0" ::"m"(control));
    }
#endif
    return {leaf + subleaf, (leaf << 32) | subleaf};
}

void HelperABI() {
    current_case = "helper pair return and live SSA";
    Builder builder{Scope::Block};
    Begin(builder);
    std::array<Value, 4> live;
    for (unsigned i = 0; i < live.size(); ++i) live[i] = Load(builder, i);
    const auto vector = LoadVector(builder, 0);
    const auto leaf = Literal(builder, 7), subleaf = Literal(builder, 9);
    const auto query = Emit(builder,
                            OpCode::Cpuid,
                            Type::U64,
                            {Operand::ValueOf(leaf),
                             Operand::ValueOf(subleaf),
                             Imm(reinterpret_cast<std::uintptr_t>(&Query))});
    const auto upper = Emit(builder, OpCode::CpuidUpper, Type::U64, {Operand::ValueOf(query)});
    Store(builder, 4, upper);
    const auto high =
            Emit(builder, OpCode::VecExtract64, Type::U64, {Operand::ValueOf(vector), Imm(1)});
    auto answer = Emit(
            builder, OpCode::Add, Type::U64, {Operand::ValueOf(query), Operand::ValueOf(high)});
    for (const auto value : live)
        answer = Emit(builder,
                      OpCode::Add,
                      Type::U64,
                      {Operand::ValueOf(answer), Operand::ValueOf(value)});
    const auto a = Vector(builder, {0x4b800000, 0}), b = Vector(builder, {0x3f800000, 0});
    const auto rounded = Emit(builder,
                              OpCode::VecFAddScalar32,
                              Type::V128,
                              {Operand::ValueOf(a), Operand::ValueOf(b)});
    StoreVector(builder, 1, rounded);
    auto program = Return(builder, answer);
    auto code = Compile(program);
    State state;
    state.scalars[0] = 1;
    state.scalars[1] = 2;
    state.scalars[2] = 3;
    state.scalars[3] = 4;
    state.vectors[0] = {7, 11};
    state.mxcsr |= 2u << 13;
    state.flags = flags::Direction | flags::Carry;
    if (Run(code, state, 37)) {
        Check(state.scalars[4] == 0x700000009ULL, "second callback return survives intervening IR");
        Check(state.vectors[1].low == 0x4b800001, "guest MXCSR is reinstalled after a host helper");
        Check(helper_environment == 0x7fa1, "helper runs in the saved caller FP environment");
        Check(state.flags == (flags::Direction | flags::Carry),
              "pure helper and ordinary arithmetic retain flags");
    }
}

void FaultMetadata() {
    current_case = "fault metadata range";
    static const u64 memory = 0x13579bdf2468ace0ULL;
    Builder builder{Scope::Block};
    Begin(builder);
    Emit(builder, OpCode::FaultLocation, Type::Void, {Imm(0x12345678)});
    const auto address = Literal(builder, reinterpret_cast<std::uintptr_t>(&memory));
    const auto loaded =
            Emit(builder, OpCode::LoadMemoryTSO, Type::U64, {Operand::ValueOf(address)});
    auto program = Return(builder, loaded);
    auto code = Compile(program);
    Check(!code.faults.empty() && code.faults.back().guest_pc == 0x12345678,
          "native fault location retains the original guest PC");
    State state;
    Run(code, state, memory);
}

void FlatExpressionShifts() {
    current_case = "fixed CL in flat RHS operand expressions";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto five = Literal(builder, 5), ten = Literal(builder, 10), one = Literal(builder, 1),
               three = Literal(builder, 3), negative = Literal(builder, UINT64_MAX - 7);
    const auto simple = Emit(builder,
                             OpCode::Add,
                             Type::U64,
                             {Operand::ValueOf(five),
                              Operand::Expression(OperandOp::Lsl, 2),
                              Operand::ValueOf(one),
                              Operand::ValueOf(three)});
    Store(builder, 0, simple);
    Store(builder,
          1,
          Emit(builder,
               OpCode::GetOperand,
               Type::U64,
               {Operand::Expression(OperandOp::Asr, 2),
                Operand::ValueOf(negative),
                Operand::ValueOf(one)}));
    const auto result = Emit(builder,
                             OpCode::Add,
                             Type::U64,
                             {Operand::ValueOf(simple),
                              Operand::Expression(OperandOp::Lsr, 2),
                              Operand::ValueOf(ten),
                              Operand::ValueOf(three)});
    auto program = Return(builder, result);
    auto code = Compile(program);
    State state;
    if (Run(code, state, 14))
        Check(state.scalars[0] == 13 && state.scalars[1] == UINT64_MAX - 3,
              "CL staging preserves the shifted value and signed arithmetic shift");
}

void PackedFeaturePaths() {
    current_case = "baseline and optional packed instruction selection";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto a = Vector(builder, Lanes32(5, UINT32_MAX, 0x80000000, 100)),
               b = Vector(builder, Lanes32(7, 1, 0, 200));
    StoreVector(builder,
                0,
                Emit(builder,
                     OpCode::VecMin,
                     Type::V128,
                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32), Imm(0)}));
    StoreVector(builder,
                1,
                Emit(builder,
                     OpCode::VecMax,
                     Type::V128,
                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32), Imm(1)}));
    StoreVector(builder,
                2,
                Emit(builder,
                     OpCode::VecMul,
                     Type::V128,
                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32)}));
    StoreVector(builder,
                3,
                Emit(builder,
                     OpCode::VecPack,
                     Type::V128,
                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32), Imm(0)}));
    StoreVector(builder,
                4,
                Emit(builder,
                     OpCode::VecExtend,
                     Type::V128,
                     {Operand::ValueOf(a), Imm(8), Imm(32), Imm(0)}));
    StoreVector(builder,
                5,
                Emit(builder,
                     OpCode::VecShiftLeftImm,
                     Type::V128,
                     {Operand::ValueOf(a), Imm(3), Imm(16)}));
    Store(builder, 0, Emit(builder, OpCode::VecMovMask, Type::U64, {Operand::ValueOf(a), Imm(16)}));
    const auto fp = Vector(builder, Lanes32(0x3fc00000, 0xbfc00000, 0x7f812345, 0x80000000));
    StoreVector(builder,
                6,
                Emit(builder,
                     OpCode::VecFRoundInt,
                     Type::V128,
                     {Operand::ValueOf(fp), Operand::ValueOf(fp), Imm(32), Imm(0), Imm(0)}));
    auto program = Return(builder);
    bool feature_available = true;
#if defined(__x86_64__) && defined(__linux__)
    unsigned ca, cb, cc, cd;
    feature_available = __get_cpuid(1, &ca, &cb, &cc, &cd) && (cc & (1u << 19));
#endif
    for (bool optional : {false, true}) {
        if (optional && NativeMode() && !feature_available) continue;
        auto options = Options();
        options.sse41 = optional;
        auto code = x64::Compile(program, options);
        Check(bool(code), "packed native/fallback emission");
        State state;
        if (Run(code, state)) {
            Check(state.vectors[0] == Lanes32(5, 1, 0, 100) &&
                          state.vectors[1] == Lanes32(7, 1, 0, 200),
                  "packed signedness and lane selection");
            Check(state.vectors[2] == Lanes32(35, UINT32_MAX, 0, 20000), "packed low product");
            Check(state.vectors[3] == Pair{0x00648000ffff0005, 0x00c8000000010007},
                  "signed dword packing");
            Check(state.vectors[4] == Lanes32(5, 0, 0, 0), "unsigned byte widening");
            Check(state.vectors[5] == Pair{0xfff8fff800000028, 0x0000032000000000} &&
                          state.scalars[0] == 0x2c,
                  "packed immediate shifts and word sign mask");
            Check(state.vectors[6] == Lanes32(0x40000000, 0xc0000000, 0x7fc12345, 0x80000000),
                  "integral rounding quiets NaNs and retains zero sign");
        }
    }
}

void AtomicOperations() {
    current_case = "native atomics and ARM min/max";
    alignas(16) u64 memory = 5;
    Builder builder{Scope::Block};
    Begin(builder);
    const auto address = Literal(builder, reinterpret_cast<std::uintptr_t>(&memory));
    const auto operand = [&](u64 value, Type type) {
        return Operand::ValueOf(Literal(builder, value, type));
    };
    const auto fetch = Emit(builder,
                            OpCode::AtomicFetchAdd,
                            Type::U64,
                            {Operand::ValueOf(address), operand(3, Type::U64)});
    Store(builder, 0, fetch);
    const auto cas =
            Emit(builder,
                 OpCode::CompareAndSwap,
                 Type::U64,
                 {Operand::ValueOf(address), operand(8, Type::U64), operand(11, Type::U64)});
    Store(builder, 1, cas);
    const auto rmw = [&](AtomicRMWOp operation, u64 value, unsigned slot) {
        const auto result = Emit(builder,
                                 OpCode::AtomicRMW,
                                 Type::U64,
                                 {Imm(unsigned(operation), Type::U8),
                                  Operand::ValueOf(address),
                                  operand(value, Type::U64),
                                  operand(0, Type::U64)});
        Store(builder, slot, result);
    };
    rmw(AtomicRMWOp::SignedMin, UINT64_MAX, 2);
    rmw(AtomicRMWOp::SignedMax, 4, 3);
    rmw(AtomicRMWOp::UnsignedMax, UINT64_MAX, 4);
    rmw(AtomicRMWOp::UnsignedMin, 9, 5);
    auto program = Return(builder);
    auto code = Compile(program);
    State state;
    if (Run(code, state)) {
        Check(memory == 9, "native atomic update sequence");
        Check(state.scalars[0] == 5 && state.scalars[1] == 8 && state.scalars[2] == 11 &&
                      state.scalars[3] == UINT64_MAX && state.scalars[4] == 4 &&
                      state.scalars[5] == UINT64_MAX,
              "atomic RMW returns original memory and uses requested signedness");
    }
    for (const bool native : {false, true}) {
        alignas(16) Pair pair{17, 23};
        Builder wide{Scope::Block};
        Begin(wide);
        const auto literal = [&](u64 n) { return Operand::ValueOf(Literal(wide, n)); };
        const auto result = Emit(wide,
                                 OpCode::CompareAndSwap128,
                                 Type::V128,
                                 {literal(reinterpret_cast<std::uintptr_t>(&pair)),
                                  literal(17),
                                  literal(23),
                                  literal(31),
                                  literal(37)});
        StoreVector(wide, 0, result);
        auto p = Return(wide);
        auto options = Options();
        options.cx16 = native;
        auto c = x64::Compile(p, options);
        Check(bool(c), "CAS128 native and guarded helper emission");
        if (!native) {
            auto linked_options = options;
            linked_options.dispatcher = 0x1000;
            linked_options.exit_handler = 0x2000;
            linked_options.relocatable = true;
            const auto linked = x64::Compile(p, linked_options);
            Check(bool(linked) && linked.relocatable,
                  "linked CAS128 emits the owner atomic service packet");
            Check(std::none_of(
                          linked.relocations.begin(),
                          linked.relocations.end(),
                          [](const auto& relocation) {
                              return relocation.symbol ==
                                     swift::runtime::backend::HostSymbol::X64CompareExchange128;
                          }),
                  "owner atomic service does not enter the standalone libatomic helper");
        }
        State st;
        if (Run(c, st))
            Check(st.vectors[0] == Pair{17, 23} && pair == Pair{31, 37},
                  "CAS128 returns coherent original pair and commits desired pair");
    }
}

void PersistentLinks() {
    current_case = "byte-offset relocation and static edge slots";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto pc = Literal(builder, 0x400123);
    Emit(builder,
         OpCode::StoreUniform,
         Type::Void,
         {Operand::Uniform(offsetof(State, pc), Type::U64), Operand::ValueOf(pc)});
    Need(builder.EndBlock(Terminal::Dispatch()));
    auto program = Submit(builder);
    auto options = Options();
    options.dispatcher = 0x123456789abcULL;
    options.exit_handler = 0xfedcba987654ULL;
    options.relocatable = true;
    auto code = x64::Compile(program, options);
    Check(bool(code) && code.relocatable && code.links.size() == 1 && !code.relocations.empty(),
          "linked native unit records semantic MOVABS relocations: error=" +
                  std::to_string(unsigned(code.error)) + " relocatable=" +
                  std::to_string(code.relocatable) + " links=" + std::to_string(code.links.size()) +
                  " relocs=" + std::to_string(code.relocations.size()));
    auto bytes =
            std::span{reinterpret_cast<std::uint8_t*>(code.words.data()), code.words.size() * 4};
    options.dispatcher = 0x1122334455667788ULL;
    Check(x64::Relocate(bytes, code.relocations, options),
          "MOVABS byte relocation rebinds process callbacks");
    const auto& site = code.links.front();
    constexpr u64 rx = 0x10000000, target = 0x20000000;
    Check(x64::InitializeNativeLink(bytes, rx, site, target, true),
          "link initialization uses actual byte offsets");
    auto* slots = reinterpret_cast<std::uintptr_t*>(bytes.data() + site.data_offset);
    Check(slots[0] == target && slots[1] == rx + site.fallback_offset,
          "static edge retains its own fallback");
    x64::UnlinkNativeLink(slots, slots, 0);
    Check(slots[0] == slots[1], "unlink restores cold dispatcher data");
    x64::PatchNativeLink(slots, slots, target, 0);
    Check(slots[0] == target, "patch publishes pointer only");
    auto invalid = code.relocations;
    invalid.front().offset = 1;
    Check(!x64::Relocate(bytes, invalid, options), "relocation rejects a non-MOVABS slot");
    Builder conditional{Scope::Block};
    Begin(conditional);
    const auto check = Load(conditional, 0);
    Emit(conditional,
         OpCode::StoreUniformSelectImm,
         Type::Void,
         {Operand::Uniform(offsetof(State, pc), Type::U64),
          Operand::ValueOf(check),
          Imm(0x400200),
          Imm(0x400300)});
    Need(conditional.EndBlock(Terminal::Dispatch()));
    auto conditional_program = Submit(conditional);
    auto edges = x64::Compile(conditional_program, options);
    Check(bool(edges) && edges.relocatable && edges.links.size() == 2,
          "terminal uniform PC select retains both real static edges");
    Check(edges.links[0].target_pc == 0x400200 && edges.links[1].target_pc == 0x400300,
          "offline sweep receives both actual guest successors");
    auto edge_bytes =
            std::span{reinterpret_cast<std::uint8_t*>(edges.words.data()), edges.words.size() * 4};
    Check(x64::Relocate(edge_bytes, edges.relocations, options),
          "both conditional cold trampolines retain semantic relocations");
    for (const auto& edge : edges.links)
        Check(x64::InitializeNativeLink(edge_bytes, rx, edge, 0, true),
              "each conditional target owns a valid link slot and fallback");
}

void ReusedByteSink() {
    current_case = "single-owner byte sink growth and forward fixups";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto gate = Emit(builder, OpCode::Goto, Type::U64, {Operand::ValueOf(Load(builder, 0))});
    const auto one = Vector(builder, {1, 1});
    auto value = Vector(builder, {0, 0});
    for (unsigned i = 0; i < 160; ++i)
        value = Emit(builder,
                     OpCode::VecSatAdd,
                     Type::V128,
                     {Operand::ValueOf(value), Operand::ValueOf(one), Imm(64), Imm(1)});
    StoreVector(builder, 0, value);
    Emit(builder, OpCode::BindLabel, Type::Void, {Operand::ValueOf(gate)});
    auto program = Return(builder);
    swift::runtime::backend::CompileResult code;
    x64::Allocation allocation;
    x64::Compile(program, Options(), code, allocation);
    Check(bool(code), "large byte stream compilation");
    const auto capacity = code.words.capacity();
    State state;
    if (Run(code, state))
        Check(state.vectors[0] == Pair{160, 160}, "grown stream executes all native instructions");
    state = {};
    state.scalars[0] = 1;
    state.vectors[0] = {77, 99};
    if (Run(code, state))
        Check(state.vectors[0] == Pair{77, 99}, "forward branch survives sink reallocation");
    x64::Compile(program, Options(), code, allocation);
    Check(bool(code) && code.words.capacity() == capacity,
          "recompile reuses the machine-byte owner capacity");
}
}  // namespace

TEST_CASE("ArithmeticFlags", "[compile][x64-compile][native]") { CHECK_NOTHROW(ArithmeticFlags()); }

TEST_CASE("ArithmeticFlagBoundaries", "[compile][x64-compile][native]") {
    ArithmeticFlagBoundaries();
}

TEST_CASE("ImmediateShiftBoundaries", "[compile][x64-compile][native]") {
    ImmediateShiftBoundaries();
}

TEST_CASE("ImmediateOperandBoundaries", "[compile][x64-compile][native]") {
    ImmediateOperandBoundaries();
}

TEST_CASE("NarrowReadsAndHelperABI", "[compile][x64-compile][native]") {
    NarrowReadsAndHelperABI();
}

TEST_CASE("UniformStoreViews", "[compile][x64-compile][native]") { UniformStoreViews(); }

TEST_CASE("FunctionLoop", "[compile][x64-compile][native]") { CHECK_NOTHROW(FunctionLoop()); }

TEST_CASE("ARM binary FP exact fast and slow paths", "[compile][x64-compile][native]") {
    ArmFPBinaryBoundaries();
    ArmFPUnalignedState();
}

TEST_CASE("Guest memory consumes actual allocation homes", "[compile][x64-compile][native]") {
    MemoryHomeOperands();
}

TEST_CASE("Packed flags feed terminal branches directly", "[compile][x64-compile][native]") {
    TerminalFlagConditions();
}

TEST_CASE("PairSpills", "[compile][x64-compile][native]") { CHECK_NOTHROW(PairSpills()); }

TEST_CASE("ShuffleBoundaries", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(ShuffleBoundaries());
}

TEST_CASE("SaturationAndExtension", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(SaturationAndExtension());
}

TEST_CASE("WideIntegerEdges", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(WideIntegerEdges());
}

TEST_CASE("LocalControlResults", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(LocalControlResults());
}

TEST_CASE("DynamicRounding", "[compile][x64-compile][native]") { CHECK_NOTHROW(DynamicRounding()); }

TEST_CASE("FlushModes", "[compile][x64-compile][native]") { CHECK_NOTHROW(FlushModes()); }

TEST_CASE("NaNsAndFusedArithmetic", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(NaNsAndFusedArithmetic());
}

TEST_CASE("HelperABI", "[compile][x64-compile][native]") { CHECK_NOTHROW(HelperABI()); }

TEST_CASE("FaultMetadata", "[compile][x64-compile][native]") { CHECK_NOTHROW(FaultMetadata()); }

TEST_CASE("AtomicOperations", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(AtomicOperations());
}

TEST_CASE("PersistentLinks", "[compile][x64-compile]") { CHECK_NOTHROW(PersistentLinks()); }

TEST_CASE("ReusedByteSink", "[compile][x64-compile][native]") { CHECK_NOTHROW(ReusedByteSink()); }

TEST_CASE("PackedFeaturePaths", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(PackedFeaturePaths());
}

TEST_CASE("FlatExpressionShifts", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(FlatExpressionShifts());
}

TEST_CASE("FusedArithmeticBoundaries", "[compile][x64-compile]") {
    CHECK_NOTHROW(FusedArithmeticBoundaries());
}

TEST_CASE("FusedDirectedZeroNative", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(FusedDirectedZeroNative());
}

TEST_CASE("CryptoFallbackABI", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(CryptoFallbackABI());
}

TEST_CASE("FloatingCompareStatus", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(FloatingCompareStatus());
}

TEST_CASE("FloatingRoundFusedStatus", "[compile][x64-compile][native]") {
    CHECK_NOTHROW(FloatingRoundFusedStatus());
}

TEST_CASE("Guest safepoint requires a linked execution context", "[compile][x64-compile]") {
    for (auto scope : {Scope::Block, Scope::Function}) {
        Builder builder{scope};
        Begin(builder);
        Emit(builder, OpCode::GuestSafepoint, Type::Void, {Imm(0x1234)});
        auto program = Return(builder, Literal(builder, 42));
        CHECK(!x64::Compile(program, Options()));
        auto options = Options();
        options.dispatcher = 0x1000;
        options.exit_handler = 0x2000;
        const auto code = x64::Compile(program, options);
        REQUIRE(bool(code));
        CHECK(!code.words.empty() && code.faults.empty() && code.links.empty());
    }
}

TEST_CASE("Native flags survive arithmetic, control, helpers and memory",
          "[compile][x64-compile][native]") {
    struct Mode {
        Mode() { test_local_flags = true; }

        ~Mode() { test_local_flags = false; }
    } mode;

    for (bool image : {false, true}) {
        test_lahf = image;
        for (auto scope : {Scope::Block, Scope::Function}) {
            for (unsigned invert = 0; invert < 2; ++invert) {
                for (unsigned condition = 0; condition < 16; ++condition) {
                    Builder builder{scope};
                    Begin(builder);
                    if (invert) Emit(builder, OpCode::InvertCarry, Type::Void, {});
                    const auto predicate = Emit(
                            builder, OpCode::CondSet, Type::Bool, {Operand::Condition(condition)});
                    if (scope == Scope::Function) {
                        const auto yes = Block(builder), no = Block(builder);
                        Need(builder.EndBlock(Terminal::Branch(predicate, yes, no)));
                        Need(builder.BeginBlock(yes));
                        Need(builder.EndBlock(Terminal::Return(Literal(builder, 1))));
                        Need(builder.BeginBlock(no));
                    }
                    const auto code = Compile(Return(
                            builder, scope == Scope::Block ? predicate : Literal(builder, 0)));
                    for (unsigned pattern = 0; pattern < 16; ++pattern) {
                        State state;
                        state.flags = (u64{1} << 55) | flags::Direction | flags::Auxiliary | 0xa5 |
                                      (u64{pattern} << 28);
                        const auto final_flags = state.flags ^ (invert ? flags::Carry : 0);
                        const bool n = flags::GetSF(final_flags), z = flags::GetZF(final_flags),
                                   c = flags::GetCF(final_flags), v = flags::GetOF(final_flags);
                        const bool expected[]{z,
                                              !z,
                                              c,
                                              !c,
                                              n,
                                              !n,
                                              v,
                                              !v,
                                              c && !z,
                                              !c || z,
                                              n == v,
                                              n != v,
                                              !z && n == v,
                                              z || n != v,
                                              true,
                                              true};
                        if (Run(code, state, expected[condition]))
                            Check(state.flags == final_flags,
                                  "native conditions preserve all portable flags");
                    }
                }
            }
        }
        ArithmeticFlags();
        ArithmeticFlagBoundaries();
        ImmediateOperandBoundaries();
        NarrowReadsAndHelperABI();
        UniformStoreViews();
        ImmediateShiftBoundaries();
        ArmFPBinaryBoundaries();
        ArmFPUnalignedState();
        MemoryHomeOperands();
        TerminalFlagConditions();
        FunctionLoop();
        LocalControlResults();
        HelperABI();
        AtomicOperations();
        FlatExpressionShifts();
    }
    test_lahf = false;
}

TEST_CASE("X64 fault recipes merge native and saved NZCV", "[compile][x64-compile]") {
    constexpr unsigned source_bits[]{0, 11, 6, 7}, destination_bits[]{29, 28, 30, 31};
    for (unsigned image : {0u, 1u})
        for (unsigned mask = 0; mask < 16; ++mask)
            for (unsigned pattern = 0; pattern < 16; ++pattern)
                for (unsigned polarity = 0; polarity < 4; ++polarity) {
                    State state;
                    swift::runtime::execution::FaultContext context;
                    context.state = &state;
                    context.flags_offset = offsetof(State, flags);
                    context.pinned_uniform_offset = UINT32_MAX;
                    const unsigned image_bits[]{8, 0, 14, 15};
                    std::uint64_t saved = image ? UINT64_C(0xdeadbeefcafe0000) : 0x202,
                                  native = 0x202;
                    for (unsigned bit = 0; bit < 4; ++bit) {
                        native |= std::uint64_t{(pattern >> bit) & 1} << source_bits[bit];
                        saved |= std::uint64_t{((pattern ^ 9) >> bit) & 1}
                                 << (image ? image_bits[bit] : source_bits[bit]);
                    }
                    std::array<std::uint64_t, 16> registers{};
                    registers[3] =
                            (std::uint64_t{1} << 55) | flags::Direction | flags::Auxiliary | 0x25;
                    registers[4] = reinterpret_cast<std::uintptr_t>(&saved);
                    swift::runtime::backend::FaultSite site{};
                    site.recovery[0] = 2 | (polarity << 2) | (image << 4);
                    site.recovery[1] = mask | ((15 ^ mask) << 4);
                    auto expected = registers[3];
                    for (unsigned bit = 0; bit < 4; ++bit) {
                        const bool use_native = mask & (1u << bit);
                        auto value = ((use_native ? native : saved) >>
                                      (image && !use_native ? image_bits[bit] : source_bits[bit])) &
                                     1;
                        if (!bit) value ^= (polarity >> (use_native ? 0 : 1)) & 1;
                        expected |= value << destination_bits[bit];
                    }
                    CHECK(x64::RestoreGuestState(
                            context, site, std::as_bytes(std::span{registers}), native));
                    CHECK(state.flags == expected);
                    site.recovery[1] = 0x11;
                    CHECK_FALSE(x64::RestoreGuestState(
                            context, site, std::as_bytes(std::span{registers}), native));
                }
}

TEST_CASE("Partial native NZCV preserves every omitted bit and condition",
          "[compile][x64-compile][native]") {
    for (const bool image : {false, true})
        for (unsigned selected = 1; selected < 15; ++selected)
            for (unsigned condition = 0; condition < 14; ++condition) {
                INFO("image=" << image << " selected=" << selected << " condition=" << condition);
                Builder builder{Scope::Block};
                Begin(builder);
                Emit(builder, OpCode::FaultLocation, Type::Void, {Imm(0x1000)});
                const auto lhs = Load(builder, 0), rhs = Load(builder, 1);
                Emit(builder,
                     OpCode::Sub,
                     Type::U64,
                     {Operand::ValueOf(lhs), Operand::ValueOf(rhs)},
                     {selected, FlagProfile::ArithmeticRaw});
                const auto address = Load(builder, 3);
                Emit(builder, OpCode::LoadMemory, Type::U64, {Operand::ValueOf(address)});
                const auto predicate =
                        Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(condition)});
                auto options = Options();
                options.local_flags = true;
                options.lahf = image;
                const auto code = x64::Compile(Return(builder, predicate), options);
                REQUIRE(code);
                REQUIRE(code.faults.size() == 1);
                CHECK((code.faults.front().recovery[1] & 15) == selected);
                CHECK((code.faults.front().recovery[1] >> 4) == (15 ^ selected));
                if (!NativeMode()) continue;
                swift::tests::NativeCode native{code.words, swift::tests::NativeISA::X64};
                for (unsigned pattern = 0; pattern < 16; ++pattern)
                    for (const auto pair :
                         {Pair{0, 1}, Pair{1, 0}, Pair{7, 7}, Pair{u64{1} << 63, 1}}) {
                        State state;
                        state.scalars[0] = pair.low;
                        state.scalars[1] = pair.high;
                        state.scalars[3] = reinterpret_cast<std::uintptr_t>(&state.scalars[4]);
                        state.flags = (u64{1} << 55) | flags::Direction | flags::Auxiliary | 0xa5 |
                                      (u64{pattern} << 28);
                        const auto result = pair.low - pair.high;
                        const bool carry = pair.low >= pair.high;
                        const bool overflow = ((pair.low ^ pair.high) & (pair.low ^ result)) >> 63;
                        const u64 produced =
                                (carry ? flags::Carry : 0) | (overflow ? flags::Overflow : 0) |
                                (!result ? flags::Zero : 0) | ((result >> 63) ? flags::Sign : 0);
                        const u64 mask = ((selected & 1) ? flags::Carry : 0) |
                                         ((selected & 2) ? flags::Overflow : 0) |
                                         ((selected & 4) ? flags::Zero : 0) |
                                         ((selected & 8) ? flags::Sign : 0);
                        const auto expected = (state.flags & ~mask) | (produced & mask);
                        const bool n = flags::GetSF(expected), z = flags::GetZF(expected),
                                   c = flags::GetCF(expected), v = flags::GetOF(expected);
                        const bool predicates[]{z,
                                                !z,
                                                c,
                                                !c,
                                                n,
                                                !n,
                                                v,
                                                !v,
                                                c && !z,
                                                !c || z,
                                                n == v,
                                                n != v,
                                                !z && n == v,
                                                z || n != v};
                        CHECK(native.Run(reinterpret_cast<std::uint8_t*>(&state)) ==
                              predicates[condition]);
                        CHECK(state.flags == expected);
                    }
            }
}

TEST_CASE("Commutative integer results reuse a last-use right operand",
          "[compile][x64-compile][native]") {
    for (const auto type : {Type::U32, Type::U64})
        for (const auto opcode : {OpCode::Add, OpCode::And, OpCode::Or, OpCode::Xor}) {
            Builder builder{Scope::Block};
            Begin(builder);
            const auto load = [&](unsigned slot) {
                return Emit(builder,
                            OpCode::LoadUniform,
                            type,
                            {Operand::Uniform(offsetof(State, scalars) + slot * 8, type)});
            };
            const auto left = load(0), right = load(1);
            const auto result =
                    Emit(builder, opcode, type, {Operand::ValueOf(left), Operand::ValueOf(right)});
            Store(builder, 2, result);
            Store(builder, 3, left);
            const auto program = Return(builder, result);
            swift::runtime::backend::CompileResult code;
            x64::Allocation allocation;
            x64::Compile(program, Options(), code, allocation);
            REQUIRE(code);
            CHECK(allocation.homes[result.node.value].reuse_rhs);
            CHECK(allocation.homes[result.node.value].reg ==
                  allocation.homes[right.node.value].reg);
            for (const auto pair :
                 {Pair{7, 9}, Pair{UINT64_MAX, 1}, Pair{0x12345678ffffffff, 0xfedcba9800000001}}) {
                State state;
                state.scalars[0] = pair.low;
                state.scalars[1] = pair.high;
                state.flags = (u64{1} << 55) | 0x900000a5;
                const u64 mask = type == Type::U32 ? 0xffffffff : UINT64_MAX;
                const auto expected = (opcode == OpCode::Add   ? pair.low + pair.high
                                       : opcode == OpCode::And ? pair.low & pair.high
                                       : opcode == OpCode::Or  ? pair.low | pair.high
                                                               : pair.low ^ pair.high) &
                                      mask;
                if (Run(code, state, expected)) {
                    CHECK(state.scalars[2] == expected);
                    CHECK(state.scalars[3] == (pair.low & mask));
                    CHECK(state.flags == ((u64{1} << 55) | 0x900000a5));
                }
            }
        }
}

TEST_CASE("Regional FP slow calls preserve a loop with dead incoming flags",
          "[compile][x64-compile][native]") {
    Builder builder{Scope::Function};
    const auto loop = Block(builder), exit = Block(builder);
    Need(builder.BeginBlock(loop));
    const auto left = LoadVector(builder, 0), right = LoadVector(builder, 1);
    const auto sum = Emit(builder,
                          OpCode::ArmFPBinary,
                          Type::V128,
                          {Operand::ValueOf(left),
                           Operand::ValueOf(right),
                           Imm(offsetof(State, fpcr)),
                           Imm(offsetof(State, fpsr)),
                           Imm(swift::runtime::arm64::ArmFPBinaryMode(0, 64, 1))});
    StoreVector(builder, 0, sum);
    const auto count = Load(builder, 0);
    const auto next = Emit(builder,
                           OpCode::Sub,
                           Type::U64,
                           {Operand::ValueOf(count), Imm(1)},
                           {15, FlagProfile::ArithmeticCanonicalCarry, false});
    Store(builder, 0, next);
    const auto again =
            Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::NE)});
    Need(builder.EndBlock(Terminal::Branch(again, loop, exit)));
    Need(builder.BeginBlock(exit));
    const auto program = Return(builder, Literal(builder, 0));
    auto options = Options();
    options.local_flags = true;
    options.arm_fpcr = 0;
    options.arm_fpcr_offset = offsetof(State, fpcr);
    x64::Allocation allocation;
    swift::runtime::backend::CompileResult code;
    x64::Compile(program, options, code, allocation);
    REQUIRE(bool(code));
    CHECK(allocation.flags_input[loop.id.value] == 0);
    for (auto input : {0x3ff0000000000000ULL, 0x7ff0000000000042ULL}) {
        State state;
        state.flags = (u64{1} << 55) | flags::Direction | flags::Auxiliary | 0xa5;
        const auto base = state.flags;
        state.scalars[0] = 3;
        state.vectors[0] = {input, UINT64_MAX};
        state.vectors[1] = {0x3ff8000000000000, UINT64_MAX};
        u64 expected = input;
        unsigned raised{};
        for (unsigned i = 0; i < 3; ++i) {
            const auto result = swift::runtime::arm64::EvaluateBinaryFP(
                    expected, state.vectors[1].low, 64, 2, 0);
            expected = result.value;
            raised |= result.exceptions;
        }
        if (Run(code, state, 0)) {
            CHECK(state.vectors[0] == (Pair{expected, 0}));
            CHECK(state.fpsr == raised);
            CHECK(state.flags == (base | flags::Zero));
            CHECK(state.scalars[0] == 0);
        }
    }
}

TEST_CASE("Specialized FP rounding clears inactive packed subtraction lanes",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    for (unsigned profile = 0; profile < 32; ++profile) {
        Builder builder{Scope::Block};
        Begin(builder);
        const auto left = LoadVector(builder, 0), right = LoadVector(builder, 1);
        const auto result = Emit(builder,
                                 OpCode::ArmFPBinary,
                                 Type::V128,
                                 {Operand::ValueOf(left),
                                  Operand::ValueOf(right),
                                  Imm(offsetof(State, fpcr)),
                                  Imm(offsetof(State, fpsr)),
                                  Imm(arm::ArmFPBinaryMode(1, 32, 2))});
        StoreVector(builder, 0, result);
        auto options = Options();
        options.local_flags = true;
        options.arm_fpcr = arm::FPCRForDomain(profile);
        options.arm_fpcr_offset = offsetof(State, fpcr);
        const auto code = x64::Compile(Return(builder), options);
        REQUIRE(bool(code));
        State state;
        state.fpcr = options.arm_fpcr;
        state.vectors[0] = Lanes32(0x3f800000, 0x40000000, 0x7f800001, 0x7fc00042);
        state.vectors[1] = state.vectors[0];
        const auto expected = (state.fpcr & (3u << 22)) == (2u << 22)
                                      ? Lanes32(0x80000000, 0x80000000, 0, 0)
                                      : Pair{};
        if (Run(code, state)) {
            CHECK(state.vectors[0] == expected);
            CHECK(state.fpsr == 0);
        }
    }
}

namespace {
u64 ChangeFPCR(u64 address) {
    reinterpret_cast<State*>(address)->fpcr = 1u << 22;
    return 0;
}
}  // namespace

TEST_CASE("Specialized FP uses exact leaves after explicit and opaque control writes",
          "[compile][x64-compile][native]") {
    for (bool helper : {false, true}) {
        Builder builder{Scope::Block};
        Begin(builder);
        if (helper) {
            const auto address = Emit(builder, OpCode::GetUniformAddress, Type::U64, {Imm(0)});
            Emit(builder,
                 OpCode::CallLambda,
                 Type::U64,
                 {Operand::Lambda(),
                  Imm(reinterpret_cast<std::uintptr_t>(&ChangeFPCR)),
                  Operand::ValueOf(address)});
        } else {
            const auto control = Literal(builder, 1u << 22, Type::U32);
            Emit(builder,
                 OpCode::StoreUniform,
                 Type::Void,
                 {Operand::Uniform(offsetof(State, fpcr), Type::U32), Operand::ValueOf(control)});
        }
        Emit(builder, OpCode::FaultLocation, Type::Void, {Imm(0x1234)});
        const auto left = LoadVector(builder, 0), right = LoadVector(builder, 1);
        const auto result = Emit(builder,
                                 OpCode::ArmFPBinary,
                                 Type::V128,
                                 {Operand::ValueOf(left),
                                  Operand::ValueOf(right),
                                  Imm(offsetof(State, fpcr)),
                                  Imm(offsetof(State, fpsr)),
                                  Imm(swift::runtime::arm64::ArmFPBinaryMode(0, 64, 1))});
        StoreVector(builder, 2, result);
        auto options = Options();
        options.local_flags = true;
        options.arm_fpcr = 0;
        options.arm_fpcr_offset = offsetof(State, fpcr);
        const auto code = x64::Compile(Return(builder), options);
        REQUIRE(bool(code));
        State state;
        state.vectors[0] = {0x3ff0000000000000, 0};
        state.vectors[1] = {0x3ca0000000000000, 0};
        state.vectors[2] = {0x12345678, 0x87654321};
        const auto expected = swift::runtime::arm64::EvaluateBinaryFP(
                state.vectors[0].low, state.vectors[1].low, 64, 2, 1u << 22);
        if (Run(code, state)) {
            CHECK(state.fpcr == (1u << 22));
            CHECK(state.vectors[2] == (Pair{expected.value, 0}));
            CHECK(state.fpsr == expected.exceptions);
        }
    }
}

TEST_CASE("Empty IR markers preserve the native condition carrier",
          "[compile][x64-compile][native]") {
    std::vector<std::uint32_t> reference;
    for (auto marker : {OpCode::Count,
                        OpCode::Nop,
                        OpCode::UniformBarrier,
                        OpCode::XchgBarrier,
                        OpCode::BranchOnlyEdges}) {
        Builder builder{Scope::Block};
        Begin(builder);
        const auto left = Load(builder, 0), right = Load(builder, 1);
        Emit(builder,
             OpCode::Add,
             Type::U64,
             {Operand::ValueOf(left), Operand::ValueOf(right)},
             {15, FlagProfile::ArithmeticCanonicalCarry, false});
        if (marker == OpCode::XchgBarrier) {
            Emit(builder, marker, Type::Void, {Imm(0)});
        } else if (marker != OpCode::Count) {
            Emit(builder, marker, Type::Void, {});
        }
        const auto condition =
                Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::EQ)});
        auto options = Options();
        options.local_flags = true;
        const auto code = x64::Compile(Return(builder, condition), options);
        REQUIRE(bool(code));
        if (reference.empty()) reference = code.words;
        CHECK(code.words == reference);
        State state;
        state.scalars[0] = 7;
        state.scalars[1] = 9;
        state.flags = (u64{1} << 55) | 0x25;
        if (Run(code, state, 0)) CHECK(state.flags == ((u64{1} << 55) | 0x25));
    }
}

TEST_CASE("Uniform flags writes preserve every untouched byte", "[compile][x64-compile][native]") {
    for (bool local : {false, true}) {
        for (const auto [offset, type] : {std::pair{0u, Type::U8},
                                          std::pair{1u, Type::U16},
                                          std::pair{3u, Type::U8},
                                          std::pair{3u, Type::U32},
                                          std::pair{4u, Type::U32},
                                          std::pair{0u, Type::U64}}) {
            Builder builder{Scope::Block};
            Begin(builder);
            const auto left = Load(builder, 0), right = Load(builder, 1);
            Emit(builder,
                 OpCode::Add,
                 Type::U64,
                 {Operand::ValueOf(left), Operand::ValueOf(right)},
                 {15, FlagProfile::ArithmeticCanonicalCarry, false});
            constexpr u64 replacement = 0x81234567feed23a1;
            const auto value = Literal(builder, replacement);
            Emit(builder,
                 OpCode::StoreUniform,
                 Type::Void,
                 {Operand::Uniform(offsetof(State, flags) + offset, type),
                  Operand::ValueOf(value)});
            const auto result = Emit(builder,
                                     OpCode::LoadUniform,
                                     Type::U64,
                                     {Operand::Uniform(offsetof(State, flags), Type::U64)});
            auto options = Options();
            options.local_flags = local;
            const auto code = x64::Compile(Return(builder, result), options);
            REQUIRE(bool(code));
            State state;
            state.scalars[0] = 7;
            state.scalars[1] = 9;
            state.flags = (u64{1} << 55) | 0xf0000025;
            const auto mask = (UINT64_MAX >> (64 - TypeBits(type))) << (offset * 8);
            const auto expected =
                    (((u64{1} << 55) | 0x25) & ~mask) | ((replacement << (offset * 8)) & mask);
            if (Run(code, state, expected)) CHECK(state.flags == expected);
        }
    }
}

TEST_CASE("Zero-count arithmetic shifts keep normalized U32 homes",
          "[compile][x64-compile][native]") {
    Builder builder{Scope::Block};
    Begin(builder);
    const auto input = Load(builder, 0);
    const auto shifted =
            Emit(builder, OpCode::AsrImm, Type::U32, {Operand::ValueOf(input, Type::U32), Imm(32)});
    const auto wide = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(shifted)});
    const auto code = x64::Compile(Return(builder, wide), Options());
    REQUIRE(bool(code));
    State state;
    state.scalars[0] = 0x1234567880000000;
    Run(code, state, 0x80000000);
}

TEST_CASE("Full flags ADC consumes the current native carry", "[compile][x64-compile][native]") {
    for (bool local : {false, true}) {
        Builder builder{Scope::Block};
        Begin(builder);
        const auto left = Literal(builder, UINT64_MAX), one = Literal(builder, 1);
        Emit(builder,
             OpCode::Add,
             Type::U64,
             {Operand::ValueOf(left), Operand::ValueOf(one)},
             {15, FlagProfile::ArithmeticCanonicalCarry, false});
        const auto zero = Literal(builder, 0);
        const auto result =
                Emit(builder,
                     OpCode::Adc,
                     Type::U64,
                     {Operand::ValueOf(zero), Operand::ValueOf(zero)},
                     {FlagBits(Flag::All), FlagProfile::ArithmeticCanonicalCarry, false});
        auto options = Options();
        options.local_flags = local;
        const auto code = x64::Compile(Return(builder, result), options);
        REQUIRE(bool(code));
        State state;
        Run(code, state, 1);
    }
}

TEST_CASE("Native ARM MXCSR status and result checks preserve exact state",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    bool avx_available = false;
#if defined(__x86_64__) && defined(__linux__)
    avx_available = __builtin_cpu_supports("avx");
#endif
    constexpr std::array<std::array<u64, 2>, 8> single{{{0x3f800000, 0x40000000},
                                                        {0x00800000, 0x3f7fffff},
                                                        {0x007fffff, 0x3f800000},
                                                        {1, 0x3f000000},
                                                        {0x7f800001, 0x7fc01234},
                                                        {0x7f800000, 0},
                                                        {0x80000000, 0},
                                                        {0x7f7fffff, 0x3f800001}}};
    constexpr std::array<std::array<u64, 2>, 8> double_precision{
            {{0x3ff0000000000000, 0x4000000000000000},
             {0x0010000000000000, 0x3fefffffffffffff},
             {0x000fffffffffffff, 0x3ff0000000000000},
             {1, 0x3fe0000000000000},
             {0x7ff0000000000001, 0x7ff8123456789abc},
             {0x7ff0000000000000, 0},
             {0x8000000000000000, 0},
             {0x7fefffffffffffff, 0x3ff0000000000001}}};
    for (unsigned feature = 0; feature < 3; ++feature) {
        if (feature == 2 && NativeMode() && !avx_available) continue;
        for (unsigned domain = 0; domain < 32; ++domain) {
            for (unsigned bits : {32u, 64u}) {
                for (unsigned lanes : {1u, 2u, 4u}) {
                    if (bits * lanes > 128) continue;
                    for (unsigned kind = 0; kind < 4; ++kind) {
                        if (kind == 3 && lanes != 1) continue;
                        Builder builder{Scope::Block};
                        Begin(builder);
                        const auto a = Literal(builder, UINT64_MAX), b = Literal(builder, 1);
                        Emit(builder,
                             OpCode::Add,
                             Type::U64,
                             {Operand::ValueOf(a), Operand::ValueOf(b)},
                             {5, FlagProfile::ArithmeticCanonicalCarry, false});
                        const auto left = LoadVector(builder, 0), right = LoadVector(builder, 1);
                        const auto result = Emit(builder,
                                                 OpCode::ArmFPBinary,
                                                 Type::V128,
                                                 {Operand::ValueOf(left),
                                                  Operand::ValueOf(right),
                                                  Imm(offsetof(State, fpcr)),
                                                  Imm(offsetof(State, fpsr)),
                                                  Imm(arm::ArmFPBinaryMode(kind, bits, lanes))});
                        StoreVector(builder, 2, result);
                        const auto status =
                                Emit(builder,
                                     OpCode::LoadUniform,
                                     Type::U32,
                                     {Operand::Uniform(offsetof(State, fpsr), Type::U32)});
                        Store(builder, 4, status);
                        const auto condition = Emit(builder,
                                                    OpCode::CondSet,
                                                    Type::Bool,
                                                    {Operand::Condition(Condition::EQ)});
                        auto options = Options();
                        options.local_flags = true;
                        options.sse41 = feature == 1;
                        options.avx = feature == 2;
                        options.arm_fpcr = arm::FPCRForDomain(domain);
                        options.arm_fpcr_offset = offsetof(State, fpcr);
                        options.arm_fpsr_offset = offsetof(State, fpsr);
                        auto code = x64::Compile(Return(builder, condition), options);
                        REQUIRE(bool(code));
                        if (feature == 2) {
                            // Exercise RIP-relative AVX constants at an unaligned code base.
                            std::vector<u32> shifted(code.words.size() + 1, 0x90909090);
                            std::memcpy(reinterpret_cast<std::uint8_t*>(shifted.data()) + 1,
                                        code.words.data(),
                                        code.words.size() * sizeof(u32));
                            code.words = std::move(shifted);
                        }
                        const auto& inputs = bits == 32 ? single : double_precision;
                        for (const auto& pair : inputs) {
                            State state;
                            state.fpcr = options.arm_fpcr;
                            state.fpsr = 0x08000000;
                            state.flags = (u64{1} << 55) | 0x25;
                            state.vectors[0] =
                                    state.vectors[1] = {0x7f8000017f800001, 0x7ff0000000000001};
                            for (unsigned lane = 0; lane < lanes; ++lane) {
                                std::memcpy(reinterpret_cast<std::uint8_t*>(&state.vectors[0]) +
                                                    lane * bits / 8,
                                            &pair[0],
                                            bits / 8);
                                std::memcpy(reinterpret_cast<std::uint8_t*>(&state.vectors[1]) +
                                                    lane * bits / 8,
                                            &pair[1],
                                            bits / 8);
                            }
                            constexpr unsigned operations[]{2, 3, 0, 1};
                            const auto expected = arm::EvaluateBinaryFP(
                                    pair[0], pair[1], bits, operations[kind], state.fpcr);
                            Pair expected_vector{};
                            for (unsigned lane = 0; lane < lanes; ++lane)
                                std::memcpy(reinterpret_cast<std::uint8_t*>(&expected_vector) +
                                                    lane * bits / 8,
                                            &expected.value,
                                            bits / 8);
                            if (Run(code, state, 1)) {
                                CHECK(state.vectors[2] == expected_vector);
                                CHECK(state.fpsr == (0x08000000 | expected.exceptions));
                                CHECK(state.scalars[4] == state.fpsr);
                                CHECK(state.flags == ((u64{1} << 55) | 0x60000025));
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("Exact FP comparison conditions retain unordered and exception semantics",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    using namespace vixl::aarch64;
    for (const bool image : {false, true})
        for (const unsigned domain : {0u, 8u, 16u, 31u})
            for (const unsigned bits : {32u, 64u})
                for (const bool signaling : {false, true})
                    for (unsigned condition = 0; condition < 14; ++condition) {
                        const u32 word = (bits == 32 ? u32(FCMP_s) : u32(FCMP_d)) | (1u << 5) |
                                         (2u << 16) | (signaling ? 16u : 0u);
                        Builder builder{Scope::Block};
                        Begin(builder);
                        const auto lhs = LoadVector(builder, 0), rhs = LoadVector(builder, 1);
                        Emit(builder,
                             OpCode::ArmFPCompare,
                             Type::Void,
                             {Operand::ValueOf(lhs),
                              Operand::ValueOf(rhs),
                              Imm(arm::FPStateOffsets(offsetof(State, fpcr),
                                                      offsetof(State, fpsr))),
                              Imm(arm::NumericCall(arm::Numeric::Scalar, word))});
                        const auto predicate = Emit(builder,
                                                    OpCode::CondSet,
                                                    Type::Bool,
                                                    {Operand::Condition(condition)});
                        auto options = Options();
                        options.local_flags = true;
                        options.lahf = image;
                        options.arm_fpcr = arm::FPCRForDomain(domain);
                        options.arm_fpcr_offset = offsetof(State, fpcr);
                        options.arm_fpsr_offset = offsetof(State, fpsr);
                        const auto code = x64::Compile(Return(builder, predicate), options);
                        REQUIRE(code);
                        if (!NativeMode()) continue;
                        swift::tests::NativeCode native{code.words, swift::tests::NativeISA::X64};
                        const u64 one = bits == 32 ? 0x3f800000 : 0x3ff0000000000000,
                                  nan = bits == 32 ? 0x7fc00042 : 0x7ff8000000000042,
                                  snan = bits == 32 ? 0x7f800042 : 0x7ff0000000000042,
                                  negative = u64{1} << (bits - 1);
                        for (const auto pair : {Pair{0, 0},
                                                Pair{one, 0},
                                                Pair{0, one},
                                                Pair{nan, one},
                                                Pair{one, snan},
                                                Pair{1, 0},
                                                Pair{negative, 0},
                                                Pair{one | negative, one}}) {
                            State state;
                            state.fpcr = options.arm_fpcr;
                            state.fpsr = 0x08000000;
                            state.flags = (u64{1} << 55) | 0x900000a5;
                            state.vectors[0] = {pair.low, UINT64_MAX};
                            state.vectors[1] = {pair.high, UINT64_MAX};
                            arm::GuestState expected;
                            expected.fpcr = state.fpcr;
                            expected.fpsr = state.fpsr;
                            std::memcpy(expected.q[1], &pair.low, bits / 8);
                            std::memcpy(expected.q[2], &pair.high, bits / 8);
                            const auto nzcv = arm::ExecuteNumeric(
                                    reinterpret_cast<u64>(&expected),
                                    arm::NumericCall(arm::Numeric::Scalar, word),
                                    0);
                            const bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
                            const bool predicates[]{z,
                                                    !z,
                                                    c,
                                                    !c,
                                                    n,
                                                    !n,
                                                    v,
                                                    !v,
                                                    c && !z,
                                                    !c || z,
                                                    n == v,
                                                    n != v,
                                                    !z && n == v,
                                                    z || n != v};
                            CHECK(native.Run(reinterpret_cast<std::uint8_t*>(&state)) ==
                                  predicates[condition]);
                            CHECK(state.fpsr == expected.fpsr);
                            CHECK(state.flags == ((u64{1} << 55) | 0xa5 | (nzcv << 28)));
                        }
                    }
}

TEST_CASE("FP bit operations specialize controls and guard a bit-only unit",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    using namespace vixl::aarch64;
    for (const unsigned precision : {0u, 1u, 3u})
        for (const u32 operation : {u32(FMOV_s), u32(FABS_s), u32(FNEG_s)})
            for (const bool specialized : {false, true}) {
                const u32 word = operation | (precision << 22) | (1u << 5);
                Builder builder{Scope::Block};
                Begin(builder);
                const auto input = LoadVector(builder, 0);
                const auto result =
                        Emit(builder,
                             OpCode::ArmFPBitUnary,
                             Type::V128,
                             {Operand::ValueOf(input), Imm(offsetof(State, fpcr)), Imm(word)});
                StoreVector(builder, 1, result);
                auto options = Options();
                options.local_flags = true;
                options.arm_fpcr_offset = offsetof(State, fpcr);
                if (specialized) options.arm_fpcr = 0;
                const auto code = x64::Compile(Return(builder), options);
                REQUIRE(code);
                if (!NativeMode()) continue;
                swift::tests::NativeCode native{code.words, swift::tests::NativeISA::X64};
                for (const u32 fpcr : {0u, 2u})
                    for (const u64 bits : {UINT64_C(0xfff0000000000042),
                                           UINT64_C(0x7ff8000000000042),
                                           UINT64_MAX,
                                           UINT64_C(0x8000000080008000)}) {
                        State state;
                        state.fpcr = fpcr;
                        state.fpsr = 0x08000010;
                        state.flags = (u64{1} << 55) | 0x900000a5;
                        state.vectors[0] = {bits, UINT64_MAX};
                        state.vectors[1] = {UINT64_MAX, UINT64_MAX};
                        const auto answer = native.Run(reinterpret_cast<std::uint8_t*>(&state));
                        if (specialized && fpcr) {
                            CHECK(answer == swift::runtime::ExitValue(
                                                    swift::runtime::NativeExit::FPEnvironment));
                            CHECK(state.vectors[1] == (Pair{UINT64_MAX, UINT64_MAX}));
                        } else {
                            CHECK(answer == 0);
                            CHECK(state.vectors[1] ==
                                  (Pair{arm::EvaluateFPBitUnary(bits, word, fpcr), 0}));
                        }
                        CHECK(state.fpsr == 0x08000010);
                        CHECK(state.flags == ((u64{1} << 55) | 0x900000a5));
                    }
            }
}

TEST_CASE("ARM numerical lowering matches exact leaves in every FPCR domain",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    using namespace vixl::aarch64;
    const std::array scalar{
            u32(FCMP_s),      u32(FCMP_d),    u32(FCMPE_s),   u32(FCMPE_d),   u32(FCMP_s_zero),
            u32(FCMP_d_zero), u32(FCCMP_s),   u32(FCCMPE_d),  u32(FMADD_s),   u32(FMSUB_s),
            u32(FNMADD_s),    u32(FNMSUB_s),  u32(FMADD_d),   u32(FMSUB_d),   u32(FNMADD_d),
            u32(FNMSUB_d),    u32(SCVTF_sw),  u32(SCVTF_sx),  u32(SCVTF_dw),  u32(SCVTF_dx),
            u32(UCVTF_sw),    u32(UCVTF_sx),  u32(UCVTF_dw),  u32(UCVTF_dx),  u32(FCVTZS_ws),
            u32(FCVTZS_xs),   u32(FCVTZS_wd), u32(FCVTZS_xd), u32(FCVTZU_ws), u32(FCVTZU_xs),
            u32(FCVTZU_wd),   u32(FCVTZU_xd), u32(FDIV_s),    u32(FDIV_d),    u32(FSQRT_s),
            u32(FSQRT_d),     u32(FCVT_ds),   u32(FCVT_sd)};
    std::vector<std::pair<arm::Numeric, u32>> instructions;
    for (const auto opcode : scalar) {
        const bool fused = (opcode & FPDataProcessing3SourceFMask) == FPDataProcessing3SourceFixed;
        const bool binary =
                (opcode & FPDataProcessing2SourceFMask) == FPDataProcessing2SourceFixed ||
                (opcode & FPCompareFMask) == FPCompareFixed ||
                (opcode & FPConditionalCompareFMask) == FPConditionalCompareFixed;
        instructions.emplace_back(
                arm::Numeric::Scalar,
                opcode | (1u << 5) | (fused || binary ? 2u << 16 : 0) | (fused ? 3u << 10 : 0));
    }
    for (const u32 opcode : {u32(NEON_FMLA), u32(NEON_FMLS)})
        for (const u32 shape : {0u, u32(NEON_Q), u32(NEON_Q) | (1u << 22)})
            instructions.emplace_back(arm::Numeric::Fused, opcode | shape | (1u << 5) | (2u << 16));
    for (const u32 opcode : {u32(NEON_FMLA_H_byelement), u32(NEON_FMLS_H_byelement)})
        for (const u32 shape : {2u << 22,
                                (2u << 22) | u32(NEON_Q),
                                (3u << 22) | u32(NEON_Q),
                                (2u << 22) | (u32(NEON_Q) | u32(NEONScalar)),
                                (3u << 22) | (u32(NEON_Q) | u32(NEONScalar))})
            instructions.emplace_back(arm::Numeric::Indexed,
                                      opcode | shape | (1u << 5) | (2u << 16));
    const std::array<u64, 18> singles{0,
                                      0x80000000,
                                      0x3f800000,
                                      0xbf800000,
                                      0x3fc00000,
                                      1,
                                      0x007fffff,
                                      0x00800000,
                                      0x00800001,
                                      0x7f7fffff,
                                      0x7f800000,
                                      0xff800000,
                                      0x7fc00001,
                                      0x7f800001,
                                      0x4f000000,
                                      0x4f800000,
                                      0x5f000000,
                                      0x5f800000};
    const std::array<u64, 18> doubles{0,
                                      UINT64_C(0x8000000000000000),
                                      UINT64_C(0x3ff0000000000000),
                                      UINT64_C(0xbff0000000000000),
                                      UINT64_C(0x3ff8000000000000),
                                      1,
                                      UINT64_C(0x000fffffffffffff),
                                      UINT64_C(0x0010000000000000),
                                      UINT64_C(0x0010000000000001),
                                      UINT64_C(0x7fefffffffffffff),
                                      UINT64_C(0x7ff0000000000000),
                                      UINT64_C(0xfff0000000000000),
                                      UINT64_C(0x7ff8000000000001),
                                      UINT64_C(0x7ff0000000000001),
                                      UINT64_C(0x41e0000000000000),
                                      UINT64_C(0x41f0000000000000),
                                      UINT64_C(0x43e0000000000000),
                                      UINT64_C(0x43f0000000000000)};
    bool fma = false, sse41 = false;
#if defined(__x86_64__) && defined(__linux__)
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        sse41 = c & (1u << 19);
        if ((c & (1u << 12)) && (c & (1u << 27)) && (c & (1u << 28))) {
            unsigned lo, hi;
            asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            fma = (lo & 6) == 6;
        }
    }
#endif
    for (unsigned domain = 0; domain < 32; ++domain) {
        for (const bool typed : {false, true})
            for (const auto [family, word] : instructions) {
                INFO("domain=" << domain << " family=" << unsigned(family) << " word=" << std::hex
                               << word);
                REQUIRE(arm::SupportsNumeric(family, word));
                Builder builder{Scope::Block};
                Begin(builder);
                const auto encoded = arm::NumericCall(family, word);
                Value result;
                if (!typed) {
                    const auto pointer =
                            Emit(builder, OpCode::GetUniformAddress, Type::U64, {Imm(0)});
                    const auto instruction = Literal(builder, encoded);
                    const auto location = Literal(builder, 0x1000);
                    result = Emit(builder,
                                  OpCode::CallLambda,
                                  Type::U64,
                                  {Operand::Lambda(),
                                   Imm(reinterpret_cast<std::uintptr_t>(&arm::ExecuteNumeric)),
                                   Operand::ValueOf(pointer),
                                   Operand::ValueOf(instruction),
                                   Operand::ValueOf(location)});
                } else {
                    const auto info = arm::DescribeFPValues(encoded);
                    const u32 rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
                    const auto vector = [&](unsigned reg) {
                        return Emit(builder,
                                    OpCode::LoadUniform,
                                    Type::V128,
                                    {Operand::Uniform(offsetof(arm::GuestState, q) + reg * 16,
                                                      Type::V128)});
                    };
                    const auto offsets = Imm(arm::FPStateOffsets(offsetof(arm::GuestState, fpcr),
                                                                 offsetof(arm::GuestState, fpsr)));
                    const auto mode = Imm(encoded);
                    const auto lhs = info.kind == arm::FPValueKind::FromInt
                                             ? Emit(builder,
                                                    OpCode::LoadUniform,
                                                    Type::U64,
                                                    {Operand::Uniform(rn * 8, Type::U64)})
                                             : vector(rn);
                    Value value;
                    if (info.kind == arm::FPValueKind::Compare) {
                        Emit(builder,
                             OpCode::ArmFPCompare,
                             Type::Void,
                             {Operand::ValueOf(lhs), Operand::ValueOf(vector(rm)), offsets, mode});
                    } else if (info.kind == arm::FPValueKind::Fused) {
                        const auto addend = family == arm::Numeric::Scalar ? (word >> 10) & 31 : rd;
                        value = Emit(builder,
                                     OpCode::ArmFPTernary,
                                     Type::V128,
                                     {Operand::ValueOf(lhs),
                                      Operand::ValueOf(vector(rm)),
                                      Operand::ValueOf(vector(addend)),
                                      offsets,
                                      mode});
                    } else if (info.kind == arm::FPValueKind::Divide) {
                        value = Emit(builder,
                                     OpCode::ArmFPBinary,
                                     Type::V128,
                                     {Operand::ValueOf(lhs),
                                      Operand::ValueOf(vector(rm)),
                                      Imm(offsetof(arm::GuestState, fpcr)),
                                      Imm(offsetof(arm::GuestState, fpsr)),
                                      Imm(arm::ArmFPBinaryMode(3, info.bits, 1))});
                    } else {
                        const bool integer = info.kind == arm::FPValueKind::ToInt;
                        const bool convert = integer || info.kind == arm::FPValueKind::FromInt;
                        value = Emit(builder,
                                     convert ? OpCode::ArmFPConvert : OpCode::ArmFPUnary,
                                     integer ? Type::U64 : Type::V128,
                                     {Operand::ValueOf(lhs), offsets, mode});
                    }
                    if (value) {
                        const bool integer = info.kind == arm::FPValueKind::ToInt;
                        Emit(builder,
                             OpCode::StoreUniform,
                             Type::Void,
                             {Operand::Uniform(
                                      integer ? rd * 8 : offsetof(arm::GuestState, q) + rd * 16,
                                      integer ? Type::U64 : Type::V128),
                              Operand::ValueOf(value)});
                    }
                    result = Literal(builder, 0);
                }
                x64::CompileOptions options;
                options.flags_offset = offsetof(arm::GuestState, packed_flags);
                options.pc_offset = offsetof(arm::GuestState, pc);
                options.arm_fpcr_offset = offsetof(arm::GuestState, fpcr);
                options.arm_fpsr_offset = offsetof(arm::GuestState, fpsr);
                options.arm_fpcr = arm::FPCRForDomain(domain);
                options.local_flags = true;
                options.sse41 = sse41 && (domain & 1);
                options.fma = fma && (!typed || (domain & 1));
                options.avx = fma && (domain & 4);
                options.lahf = domain & 2;
                const auto code = x64::Compile(Return(builder, result), options);
                REQUIRE(bool(code));
                if (!NativeMode()) continue;
                swift::tests::NativeCode native{code.words, swift::tests::NativeISA::X64};
                const unsigned bits = ((word >> 22) & 3) == 1 || (family != arm::Numeric::Scalar &&
                                                                  (word & (1u << 22)))
                                              ? 64
                                              : 32;
                const auto& values = bits == 32 ? singles : doubles;
                for (unsigned sample = 0; sample < values.size(); ++sample) {
                    arm::GuestState state;
                    state.fpcr = options.arm_fpcr;
                    state.fpsr = 0x08000000;
                    state.packed_flags = (u64{1} << 55) | 0x91234567;
                    state.gprs[1] = sample < 9    ? values[sample]
                                    : sample == 9 ? UINT64_MAX
                                                  : (u64{1} << 63) + sample - 10;
                    for (unsigned reg = 0; reg < 4; ++reg)
                        for (unsigned lane = 0; lane < 128 / bits; ++lane) {
                            const u64 value = values[(sample +
                                                      (reg == 1   ? 0
                                                       : reg == 2 ? 5
                                                                  : 9) +
                                                      lane) %
                                                     values.size()];
                            std::memcpy(state.q[reg] + lane * bits / 8, &value, bits / 8);
                        }
                    auto expected = state;
                    const auto answer =
                            arm::ExecuteNumeric(reinterpret_cast<std::uintptr_t>(&expected),
                                                arm::NumericCall(family, word),
                                                0x1000);
                    if (typed && arm::DescribeFPValues(encoded).kind == arm::FPValueKind::Compare)
                        expected.packed_flags =
                                (expected.packed_flags & ~u64{0xf0000000}) | (answer << 28);
                    CHECK(native.Run(reinterpret_cast<std::uint8_t*>(&state)) ==
                          (typed ? 0 : answer));
                    CHECK(std::memcmp(state.q, expected.q, sizeof(state.q)) == 0);
                    CHECK(std::memcmp(state.gprs, expected.gprs, sizeof(state.gprs)) == 0);
                    CHECK(state.fpsr == expected.fpsr);
                    CHECK(state.packed_flags == expected.packed_flags);
                    CHECK(state.helper_status == expected.helper_status);
                }
            }
    }
}

TEST_CASE("Exact floating cold calls share register-independent descriptors",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    for (const bool image : {false, true}) {
        Builder builder{Scope::Block};
        Begin(builder);
        const std::array inputs{
                LoadVector(builder, 0), LoadVector(builder, 1), LoadVector(builder, 2)};
        std::array<u64, 3> modes;
        for (unsigned i = 0; i < modes.size(); ++i) {
            const u32 word = u32(vixl::aarch64::FMADD_s) | ((i + 4) << 5) | ((i + 8) << 16) |
                             ((i + 12) << 10) | (i + 16);
            modes[i] = arm::NumericCall(arm::Numeric::Scalar, word);
            const auto result =
                    Emit(builder,
                         OpCode::ArmFPTernary,
                         Type::V128,
                         {Operand::ValueOf(inputs[i]),
                          Operand::ValueOf(inputs[(i + 1) % inputs.size()]),
                          Operand::ValueOf(inputs[(i + 2) % inputs.size()]),
                          Imm(arm::FPStateOffsets(offsetof(State, fpcr), offsetof(State, fpsr))),
                          Imm(modes[i])});
            StoreVector(builder, i + 3, result);
        }
        auto options = Options();
        options.relocatable = true;
        options.local_flags = true;
        options.lahf = image;
        options.arm_fpcr_offset = offsetof(State, fpcr);
        options.arm_fpsr_offset = offsetof(State, fpsr);
        const auto code = x64::Compile(Return(builder, Literal(builder, 0)), options);
        REQUIRE(bool(code));
        CHECK(std::count_if(code.relocations.begin(), code.relocations.end(), [](const auto& r) {
                  return r.symbol == swift::runtime::backend::HostSymbol::ArmFPValues;
              }) == 1);
        if (!NativeMode()) continue;
        swift::tests::NativeCode native{code.words, swift::tests::NativeISA::X64};
        for (const u32 fpcr : {0u, 1u << 24})
            for (const u32 first : {0x3fc00000u, 0x7f800001u, 1u}) {
                State state;
                state.fpcr = fpcr;
                state.fpsr = 0x08000000;
                state.flags = 0xf1234567;
                state.vectors[0] = Lanes32(first, UINT32_MAX, UINT32_MAX, UINT32_MAX);
                state.vectors[1] = Lanes32(0x40000000, UINT32_MAX, UINT32_MAX, UINT32_MAX);
                state.vectors[2] = Lanes32(0xc0400000, UINT32_MAX, UINT32_MAX, UINT32_MAX);
                auto expected = state;
                for (unsigned i = 0; i < modes.size(); ++i) {
                    const std::array values{state.vectors[i],
                                            state.vectors[(i + 1) % inputs.size()],
                                            state.vectors[(i + 2) % inputs.size()]};
                    REQUIRE(arm::EvaluateFPValues(&expected.vectors[i + 3],
                                                  values.data(),
                                                  modes[i],
                                                  fpcr,
                                                  &expected.fpsr) == 0);
                }
                CHECK(native.Run(reinterpret_cast<std::uint8_t*>(&state)) == 0);
                CHECK(state.vectors == expected.vectors);
                CHECK(state.fpsr == expected.fpsr);
                CHECK(state.flags == expected.flags);
            }
    }
}

TEST_CASE("Terminal conditions materialize across native clobbers",
          "[compile][x64-compile][native]") {
    for (bool image : {false, true})
        for (unsigned condition = 0; condition < 14; ++condition) {
            Builder builder{Scope::Function};
            Begin(builder);
            const auto observed =
                    Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(condition)});
            Store(builder, 3, observed);
            const auto left = Load(builder, 0), right = Load(builder, 1);
            const auto product = Emit(builder,
                                      OpCode::Mul,
                                      Type::U64,
                                      {Operand::ValueOf(left), Operand::ValueOf(right)});
            Store(builder, 2, product);
            const auto predicate =
                    Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(condition)});
            const auto yes = Block(builder), no = Block(builder);
            Need(builder.EndBlock(Terminal::Branch(predicate, yes, no)));
            Need(builder.BeginBlock(yes));
            Need(builder.EndBlock(Terminal::Return(Literal(builder, 1))));
            Need(builder.BeginBlock(no));
            const auto program = Return(builder, Literal(builder, 0));
            auto options = Options();
            options.local_flags = true;
            options.lahf = image;
            const auto code = x64::Compile(program, options);
            REQUIRE(bool(code));
            for (unsigned pattern = 0; pattern < 16; ++pattern) {
                State state;
                state.flags = (u64{1} << 55) | 0x25 | (u64{pattern} << 28);
                const auto old = state.flags;
                state.scalars[0] = 7;
                state.scalars[1] = 9;
                const bool n = flags::GetSF(old), z = flags::GetZF(old), carry = flags::GetCF(old),
                           v = flags::GetOF(old);
                const bool expected[]{z,
                                      !z,
                                      carry,
                                      !carry,
                                      n,
                                      !n,
                                      v,
                                      !v,
                                      carry && !z,
                                      !carry || z,
                                      n == v,
                                      n != v,
                                      !z && n == v,
                                      z || n != v};
                if (Run(code, state, expected[condition])) {
                    CHECK(state.flags == old);
                    CHECK(state.scalars[2] == 63);
                    CHECK(state.scalars[3] == expected[condition]);
                }
            }
        }
}

TEST_CASE("Uniform-backed spills honor writes and raw vector views",
          "[compile][x64-compile][native]") {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Builder builder{Scope::Block};
        Begin(builder);
        std::array<Value, 20> loads;
        for (unsigned i = 0; i < loads.size(); ++i) loads[i] = Load(builder, i);
        u64 external = 0;
        if (scenario) {
            const auto replacement = Literal(builder, 1000);
            if (scenario == 1)
                Store(builder, 18, replacement);
            else {
                const auto address =
                        scenario == 2
                                ? Emit(builder, OpCode::GetUniformAddress, Type::U64, {Imm(18 * 8)})
                                : Literal(builder, reinterpret_cast<std::uintptr_t>(&external));
                Emit(builder,
                     OpCode::StoreMemory,
                     Type::Void,
                     {Operand::ValueOf(address), Operand::ValueOf(replacement)});
            }
        }
        auto sum = loads.front();
        for (unsigned i = 1; i < loads.size(); ++i)
            sum = Emit(builder,
                       OpCode::Add,
                       Type::U64,
                       {Operand::ValueOf(sum), Operand::ValueOf(loads[i])});
        const auto program = Return(builder, sum);
        auto options = Options();
        options.local_flags = true;
        options.private_state = scenario == 3;
        swift::runtime::backend::CompileResult code;
        x64::Allocation allocation;
        x64::Compile(program, options, code, allocation);
        REQUIRE(bool(code));
        unsigned backed = 0;
        for (const auto& home : allocation.homes)
            backed += home.spilled && home.uniform_offset != UINT32_MAX;
        CHECK((backed > 0) == (scenario != 2));
        if (scenario == 1)
            CHECK(allocation.homes[loads[18].node.value].uniform_offset == UINT32_MAX);
        State state;
        for (unsigned i = 0; i < loads.size(); ++i) state.scalars[i] = i + 1;
        if (Run(code, state, 210) && scenario == 3) CHECK(external == 1000);
    }
    Builder builder{Scope::Block};
    Begin(builder);
    std::array<Value, 16> vectors;
    for (unsigned i = 0; i < vectors.size(); ++i) vectors[i] = LoadVector(builder, i);
    auto sum = Literal(builder, 0);
    for (const auto vector : vectors) {
        const auto low = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(vector)});
        sum = Emit(builder, OpCode::Add, Type::U64, {Operand::ValueOf(sum), Operand::ValueOf(low)});
    }
    auto options = Options();
    options.local_flags = true;
    const auto code = x64::Compile(Return(builder, sum), options);
    REQUIRE(bool(code));
    State state;
    for (unsigned i = 0; i < vectors.size(); ++i) state.vectors[i] = {i + 1, 0x7ff0000000000001};
    Run(code, state, 136);
}

TEST_CASE("Cross-block predicate values retain their flags source",
          "[compile][x64-compile][native]") {
    Builder builder{Scope::Function};
    Begin(builder);
    const auto a = Literal(builder, UINT64_MAX), b = Literal(builder, 1);
    Emit(builder,
         OpCode::Add,
         Type::U64,
         {Operand::ValueOf(a), Operand::ValueOf(b)},
         {15, FlagProfile::ArithmeticCanonicalCarry, false});
    Emit(builder, OpCode::Mul, Type::U64, {Operand::ValueOf(a), Operand::ValueOf(b)});
    const auto predicate =
            Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::EQ)});
    const auto yes = Block(builder), no = Block(builder);
    Need(builder.EndBlock(Terminal::Branch(predicate, yes, no)));
    Need(builder.BeginBlock(yes));
    Emit(builder, OpCode::ClearFlags, Type::Void, {Operand::Flags(Flag::NZCV)});
    Need(builder.EndBlock(Terminal::Return(predicate)));
    Need(builder.BeginBlock(no));
    Emit(builder, OpCode::ClearFlags, Type::Void, {Operand::Flags(Flag::NZCV)});
    const auto program = Return(builder, predicate);
    for (bool image : {false, true}) {
        auto options = Options();
        options.local_flags = true;
        options.lahf = image;
        const auto code = x64::Compile(program, options);
        REQUIRE(bool(code));
        State state;
        Run(code, state, 1);
    }
}

TEST_CASE("Local label joins discard path-specific condition bytes",
          "[compile][x64-compile][native]") {
    for (bool image : {false, true}) {
        Builder builder{Scope::Function};
        Begin(builder);
        const auto select = Load(builder, 0);
        const auto one = Literal(builder, 1);
        const auto choose = Emit(builder, OpCode::Goto, Type::U64, {Operand::ValueOf(select)});
        Emit(builder,
             OpCode::Add,
             Type::U64,
             {Operand::ValueOf(one), Operand::ValueOf(one)},
             {15, FlagProfile::ArithmeticCanonicalCarry, false});
        const auto done = Emit(builder, OpCode::Goto, Type::U64, {Operand::ValueOf(one)});
        Emit(builder, OpCode::BindLabel, Type::Void, {Operand::ValueOf(choose)});
        Emit(builder,
             OpCode::Sub,
             Type::U64,
             {Operand::ValueOf(one), Operand::ValueOf(one)},
             {15, FlagProfile::ArithmeticCanonicalCarry, false});
        Emit(builder, OpCode::BindLabel, Type::Void, {Operand::ValueOf(done)});
        const auto predicate =
                Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::EQ)});
        const auto yes = Block(builder), no = Block(builder);
        Need(builder.EndBlock(Terminal::Branch(predicate, yes, no)));
        Need(builder.BeginBlock(yes));
        Need(builder.EndBlock(Terminal::Return(Literal(builder, 1))));
        Need(builder.BeginBlock(no));
        const auto program = Return(builder, Literal(builder, 0));
        auto options = Options();
        options.local_flags = true;
        options.lahf = image;
        swift::runtime::backend::CompileResult code;
        x64::Allocation allocation;
        x64::Compile(program, options, code, allocation);
        REQUIRE(bool(code));
        // Poison the future stack byte before entering the normal prologue.
        // The fallthrough path skips the condition capture before BindLabel.
        std::array<std::uint8_t, 8> prefix{0xc6, 0x84, 0x24, 0, 0, 0, 0, 1};
        const auto displacement = static_cast<std::int32_t>(allocation.condition_slot) -
                                  static_cast<std::int32_t>(code.frame_bytes);
        std::memcpy(prefix.data() + 3, &displacement, 4);
        std::vector<std::uint32_t> words(2);
        std::memcpy(words.data(), prefix.data(), prefix.size());
        words.insert(words.end(), code.words.begin(), code.words.end());
        code.words = std::move(words);
        for (unsigned choice : {0u, 1u}) {
            State state;
            state.scalars[0] = choice;
            Run(code, state, choice);
        }
    }
}

TEST_CASE("Stored scalar and vector values rematerialize from stable slots",
          "[compile][x64-compile][native]") {
    for (bool overwrite : {false, true}) {
        Builder builder{Scope::Block};
        Begin(builder);
        std::array<Value, 16> values;
        for (unsigned i = 0; i < values.size(); ++i) {
            values[i] = Vector(builder, {i + 1, UINT64_C(0x123456789abcdef0) + i});
            StoreVector(builder, i, values[i]);
        }
        if (overwrite) StoreVector(builder, 14, Vector(builder, {}));
        auto answer = values.front();
        for (unsigned i = 1; i < values.size(); ++i)
            answer = Emit(builder,
                          OpCode::VecXor,
                          Type::V128,
                          {Operand::ValueOf(answer), Operand::ValueOf(values[i])});
        StoreVector(builder, 15, answer);
        const auto program = Return(builder);
        auto options = Options();
        options.local_flags = true;
        swift::runtime::backend::CompileResult code;
        x64::Allocation allocation;
        x64::Compile(program, options, code, allocation);
        REQUIRE(bool(code));
        unsigned stored = 0;
        for (const auto& home : allocation.homes)
            stored += home.spilled && home.uniform_store_bytes;
        CHECK(stored > 0);
        if (overwrite) CHECK(allocation.homes[values[14].node.value].uniform_store_bytes == 0);
        Pair expected;
        for (unsigned i = 0; i < values.size(); ++i) {
            expected.low ^= i + 1;
            expected.high ^= UINT64_C(0x123456789abcdef0) + i;
        }
        State state;
        if (Run(code, state)) CHECK(state.vectors[15] == expected);
    }
}

TEST_CASE("Stored U32 arithmetic results keep zero extension when evicted",
          "[compile][x64-compile][native]") {
    Builder builder{Scope::Block};
    Begin(builder);
    std::array<Value, 20> values;
    for (unsigned i = 0; i < values.size(); ++i) {
        const auto input = Literal(builder, 0x80000000u + i, Type::U32);
        values[i] = Emit(builder, OpCode::AsrImm, Type::U32, {Operand::ValueOf(input), Imm(32)});
        Store(builder, i, values[i]);
    }
    auto answer = Literal(builder, 0);
    for (const auto value : values) {
        const auto raw = Emit(builder, OpCode::BitCast, Type::U64, {Operand::ValueOf(value)});
        answer = Emit(
                builder, OpCode::Add, Type::U64, {Operand::ValueOf(answer), Operand::ValueOf(raw)});
    }
    auto options = Options();
    options.local_flags = true;
    swift::runtime::backend::CompileResult code;
    x64::Allocation allocation;
    x64::Compile(Return(builder, answer), options, code, allocation);
    REQUIRE(bool(code));
    unsigned stored = 0;
    for (const auto& home : allocation.homes) stored += home.spilled && home.uniform_store_bytes;
    CHECK(stored > 0);
    State state;
    if (Run(code, state, UINT64_C(20) * 0x80000000u + 190))
        for (unsigned i = 0; i < values.size(); ++i) CHECK(state.scalars[i] == 0x80000000u + i);
}

TEST_CASE("Fused base-displacement addressing, scalar-to-vector loads, and immediate uniform stores",
          "[compile][x64-compile][native]") {
    bool avx_available = false;
#if defined(__x86_64__) && defined(__linux__)
    avx_available = __builtin_cpu_supports("avx");
#endif
    alignas(16) std::array<u64, 8> buffer{UINT64_C(0x0102030405060708),
                                          UINT64_C(0x1122334455667788),
                                          UINT64_C(0x99aabbccddeeff00),
                                          UINT64_C(0xfedcba9876543210),
                                          0,
                                          0,
                                          0,
                                          0};
    for (bool use_avx : {false, true}) {
        if (use_avx && NativeMode() && !avx_available) continue;
        Builder builder{Scope::Block};
        Begin(builder);
        const auto base = Load(builder, 0);
        // Positive disp32 fused with BitCast :v128 (U64 and U32) and VecDup64 :v128.
        Emit(builder, OpCode::FaultLocation, Type::Void, {Imm(0x4100)});
        const auto addr_d = Emit(builder, OpCode::Add, Type::U64, {Operand::ValueOf(base), Imm(8)});
        const auto load_d = Emit(builder, OpCode::LoadMemory, Type::U64, {Operand::ValueOf(addr_d)});
        const auto vec_d = Emit(builder, OpCode::BitCast, Type::V128, {Operand::ValueOf(load_d)});
        StoreVector(builder, 0, vec_d);

        Emit(builder, OpCode::FaultLocation, Type::Void, {Imm(0x4104)});
        const auto addr_s =
                Emit(builder, OpCode::Sub, Type::U64, {Operand::ValueOf(base), Imm(8)});
        const auto load_s = Emit(builder, OpCode::LoadMemory, Type::U32, {Operand::ValueOf(addr_s)});
        const auto vec_s = Emit(builder, OpCode::BitCast, Type::V128, {Operand::ValueOf(load_s)});
        StoreVector(builder, 1, vec_s);

        Emit(builder, OpCode::FaultLocation, Type::Void, {Imm(0x4108)});
        const auto addr_dup =
                Emit(builder, OpCode::Add, Type::U64, {Operand::ValueOf(base), Imm(16)});
        const auto load_dup =
                Emit(builder, OpCode::LoadMemory, Type::U64, {Operand::ValueOf(addr_dup)});
        const auto vec_dup = Emit(builder, OpCode::VecDup64, Type::V128, {Operand::ValueOf(load_dup)});
        StoreVector(builder, 2, vec_dup);

        // StoreMemory with Sub displacement and immediate StoreUniform folding.
        const auto addr_st =
                Emit(builder, OpCode::Add, Type::U64, {Operand::ValueOf(base), Imm(24)});
        const auto sum = Emit(builder,
                              OpCode::Add,
                              Type::U64,
                              {Operand::ValueOf(base), Imm(0x1234)});
        Emit(builder, OpCode::StoreMemory, Type::Void, {Operand::ValueOf(addr_st), Operand::ValueOf(sum)});
        Store(builder, 2, Literal(builder, UINT64_C(0xffffffff87654321)));
        Store(builder, 3, Literal(builder, UINT64_C(0x123456789abcdef0)));

        auto options = Options();
        options.avx = use_avx;
        options.local_flags = true;
        swift::runtime::backend::CompileResult code;
        x64::Allocation allocation;
        x64::Compile(Return(builder), options, code, allocation);
        REQUIRE(bool(code));
        CHECK(code.faults.size() >= 3);
        CHECK(allocation.homes[addr_d.node.value].rematerialize);
        CHECK(allocation.homes[addr_s.node.value].rematerialize);
        CHECK(allocation.homes[load_d.node.value].rematerialize);
        CHECK(allocation.homes[load_s.node.value].rematerialize);
        if (use_avx) CHECK(allocation.homes[load_dup.node.value].rematerialize);

        State state;
        state.scalars[0] = reinterpret_cast<std::uintptr_t>(&buffer[1]);
        state.vectors[0] = {UINT64_MAX, UINT64_MAX};
        state.vectors[1] = {UINT64_MAX, UINT64_MAX};
        state.vectors[2] = {UINT64_MAX, UINT64_MAX};
        if (Run(code, state)) {
            CHECK(state.vectors[0] == (Pair{buffer[2], 0}));
            CHECK(state.vectors[1] == (Pair{static_cast<u32>(buffer[0]), 0}));
            CHECK(state.vectors[2] == (Pair{buffer[3], buffer[3]}));
            CHECK(buffer[4] == state.scalars[0] + 0x1234);
            CHECK(state.scalars[2] == UINT64_C(0xffffffff87654321));
            CHECK(state.scalars[3] == UINT64_C(0x123456789abcdef0));
        }
    }
}

TEST_CASE("Uniform backing crosses disjoint uniform stores while honoring early overwrite hazards",
          "[compile][x64-compile][native]") {
    Builder builder{Scope::Block};
    Begin(builder);
    const auto old_slot1 = LoadVector(builder, 1);
    const auto v0 = Vector(builder, {0x1111, 0x2222});
    const auto v1 = Vector(builder, {0x3333, 0x4444});
    // StoreVector(0, v0) sits between v1's definition and StoreVector(1, v1), as in LDP.
    StoreVector(builder, 0, v0);
    StoreVector(builder, 1, v1);
    std::array<Value, 14> extras;
    for (unsigned i = 0; i < extras.size(); ++i) {
        extras[i] = Vector(builder, {0x5000 + i, 0x6000 + i});
        StoreVector(builder, 2 + i, extras[i]);
    }
    auto acc = Emit(
            builder, OpCode::VecXor, Type::V128, {Operand::ValueOf(old_slot1), Operand::ValueOf(v1)});
    acc = Emit(builder, OpCode::VecXor, Type::V128, {Operand::ValueOf(acc), Operand::ValueOf(v0)});
    for (const auto ex : extras)
        acc = Emit(builder, OpCode::VecXor, Type::V128, {Operand::ValueOf(acc), Operand::ValueOf(ex)});
    StoreVector(builder, 15, acc);

    auto options = Options();
    options.avx = false;
    options.local_flags = true;
    swift::runtime::backend::CompileResult code;
    x64::Allocation allocation;
    x64::Compile(Return(builder), options, code, allocation);
    REQUIRE(bool(code));
    CHECK(allocation.homes[v1.node.value].uniform_store_bytes == 16);
    CHECK(allocation.homes[old_slot1.node.value].uniform_offset == UINT32_MAX);

    State state;
    state.vectors[1] = {UINT64_C(0xdeadbeef00001111), UINT64_C(0xcafebabe00002222)};
    Pair expected = state.vectors[1];
    expected.low ^= 0x3333 ^ 0x1111;
    expected.high ^= 0x4444 ^ 0x2222;
    for (unsigned i = 0; i < extras.size(); ++i) {
        expected.low ^= 0x5000 + i;
        expected.high ^= 0x6000 + i;
    }
    if (Run(code, state)) CHECK(state.vectors[15] == expected);
}

TEST_CASE("Composite SIB load addressing, zero-identity copies, and BMI1/BMI2 lowering",
          "[compile][x64-compile][native]") {
    bool bmi1_available = false, bmi2_available = false;
#if defined(__x86_64__) && defined(__linux__)
    bmi1_available = __builtin_cpu_supports("bmi");
    bmi2_available = __builtin_cpu_supports("bmi2");
#endif
    alignas(16) std::array<u32, 16> table{};
    for (unsigned i = 0; i < table.size(); ++i) table[i] = 0x10000000u + i * 0x01020304u;

    for (bool use_bmi : {false, true}) {
        if (use_bmi && NativeMode() && (!bmi1_available || !bmi2_available)) continue;
        Builder builder{Scope::Function};
        const auto entry = Block(builder), loop = Block(builder), exit = Block(builder);
        Need(builder.BeginBlock(entry));
        const auto zero = Literal(builder, 0);
        const auto init_cnt = Load(builder, 0);
        const auto cnt0 = Emit(builder,
                               OpCode::Sub,
                               Type::U64,
                               {Operand::ValueOf(init_cnt), Imm(0)},
                               {15, FlagProfile::ArithmeticCanonicalCarry, false});
        Store(builder, 0, cnt0);
        Need(builder.EndBlock(Terminal::Jump(loop)));

        Need(builder.BeginBlock(loop));
        const auto base = Load(builder, 1);
        const auto raw_idx = Load(builder, 2);
        // Upper 32 bits of raw_idx are dirty; ZeroExtend32To64 + LslImm(2) + Add(disp) fuses into SIB load.
        const auto uxtw =
                Emit(builder, OpCode::ZeroExtend32To64, Type::U64, {Operand::ValueOf(raw_idx, Type::U32)});
        const auto scaled = Emit(builder, OpCode::LslImm, Type::U64, {Operand::ValueOf(uxtw), Imm(2)});
        const auto sum_addr =
                Emit(builder, OpCode::Add, Type::U64, {Operand::ValueOf(base), Operand::ValueOf(scaled)});
        const auto disp_addr =
                Emit(builder, OpCode::Add, Type::U64, {Operand::ValueOf(sum_addr), Imm(8)});
        const auto elem = Emit(builder, OpCode::LoadMemory, Type::U32, {Operand::ValueOf(disp_addr)});

        // 64-bit And with 0xffffffff lowers to zero-extending move without clobbering flags.
        const auto masked64 =
                Emit(builder, OpCode::And, Type::U64, {Operand::ValueOf(raw_idx), Imm(0xffffffffu)});
        // RorImm under BMI2 (rorx) and zero-identity Or/Xor/Add/Sub preserve native flags.
        const auto rot = Emit(builder, OpCode::RorImm, Type::U64, {Operand::ValueOf(masked64), Imm(7)});
        const auto copy_or =
                Emit(builder, OpCode::Or, Type::U32, {Operand::ValueOf(zero, Type::U32), Operand::ValueOf(elem)});
        const auto copy_xor = Emit(
                builder, OpCode::Xor, Type::U64, {Operand::ValueOf(rot), Operand::ValueOf(zero)});
        const auto combined0 =
                Emit(builder, OpCode::Add, Type::U64, {Operand::ValueOf(copy_xor), Operand::ValueOf(copy_or, Type::U32)});
        const auto combined =
                Emit(builder, OpCode::Adc, Type::U64, {Operand::ValueOf(combined0), Imm(0)});
        // AndNot (a & ~b) with and without flags.
        const auto mask_inv = Load(builder, 3);
        const auto bic = Emit(
                builder, OpCode::AndNot, Type::U64, {Operand::ValueOf(combined), Operand::ValueOf(mask_inv)});
        const auto prev_acc = Load(builder, 4);
        const auto next_acc =
                Emit(builder, OpCode::Xor, Type::U64, {Operand::ValueOf(prev_acc), Operand::ValueOf(bic)});
        Store(builder, 4, next_acc);

        // Decrement loop counter with flags, followed by zero-identity copies before Branch.
        const auto cnt = Load(builder, 0);
        const auto next_cnt = Emit(builder,
                                   OpCode::Sub,
                                   Type::U64,
                                   {Operand::ValueOf(cnt), Imm(1)},
                                   {15, FlagProfile::ArithmeticCanonicalCarry, false});
        const auto copied_cnt =
                Emit(builder, OpCode::Or, Type::U64, {Operand::ValueOf(zero), Operand::ValueOf(next_cnt)});
        Store(builder, 0, copied_cnt);
        const auto next_idx = Emit(builder, OpCode::Add, Type::U32, {Operand::ValueOf(raw_idx, Type::U32), Imm(1)});
        const auto copied_idx =
                Emit(builder, OpCode::Add, Type::U32, {Operand::ValueOf(zero, Type::U32), Operand::ValueOf(next_idx)});
        Store(builder, 2, copied_idx);
        const auto more =
                Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::NE)});
        Need(builder.EndBlock(Terminal::Branch(more, loop, exit)));

        Need(builder.BeginBlock(exit));
        const auto bics = Emit(builder,
                               OpCode::AndNot,
                               Type::U64,
                               {Operand::ValueOf(Load(builder, 4)), Operand::ValueOf(Load(builder, 5))},
                               {15, FlagProfile::LogicalZeroCVA, false});
        const auto program = Return(builder, bics);

        auto options = Options();
        options.local_flags = true;
        options.bmi1 = use_bmi;
        options.bmi2 = use_bmi;
        swift::runtime::backend::CompileResult code;
        x64::Allocation allocation;
        x64::Compile(program, options, code, allocation);
        REQUIRE(bool(code));
        CHECK(allocation.homes[zero.node.value].rematerialize);
        CHECK(allocation.homes[uxtw.node.value].rematerialize);
        CHECK(allocation.homes[scaled.node.value].rematerialize);
        CHECK(allocation.homes[sum_addr.node.value].rematerialize);
        CHECK(allocation.homes[disp_addr.node.value].rematerialize);
        CHECK(allocation.native_entry[loop.id.value]);

        State state;
        state.scalars[0] = 4;
        state.scalars[1] = reinterpret_cast<std::uintptr_t>(table.data());
        state.scalars[2] = UINT64_C(0xfeedface00000001);
        state.scalars[3] = UINT64_C(0x00ff00ff00ff00ff);
        state.scalars[4] = 0;
        state.scalars[5] = UINT64_C(0xf00000000000000f);
        u64 expected_acc = 0;
        for (u32 step = 0; step < 4; ++step) {
            const u32 idx32 = 1u + step;
            const u32 loaded = table[idx32 + 2];
            const u64 rotated = (u64{idx32} >> 7) | (u64{idx32} << 57);
            const u64 comb = rotated + loaded + 1;
            expected_acc ^= comb & ~state.scalars[3];
        }
        const u64 expected_ret = expected_acc & ~state.scalars[5];
        if (Run(code, state, expected_ret)) {
            CHECK(state.scalars[0] == 0);
            CHECK(state.scalars[2] == 5);
            CHECK(state.scalars[4] == expected_acc);
            const bool zf = expected_ret == 0;
            const bool sf = (expected_ret >> 63) != 0;
            CHECK(bool(state.flags & flags::Zero) == zf);
            CHECK(bool(state.flags & flags::Sign) == sf);
            CHECK(!(state.flags & (flags::Carry | flags::Overflow)));
        }
    }
}

TEST_CASE("Resident AVX single-precision and 2S fused/binary FP match ARM oracle",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    using namespace vixl::aarch64;
    bool fma_available = false;
#if defined(__x86_64__) && defined(__linux__)
    fma_available = __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma");
#endif
    if (NativeMode() && !fma_available) return;

    for (unsigned rmode = 0; rmode < 4; ++rmode) {
        const u32 fpcr = rmode << 22;
        for (u32 fma_op : {u32(FMADD_s), u32(FMSUB_s), u32(FNMADD_s), u32(FNMSUB_s)}) {
            Builder builder{Scope::Block};
            Begin(builder);
            // Force resident XMM allocation via VecXor with zero before ArmFPBinary / ArmFPTernary.
            const auto zvec = Vector(builder, {0, 0});
            const auto a = Emit(builder, OpCode::VecXor, Type::V128, {Operand::ValueOf(LoadVector(builder, 0)), Operand::ValueOf(zvec)});
            const auto b = Emit(builder, OpCode::VecXor, Type::V128, {Operand::ValueOf(LoadVector(builder, 1)), Operand::ValueOf(zvec)});
            const auto c = Emit(builder, OpCode::VecXor, Type::V128, {Operand::ValueOf(LoadVector(builder, 2)), Operand::ValueOf(zvec)});
            const auto offsets =
                    Imm(arm::FPStateOffsets(offsetof(State, fpcr), offsetof(State, fpsr)));
            const u32 scalar_word = fma_op | (1u << 5) | (2u << 16) | (3u << 10);
            const auto s_res = Emit(builder,
                                    OpCode::ArmFPTernary,
                                    Type::V128,
                                    {Operand::ValueOf(a),
                                     Operand::ValueOf(b),
                                     Operand::ValueOf(c),
                                     offsets,
                                     Imm(arm::NumericCall(arm::Numeric::Scalar, scalar_word))});
            const u32 fmls_2s = u32(NEON_FMLS) | (1u << 5) | (2u << 16);
            const auto v_res = Emit(builder,
                                    OpCode::ArmFPTernary,
                                    Type::V128,
                                    {Operand::ValueOf(a),
                                     Operand::ValueOf(b),
                                     Operand::ValueOf(c),
                                     offsets,
                                     Imm(arm::NumericCall(arm::Numeric::Fused, fmls_2s))});
            const auto bin_s = Emit(builder,
                                    OpCode::ArmFPBinary,
                                    Type::V128,
                                    {Operand::ValueOf(a),
                                     Operand::ValueOf(b),
                                     Imm(offsetof(State, fpcr)),
                                     Imm(offsetof(State, fpsr)),
                                     Imm(arm::ArmFPBinaryMode(2, 32, 1, true, 1))});
            StoreVector(builder, 3, s_res);
            StoreVector(builder, 4, v_res);
            StoreVector(builder, 5, bin_s);

            auto options = Options();
            options.local_flags = true;
            options.avx = true;
            options.fma = true;
            options.arm_fpcr = fpcr;
            options.arm_fpcr_offset = offsetof(State, fpcr);
            options.arm_fpsr_offset = offsetof(State, fpsr);
            const auto code = x64::Compile(Return(builder), options);
            REQUIRE(bool(code));

            for (bool zero_cancel : {false, true}) {
                State state;
                state.fpcr = fpcr;
                // Put signaling/quiet NaNs in upper inactive lanes to verify zero spurious exceptions!
                state.vectors[0] = zero_cancel
                                           ? Lanes32(0x00000000, 0x3f800000, 0x7f800001, 0x7fc00042)
                                           : Lanes32(0x3fc00000, 0xbf800000, 0x7f800001, 0x7fc00042);
                state.vectors[1] = zero_cancel
                                           ? Lanes32(0x3f800000, 0x3f800000, 0x7f800001, 0xff800000)
                                           : Lanes32(0x40000000, 0x40200000, 0x7f800001, 0xff800000);
                state.vectors[2] = zero_cancel
                                           ? Lanes32(0x00000000, 0x3f800000, 0x7f800001, 0x7fc00042)
                                           : Lanes32(0x3f000000, 0x40400000, 0x7f800001, 0x7fc00042);
                if (!Run(code, state)) continue;

                const bool neg_add = scalar_word & (1u << 21);
                const bool neg_lhs = bool(scalar_word & (1u << 15)) != neg_add;
                const u32 ctrl = (neg_lhs ? 1u : 0u) | (neg_add ? 2u : 0u);
                const auto exp_s = arm::EvaluateFusedFP(
                        u32(state.vectors[0].low), u32(state.vectors[1].low), u32(state.vectors[2].low), 32, ctrl, fpcr);
                const auto exp_2s_0 = arm::EvaluateFusedFP(
                        u32(state.vectors[0].low), u32(state.vectors[1].low), u32(state.vectors[2].low), 32, 1, fpcr);
                const auto exp_2s_1 = arm::EvaluateFusedFP(
                        u32(state.vectors[0].low >> 32),
                        u32(state.vectors[1].low >> 32),
                        u32(state.vectors[2].low >> 32),
                        32,
                        1,
                        fpcr);
                const auto exp_bin = arm::EvaluateBinaryFP(
                        u32(state.vectors[0].low), u32(state.vectors[1].low >> 32), 32, 0, fpcr);
                CHECK(state.vectors[3] == (Pair{u32(exp_s.value), 0}));
                CHECK(state.vectors[4] == Lanes32(u32(exp_2s_0.value), u32(exp_2s_1.value), 0, 0));
                CHECK(state.vectors[5] == (Pair{u32(exp_bin.value), 0}));
                CHECK(state.fpsr ==
                      (exp_s.exceptions | exp_2s_0.exceptions | exp_2s_1.exceptions | exp_bin.exceptions));
            }
        }
    }
}

TEST_CASE("Dynamic dispatch defers its event poll to the inline L1 probe",
          "[compile][x64-compile]") {
    using swift::runtime::backend::FaultSiteKind;
    using swift::runtime::backend::HostSymbol;
    for (const auto scope : {Scope::Block, Scope::Function}) {
        const auto build = [scope](OpCode marker, bool constant_tail) {
            Builder builder{scope};
            Need(builder.BeginBlock(Block(builder)));
            if (marker != OpCode::Nop) Emit(builder, marker, Type::Void, {Imm(0x1200)});
            const auto target = constant_tail ? Literal(builder, 0x1400) : Load(builder, 0);
            Emit(builder,
                 OpCode::StoreUniform,
                 Type::Void,
                 {Operand::Uniform(offsetof(State, pc), Type::U64), Operand::ValueOf(target)});
            Need(builder.EndBlock(Terminal::Dispatch()));
            return Submit(builder);
        };
        auto options = Options();
        options.dispatcher = 0x1000;
        options.exit_handler = 0x2000;
        options.l2_dispatcher = 0x3000;
        options.local_flags = options.linked_flags = options.private_state = true;
        options.relocatable = true;
        for (const bool counted : {false, true}) {
            options.counted_dispatch = counted;
            const auto bare = x64::Compile(build(OpCode::Nop, false), options);
            const auto interruptible =
                    x64::Compile(build(OpCode::GuestDispatchSafepoint, false), options);
            const auto explicit_poll =
                    x64::Compile(build(OpCode::GuestSafepoint, false), options);
            REQUIRE(bool(bare));
            REQUIRE(bool(interruptible));
            REQUIRE(bool(explicit_poll));
            CHECK(interruptible.words == bare.words);
            CHECK(explicit_poll.words.size() > interruptible.words.size());
            CHECK(interruptible.has_indirect_dispatch);
            REQUIRE(interruptible.faults.size() == 1);
            const auto& site = interruptible.faults.front();
            CHECK(site.Kind() == FaultSiteKind::DispatchInterrupt);
            CHECK(site.last_offset == site.first_offset + 3);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(interruptible.words.data());
            CHECK(bytes[site.first_offset] == 0x49);
            CHECK(bytes[site.first_offset + 1] == 0x39);
            CHECK(bytes[site.first_offset + 2] == 0x38);
            const bool has_l2 = std::any_of(
                    interruptible.relocations.begin(),
                    interruptible.relocations.end(),
                    [](const auto& r) { return r.symbol == HostSymbol::DispatcherL2; });
            CHECK(has_l2);
            auto relocated = interruptible.words;
            CHECK(x64::Relocate(
                    {reinterpret_cast<std::uint8_t*>(relocated.data()), relocated.size() * 4},
                    interruptible.relocations,
                    options));
            const auto fallback =
                    x64::Compile(build(OpCode::GuestDispatchSafepoint, true), options);
            const auto ordinary = x64::Compile(build(OpCode::GuestSafepoint, true), options);
            REQUIRE(bool(fallback));
            REQUIRE(bool(ordinary));
            CHECK(fallback.words == ordinary.words);
        }
    }
}

TEST_CASE("Vector widen multiply, TBL32 shuffle, and AVX 3-operand lanes match baseline",
          "[compile][x64-compile][native]") {
    bool ssse3_ok = false, sse41_ok = false, avx_ok = false;
#if defined(__x86_64__) && defined(__linux__)
    ssse3_ok = __builtin_cpu_supports("ssse3");
    sse41_ok = __builtin_cpu_supports("sse4.1");
    avx_ok = __builtin_cpu_supports("avx");
#endif
    const auto build_suite = [] {
        Builder builder{Scope::Block};
        Begin(builder);
        const auto z = Vector(builder, {0, 0});
        const auto a = Emit(builder, OpCode::VecXor, Type::V128,
                            {Operand::ValueOf(LoadVector(builder, 0)), Operand::ValueOf(z)});
        const auto b = Emit(builder, OpCode::VecXor, Type::V128,
                            {Operand::ValueOf(LoadVector(builder, 1)), Operand::ValueOf(z)});
        StoreVector(builder, 2, Emit(builder, OpCode::VecMulWiden, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(8), Imm(0)}));
        StoreVector(builder, 3, Emit(builder, OpCode::VecMulWiden, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(8), Imm(1)}));
        StoreVector(builder, 4, Emit(builder, OpCode::VecMulWiden, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(16), Imm(0)}));
        StoreVector(builder, 5, Emit(builder, OpCode::VecMulWiden, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32), Imm(1)}));
        StoreVector(builder, 6, Emit(builder, OpCode::VecSignedMulWidenLow, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(8)}));
        StoreVector(builder, 7, Emit(builder, OpCode::VecSignedMulWidenLow, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32)}));
        StoreVector(builder, 8, Emit(builder, OpCode::VecShuffle32Indexed, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b)}));
        StoreVector(builder, 9, Emit(builder, OpCode::VecAvg, Type::V128,
                                     {Operand::ValueOf(a), Operand::ValueOf(b), Imm(16)}));
        StoreVector(builder, 10, Emit(builder, OpCode::VecSatSub, Type::V128,
                                      {Operand::ValueOf(a), Operand::ValueOf(b), Imm(8), Imm(0)}));
        StoreVector(builder, 11, Emit(builder, OpCode::VecMul, Type::V128,
                                      {Operand::ValueOf(a), Operand::ValueOf(b), Imm(32)}));
        StoreVector(builder, 12, Emit(builder, OpCode::VecShiftRightArithmeticImm, Type::V128,
                                      {Operand::ValueOf(a), Imm(3), Imm(16)}));
        StoreVector(builder, 13, a);
        StoreVector(builder, 14, b);
        return Return(builder);
    };
    const auto program = build_suite();
    auto base_opts = Options();
    base_opts.ssse3 = base_opts.sse41 = base_opts.avx = false;
    const auto ref_code = x64::Compile(program, base_opts);
    REQUIRE(bool(ref_code));

    for (unsigned tier = 1; tier <= 3; ++tier) {
        if (NativeMode() && ((tier == 1 && !ssse3_ok) || (tier == 2 && !sse41_ok) ||
                             (tier == 3 && !avx_ok)))
            continue;
        auto opts = base_opts;
        opts.ssse3 = tier >= 1;
        opts.sse41 = tier >= 2;
        opts.avx = tier >= 3;
        const auto opt_code = x64::Compile(program, opts);
        REQUIRE(bool(opt_code));
        if (!NativeMode()) continue;
        State ref_state, opt_state;
        ref_state.vectors[0] = {UINT64_C(0x80ff7f01fedcba98), UINT64_C(0x800000017fffffff)};
        ref_state.vectors[1] = {UINT64_C(0x100f00ff03020100), UINT64_C(0xff80110e0d0c0b0a)};
        opt_state = ref_state;
        REQUIRE(Run(ref_code, ref_state));
        REQUIRE(Run(opt_code, opt_state));
        for (unsigned slot = 2; slot <= 14; ++slot)
            CHECK(opt_state.vectors[slot] == ref_state.vectors[slot]);
    }
}

TEST_CASE("Fixed-point, NEON FP conversions, vector FSQRT, and RestoreFlags match ARM oracle",
          "[compile][x64-compile][native]") {
    namespace arm = swift::runtime::arm64;
    using namespace vixl::aarch64;

    using IrCond = swift::runtime::ir::Condition;
    // 1. Immediate RestoreFlags under local_flags + lahf across all 16 NZCV states.
    for (u64 nzcv = 0; nzcv < 16; ++nzcv) {
        Builder builder{Scope::Block};
        Begin(builder);
        Emit(builder,
             OpCode::RestoreFlags,
             Type::Void,
             {Operand::ValueOf(Literal(builder, nzcv << 28)), Operand::Flags(15)});
        Store(builder, 0, Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(IrCond::EQ)}));
        Store(builder, 1, Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(IrCond::CS)}));
        Store(builder, 2, Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(IrCond::MI)}));
        Store(builder, 3, Emit(builder, OpCode::CondSet, Type::Bool, {Operand::Condition(IrCond::VS)}));
        auto opts = Options();
        opts.local_flags = true;
        opts.lahf = true;
        const auto code = x64::Compile(Return(builder), opts);
        REQUIRE(bool(code));
        State state;
        state.flags = (~nzcv & 15) << 28;
        if (Run(code, state)) {
            CHECK(state.scalars[0] == ((nzcv >> 2) & 1));
            CHECK(state.scalars[1] == ((nzcv >> 1) & 1));
            CHECK(state.scalars[2] == ((nzcv >> 3) & 1));
            CHECK(state.scalars[3] == (nzcv & 1));
            CHECK(((state.flags >> 28) & 15) == nzcv);
        }
    }

    // 2. Scalar fixed-point + NEON Integer/Fixed/VectorUnary across FPCR rounding & FZ domains.
    const auto offsets = Imm(arm::FPStateOffsets(offsetof(State, fpcr), offsetof(State, fpsr)));
    const u64 s_scvtf = arm::NumericCall(
            arm::Numeric::Scalar,
            u32(FPFixedPointConvertFixed) | (1u << 31) | (2u << 16) | ((64u - 16u) << 10) | 32u);
    const u64 s_fcvtzs = arm::NumericCall(
            arm::Numeric::Scalar,
            u32(FPFixedPointConvertFixed) | (24u << 16) | ((64u - 8u) << 10) | 32u);
    const u64 v_scvtf_4s =
            arm::NumericCall(arm::Numeric::Integer, u32(NEON_SCVTF) | u32(NEON_Q) | 32u);
    const u64 v_ucvtf_2d = arm::NumericCall(
            arm::Numeric::Integer, u32(NEON_UCVTF) | u32(NEON_Q) | (1u << 22) | 32u);
    const u64 v_fcvtzs_4s_imm = arm::NumericCall(
            arm::Numeric::Fixed, u32(NEON_FCVTZS_imm) | u32(NEON_Q) | ((64u - 12u) << 16) | 32u);
    const u64 v_fcvtzu_2d_imm = arm::NumericCall(
            arm::Numeric::Fixed, u32(NEON_FCVTZU_imm) | u32(NEON_Q) | ((128u - 20u) << 16) | 32u);
    const u64 v_fsqrt_4s =
            arm::NumericCall(arm::Numeric::VectorUnary, u32(NEON_FSQRT) | u32(NEON_Q) | 32u);
    const u64 v_fsqrt_2d = arm::NumericCall(
            arm::Numeric::VectorUnary, u32(NEON_FSQRT) | u32(NEON_Q) | (1u << 22) | 32u);
    for (u64 enc : {s_scvtf, s_fcvtzs, v_scvtf_4s, v_ucvtf_2d,
                    v_fcvtzs_4s_imm, v_fcvtzu_2d_imm, v_fsqrt_4s, v_fsqrt_2d}) {
        REQUIRE(arm::DescribeFPValues(enc).kind != arm::FPValueKind::Invalid);
    }

    for (u32 fpcr : {0u, 1u << 22, 2u << 22, 3u << 22, 1u << 24}) {
        Builder builder{Scope::Block};
        Begin(builder);
        StoreVector(builder, 4, Emit(builder, OpCode::ArmFPConvert, Type::V128,
                                     {Operand::ValueOf(Load(builder, 0)), offsets, Imm(s_scvtf)}));
        Store(builder, 1, Emit(builder, OpCode::ArmFPConvert, Type::U64,
                               {Operand::ValueOf(LoadVector(builder, 0)), offsets, Imm(s_fcvtzs)}));
        StoreVector(builder, 5, Emit(builder, OpCode::ArmFPConvert, Type::V128,
                                     {Operand::ValueOf(LoadVector(builder, 1)), offsets,
                                      Imm(v_scvtf_4s)}));
        StoreVector(builder, 6, Emit(builder, OpCode::ArmFPConvert, Type::V128,
                                     {Operand::ValueOf(LoadVector(builder, 2)), offsets,
                                      Imm(v_ucvtf_2d)}));
        StoreVector(builder, 7, Emit(builder, OpCode::ArmFPConvert, Type::V128,
                                     {Operand::ValueOf(LoadVector(builder, 0)), offsets,
                                      Imm(v_fcvtzs_4s_imm)}));
        StoreVector(builder, 8, Emit(builder, OpCode::ArmFPConvert, Type::V128,
                                     {Operand::ValueOf(LoadVector(builder, 3)), offsets,
                                      Imm(v_fcvtzu_2d_imm)}));
        StoreVector(builder, 9, Emit(builder, OpCode::ArmFPUnary, Type::V128,
                                     {Operand::ValueOf(LoadVector(builder, 0)), offsets,
                                      Imm(v_fsqrt_4s)}));
        StoreVector(builder, 10, Emit(builder, OpCode::ArmFPUnary, Type::V128,
                                      {Operand::ValueOf(LoadVector(builder, 3)), offsets,
                                       Imm(v_fsqrt_2d)}));

        auto opts = Options();
        opts.local_flags = true;
        opts.arm_fpcr = fpcr;
        opts.arm_fpcr_offset = offsetof(State, fpcr);
        opts.arm_fpsr_offset = offsetof(State, fpsr);
        const auto code = x64::Compile(Return(builder), opts);
        REQUIRE(bool(code));
        if (!NativeMode()) continue;

        for (bool edge_case : {false, true}) {
            State state;
            state.fpcr = fpcr;
            state.scalars[0] = edge_case ? UINT64_C(0x7fffffffffffffff) : UINT64_C(0xfffffffffffe8000);
            state.vectors[0] = edge_case ? Lanes32(0x7fc00001, 0x00000001, 0xbf800000, 0x4f000000)
                                         : Lanes32(0x3fc00000, 0x40900000, 0x41820000, 0x3e800000);
            state.vectors[1] = edge_case ? Lanes32(0x80000000, 0x7fffffff, 0x00000000, 0xffffffff)
                                         : Lanes32(100, static_cast<u32>(-250), 16777217, 42);
            state.vectors[2] = edge_case ? Pair{UINT64_C(0x8000000000000005), UINT64_C(999)}
                                         : Pair{UINT64_C(123456789), UINT64_C(0x7fffffffffffffff)};
            state.vectors[3] = edge_case
                                       ? Pair{UINT64_C(0xfff0000000000000), UINT64_C(0x43f0000000000000)}
                                       : Pair{UINT64_C(0x4002000000000000), UINT64_C(0x4022000000000000)};
            REQUIRE(Run(code, state));

            u32 expected_fpsr = 0;
            const auto eval = [&](u64 enc, const void* in, void* out) {
                REQUIRE(arm::EvaluateFPValues(out, in, enc, fpcr, &expected_fpsr) == 0);
            };
            Pair out_v4{}, out_v5{}, out_v6{}, out_v7{}, out_v8{}, out_v9{}, out_v10{};
            Pair out_s1{};
            eval(s_scvtf, &state.scalars[0], &out_v4);
            eval(s_fcvtzs, &state.vectors[0], &out_s1);
            eval(v_scvtf_4s, &state.vectors[1], &out_v5);
            eval(v_ucvtf_2d, &state.vectors[2], &out_v6);
            eval(v_fcvtzs_4s_imm, &state.vectors[0], &out_v7);
            eval(v_fcvtzu_2d_imm, &state.vectors[3], &out_v8);
            eval(v_fsqrt_4s, &state.vectors[0], &out_v9);
            eval(v_fsqrt_2d, &state.vectors[3], &out_v10);

            CHECK(state.scalars[1] == out_s1.low);
            CHECK(state.vectors[4] == out_v4);
            CHECK(state.vectors[5] == out_v5);
            CHECK(state.vectors[6] == out_v6);
            CHECK(state.vectors[7] == out_v7);
            CHECK(state.vectors[8] == out_v8);
            CHECK(state.vectors[9] == out_v9);
            CHECK(state.vectors[10] == out_v10);
            CHECK(state.fpsr == expected_fpsr);
        }
    }
}
