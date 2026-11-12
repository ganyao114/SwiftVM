#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/backend/x64/allocate.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/helpers.h"
#include "runtime/backend/x64/patch.h"
#include "runtime/backend/x64/relocate.h"
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

x64::CompileOptions Options() {
    x64::CompileOptions options;
    options.flags_offset = offsetof(State, flags);
    options.pc_offset = offsetof(State, pc);
    options.mxcsr_offset = offsetof(State, mxcsr);
    options.fault_locations = true;
    options.native_fp_environment = true;
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
    const auto sum = Emit(
            builder, OpCode::Add, Type::U8, {Operand::ValueOf(ff), Operand::ValueOf(one)}, full);
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
    for (const auto type : {Type::U8, Type::U16, Type::U32, Type::U64}) {
        const unsigned bits = TypeBits(type);
        const u64 mask = UINT64_MAX >> (64 - bits), sign = u64{1} << (bits - 1);
        const u64 values[]{0, 1, 15, 16, mask, sign - 1, sign, mask / 3, mask / 3 * 2};
        for (const auto opcode : {OpCode::Add, OpCode::Sub, OpCode::Adc, OpCode::Sbb}) {
            const bool subtract = opcode == OpCode::Sub || opcode == OpCode::Sbb;
            const bool with_carry = opcode == OpCode::Adc || opcode == OpCode::Sbb;
            for (const auto profile :
                 {FlagProfile::ArithmeticRaw, FlagProfile::ArithmeticCanonicalCarry}) {
                Builder builder{Scope::Block};
                Begin(builder);
                const auto lhs = Load(builder, 0), rhs = Load(builder, 1);
                const auto result = Emit(builder,
                                         opcode,
                                         type,
                                         {Operand::ValueOf(lhs, type), Operand::ValueOf(rhs, type)},
                                         {FlagBits(Flag::All), profile});
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
                                    reserved | (answer & 255) | (packed_carry ? flags::Carry : 0) |
                                    (of ? flags::Overflow : 0) | (!answer ? flags::Zero : 0) |
                                    ((answer & sign) ? flags::Sign : 0) |
                                    (((a ^ b ^ answer) & 16) ? flags::Auxiliary : 0);
                            State state;
                            state.scalars[0] = a;
                            state.scalars[1] = b;
                            state.flags = reserved | (carry ? flags::Carry : 0);
                            if (Run(code, state, answer)) CHECK(state.flags == expected);
                        }
                    }
                }
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

TEST_CASE("FunctionLoop", "[compile][x64-compile][native]") { CHECK_NOTHROW(FunctionLoop()); }

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
