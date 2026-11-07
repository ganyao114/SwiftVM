#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/backend/loongarch64/allocate.h"
#include "runtime/backend/loongarch64/compile.h"
#include "runtime/backend/loongarch64/relocate.h"
#include "runtime/ir/builder.h"
#include "tests/support/check.h"
#include "tests/support/guest_memory.h"
#include "tests/support/ir_builder.h"
#include "tests/support/native_code.h"
#include "tests/support/test_mode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
#include <asm/unistd.h>
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
namespace la = swift::runtime::backend::loongarch64;
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

la::CompileOptions Options() {
    la::CompileOptions options;
    options.flags_offset = offsetof(State, flags);
    options.pc_offset = offsetof(State, pc);
    options.mxcsr_offset = offsetof(State, mxcsr);
    options.fault_locations = true;
    return options;
}

swift::runtime::backend::CompileResult Compile(const Program& program) {
    auto code = la::Compile(program, Options());
    if (!code)
        std::fprintf(stderr,
                     "%s: compile error=%u node=%u block=%u\n",
                     current_case,
                     unsigned(code.error),
                     code.node.value,
                     code.block.value);
    Check(bool(code) && !code.words.empty(), "LA64 compile rejected supported IR");
    Check(code.frame_bytes >= 112 && code.frame_bytes % 16 == 0, "native C ABI frame is aligned");
    for (const auto& fault : code.faults)
        Check(fault.first_offset < fault.last_offset && fault.last_offset <= code.words.size() * 4,
              "fault range belongs to actual emitted words");
    return code;
}

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
class NativeBlock {
public:
    explicit NativeBlock(const swift::runtime::backend::CompileResult& source)
            : code(source.words, swift::tests::NativeISA::LoongArch64) {}

    u64 Run(State& state) const {
        u64 previous;
        asm volatile("movfcsr2gr %0, $fcsr0" : "=r"(previous));
        // Deliberately disagree with guest rounding and set sticky status.
        // The unit must select guest controls and restore this exact word.
        const u64 host = (3u << 8) | 0x10000;
        asm volatile("movgr2fcsr $fcsr0, %0" ::"r"(host));
        const auto answer = code.Run(reinterpret_cast<std::uint8_t*>(&state));
        u64 after;
        asm volatile("movfcsr2gr %0, $fcsr0" : "=r"(after));
        asm volatile("movgr2fcsr $fcsr0, %0" ::"r"(previous));
        Check(after == host, "native return restores complete caller FCSR");
        return answer;
    }

private:
    swift::tests::NativeCode code;
};
#endif

bool Run(const swift::runtime::backend::CompileResult& code, State& state, u64 expected = 0) {
#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
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
    current_case = "V128 fixed pair spills";
    Builder builder{Scope::Block};
    Begin(builder);
    std::array<Value, 7> values;
    for (unsigned i = 0; i < values.size(); ++i) values[i] = LoadVector(builder, i);
    auto answer = values[0];
    for (unsigned i = 1; i < values.size(); ++i)
        answer = Emit(builder,
                      OpCode::VecXor,
                      Type::V128,
                      {Operand::ValueOf(answer), Operand::ValueOf(values[i])});
    StoreVector(builder, 7, answer);
    auto program = Return(builder);
    auto code = Compile(program);
    Check(code.spill_slots > 0, "simultaneous pairs exceed the physical bank");
    State state;
    Pair expected;
    for (unsigned i = 0; i < values.size(); ++i) {
        state.vectors[i] = {0x0101010101010101ULL * (i + 1),
                            0xfedcba9876543210ULL ^ (u64{i} << 33)};
        expected.low ^= state.vectors[i].low;
        expected.high ^= state.vectors[i].high;
    }
    if (Run(code, state))
        Check(state.vectors[7] == expected, "both halves reload from real spill homes");
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
        la::Allocation allocation;
        swift::runtime::backend::CompileResult code;
        la::Compile(program, Options(), code, allocation);
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
    const auto code = la::Compile(program, options);
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
                  "scalar add obeys guest FRM and preserves upper bits");
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
                  "min/max uses DAZ and preserves the selected raw value");
        }
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
#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
    if (NativeMode()) {
        asm volatile("movfcsr2gr %0, $fcsr0" : "=r"(helper_environment));
        asm volatile("movgr2fcsr $fcsr0, $zero");
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
        Check(state.vectors[1].low == 0x4b800001, "guest FRM is reinstalled after a host helper");
        Check(helper_environment == ((3u << 8) | 0x10000),
              "helper runs in the saved caller FP environment");
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

void AtomicMinMax() {
    current_case = "signed/unsigned atomic min/max";
    for (const auto type : {Type::U8, Type::U16, Type::U32, Type::U64}) {
        const auto bits = TypeBits(type);
        const auto mask = bits == 64 ? ~u64{} : (u64{1} << bits) - 1;
        alignas(8) u64 memory = UINT64_C(0x1122334400000000) & ~mask;
        const auto untouched = memory;
        Builder builder{Scope::Block};
        Begin(builder);
        const auto address = Literal(builder, reinterpret_cast<std::uintptr_t>(&memory));
        const auto zero = Literal(builder, 0);
        const std::array kinds{AtomicRMWOp::SignedMax,
                               AtomicRMWOp::SignedMin,
                               AtomicRMWOp::UnsignedMax,
                               AtomicRMWOp::UnsignedMin};
        const std::array operands{~u64{}, u64{} - 3, u64{5}, u64{4}};
        for (unsigned i = 0; i < kinds.size(); ++i) {
            const auto input = Literal(builder, operands[i]);
            const auto old = Emit(builder,
                                  OpCode::AtomicRMW,
                                  type,
                                  {Imm(static_cast<u64>(kinds[i]), Type::U8),
                                   Operand::ValueOf(address),
                                   Operand::ValueOf(input, type),
                                   Operand::ValueOf(zero)});
            Store(builder, i, old);
        }
        const auto final = Emit(builder, OpCode::LoadMemory, type, {Operand::ValueOf(address)});
        auto program = Return(builder, final);
        auto code = Compile(program);
        State state;
        if (Run(code, state, 4)) {
            Check(state.scalars[0] == 0 && state.scalars[1] == 0 && state.scalars[2] == mask - 2 &&
                          state.scalars[3] == mask - 2,
                  "atomic min/max compares signedness and returns the original narrow value");
            Check(memory == (untouched | 4), "subword reservation preserves neighboring memory");
        }
    }
}

void PersistentHelpers() {
    current_case = "cold helper symbol binding";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto zero = Literal(builder, 0), low = Literal(builder, 42), seven = Literal(builder, 7);
    const auto query =
            Emit(builder,
                 OpCode::Cpuid,
                 Type::U64,
                 {Operand::ValueOf(zero),
                  Operand::ValueOf(zero),
                  Imm(reinterpret_cast<std::uintptr_t>(&swift::runtime::x86::QueryGuestCpu))});
    const auto quotient = Emit(builder,
                               OpCode::Div128,
                               Type::U64,
                               {Operand::ValueOf(zero),
                                Operand::ValueOf(low),
                                Operand::ValueOf(seven),
                                Imm(0, Type::U8)});
    const auto answer = Emit(
            builder, OpCode::Add, Type::U64, {Operand::ValueOf(query), Operand::ValueOf(quotient)});
    auto program = Return(builder, answer);
    auto options = Options();
    options.relocatable = true;
    auto code = la::Compile(program, options);
    Check(bool(code) && code.relocatable && code.relocations.size() >= 2,
          "known CPU and wide divide addresses are recorded at their actual literals");
    auto original = code.words;
    auto bytes =
            std::span{reinterpret_cast<std::uint8_t*>(code.words.data()), code.words.size() * 4};
    Check(la::Relocate(bytes, code.relocations, options) && code.words == original,
          "cold symbol binding preserves a unit already bound to this process");
    auto bad = code.relocations.front();
    bad.symbol = static_cast<swift::runtime::backend::HostSymbol>(0xffff);
    Check(!la::Relocate(bytes, std::span{&bad, 1}, options), "unknown symbols reject installation");
    State state;
    Run(code, state, swift::runtime::x86::QueryGuestCpu(0, 0).lower + 6);
}
}  // namespace

TEST_CASE("ArithmeticFlags", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(ArithmeticFlags());
}

TEST_CASE("FunctionLoop", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(FunctionLoop());
}

TEST_CASE("PairSpills", "[compile][loongarch64-compile][native]") { CHECK_NOTHROW(PairSpills()); }

TEST_CASE("ShuffleBoundaries", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(ShuffleBoundaries());
}

TEST_CASE("SaturationAndExtension", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(SaturationAndExtension());
}

TEST_CASE("WideIntegerEdges", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(WideIntegerEdges());
}

TEST_CASE("LocalControlResults", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(LocalControlResults());
}

TEST_CASE("DynamicRounding", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(DynamicRounding());
}

TEST_CASE("FlushModes", "[compile][loongarch64-compile][native]") { CHECK_NOTHROW(FlushModes()); }

TEST_CASE("NaNsAndFusedArithmetic", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(NaNsAndFusedArithmetic());
}

TEST_CASE("HelperABI", "[compile][loongarch64-compile][native]") { CHECK_NOTHROW(HelperABI()); }

TEST_CASE("FaultMetadata", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(FaultMetadata());
}

TEST_CASE("AtomicMinMax", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(AtomicMinMax());
}

TEST_CASE("PersistentHelpers", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(PersistentHelpers());
}
