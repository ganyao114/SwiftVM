#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/backend/loongarch64/allocate.h"
#include "runtime/backend/loongarch64/assembler.h"
#include "runtime/backend/loongarch64/compile.h"
#include "runtime/backend/loongarch64/host_features.h"
#include "runtime/backend/loongarch64/relocate.h"
#include "runtime/execution/code_image.h"
#include "runtime/execution/host_backend.h"
#include "runtime/execution/runtime.h"
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
#include <elf.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/loongarch64_host.h"
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

void BitFieldsAndSelect() {
    current_case = "bitfield, bit test, rotate and branchless select";
    Builder builder{Scope::Block};
    Begin(builder);
    const auto x = Literal(builder, 0x0123456789ABCDEFULL);
    const auto y = Literal(builder, 0xAB);
    const auto x32 = Literal(builder, 0x89ABCDEFULL, Type::U32);
    const auto y32 = Literal(builder, 0x5A, Type::U32);
    Store(builder,
          0,
          Emit(builder, OpCode::BitExtract, Type::U64, {Operand::ValueOf(x), Imm(3), Imm(5)}));
    Store(builder,
          1,
          Emit(builder,
               OpCode::BitInsert,
               Type::U64,
               {Operand::ValueOf(x), Operand::ValueOf(y), Imm(8), Imm(8)}));
    Store(builder,
          2,
          Emit(builder, OpCode::BitClear, Type::U64, {Operand::ValueOf(x), Imm(8), Imm(8)}));
    Store(builder,
          3,
          Emit(builder, OpCode::BitExtract, Type::U32, {Operand::ValueOf(x32), Imm(3), Imm(5)}));
    Store(builder,
          4,
          Emit(builder,
               OpCode::BitInsert,
               Type::U32,
               {Operand::ValueOf(x32), Operand::ValueOf(y32), Imm(8), Imm(8)}));
    Store(builder,
          5,
          Emit(builder, OpCode::BitClear, Type::U32, {Operand::ValueOf(x32), Imm(8), Imm(8)}));
    Store(builder, 6, Emit(builder, OpCode::TestBit, Type::Bool, {Operand::ValueOf(x), Imm(63)}));
    Store(builder, 7, Emit(builder, OpCode::TestBit, Type::Bool, {Operand::ValueOf(x), Imm(0)}));
    Store(builder, 8, Emit(builder, OpCode::TestBit, Type::Bool, {Operand::ValueOf(x32), Imm(31)}));
    Store(builder, 9, Emit(builder, OpCode::RorImm, Type::U64, {Operand::ValueOf(x), Imm(13)}));
    Store(builder, 10, Emit(builder, OpCode::RorImm, Type::U32, {Operand::ValueOf(x32), Imm(5)}));
    const auto yes = Load(builder, 22), no = Load(builder, 23);
    const auto one = Load(builder, 20), zero = Load(builder, 21), high = Load(builder, 24);
    Store(builder,
          11,
          Emit(builder,
               OpCode::Select,
               Type::U64,
               {Operand::ValueOf(one), Operand::ValueOf(yes), Operand::ValueOf(no)}));
    Store(builder,
          12,
          Emit(builder,
               OpCode::Select,
               Type::U64,
               {Operand::ValueOf(zero), Operand::ValueOf(yes), Operand::ValueOf(no)}));
    Store(builder,
          13,
          Emit(builder,
               OpCode::SelectZero,
               Type::U64,
               {Operand::ValueOf(zero), Operand::ValueOf(yes), Operand::ValueOf(no)}));
    Store(builder,
          14,
          Emit(builder,
               OpCode::SelectZero,
               Type::U64,
               {Operand::ValueOf(one), Operand::ValueOf(yes), Operand::ValueOf(no)}));
    Store(builder,
          15,
          Emit(builder,
               OpCode::Select,
               Type::U64,
               {Operand::ValueOf(high), Operand::ValueOf(yes), Operand::ValueOf(no)}));
    const auto x16 = Literal(builder, 0xABCD, Type::U16);
    const auto y16 = Literal(builder, 0x5A, Type::U16);
    const auto x8 = Literal(builder, 0xCD, Type::U8);
    const auto y8 = Literal(builder, 0x3, Type::U8);
    Store(builder,
          16,
          Emit(builder, OpCode::BitExtract, Type::U16, {Operand::ValueOf(x16), Imm(3), Imm(5)}));
    Store(builder,
          17,
          Emit(builder,
               OpCode::BitInsert,
               Type::U16,
               {Operand::ValueOf(x16), Operand::ValueOf(y16), Imm(8), Imm(8)}));
    Store(builder,
          18,
          Emit(builder, OpCode::BitClear, Type::U16, {Operand::ValueOf(x16), Imm(8), Imm(8)}));
    Store(builder,
          19,
          Emit(builder, OpCode::BitExtract, Type::U8, {Operand::ValueOf(x8), Imm(4), Imm(4)}));
    Store(builder,
          25,
          Emit(builder,
               OpCode::BitInsert,
               Type::U8,
               {Operand::ValueOf(x8), Operand::ValueOf(y8), Imm(0), Imm(4)}));
    Store(builder,
          26,
          Emit(builder, OpCode::BitClear, Type::U8, {Operand::ValueOf(x8), Imm(4), Imm(4)}));
    Store(builder,
          27,
          Emit(builder, OpCode::BitExtract, Type::U64, {Operand::ValueOf(x), Imm(8), Imm(0)}));
    Store(builder,
          28,
          Emit(builder,
               OpCode::BitInsert,
               Type::U16,
               {Operand::ValueOf(x16), Operand::ValueOf(y16), Imm(4), Imm(0)}));
    Store(builder,
          29,
          Emit(builder, OpCode::BitClear, Type::U16, {Operand::ValueOf(x16), Imm(4), Imm(0)}));
    auto program = Return(builder);
    auto code = Compile(program);
    State state;
    state.scalars[20] = 1;
    state.scalars[21] = 0;
    state.scalars[22] = 0x1111111111111111ULL;
    state.scalars[23] = 0x2222222222222222ULL;
    state.scalars[24] = 0x100000000ULL;
    if (Run(code, state)) {
        Check(state.scalars[0] == 0x1D, "64-bit bitfield extract");
        Check(state.scalars[1] == 0x0123456789ABABEFULL, "64-bit bitfield insert");
        Check(state.scalars[2] == 0x0123456789AB00EFULL, "64-bit bitfield clear");
        Check(state.scalars[3] == 0x1D, "32-bit bitfield extract");
        Check(state.scalars[4] == 0x89AB5AEFULL, "32-bit bitfield insert zero-extends");
        Check(state.scalars[5] == 0x89AB00EFULL, "32-bit bitfield clear zero-extends");
        Check(state.scalars[6] == 0 && state.scalars[7] == 1, "bit test reads the selected lane");
        Check(state.scalars[8] == 1, "32-bit bit test reads bit 31");
        Check(state.scalars[9] == 0x6F78091A2B3C4D5EULL, "64-bit immediate rotate");
        Check(state.scalars[10] == 0x7C4D5E6FULL, "32-bit immediate rotate zero-extends");
        Check(state.scalars[11] == 0x1111111111111111ULL, "select takes the nonzero arm");
        Check(state.scalars[12] == 0x2222222222222222ULL, "select takes the zero arm");
        Check(state.scalars[13] == 0x1111111111111111ULL, "select-zero takes the zero arm");
        Check(state.scalars[14] == 0x2222222222222222ULL, "select-zero takes the nonzero arm");
        Check(state.scalars[15] == 0x1111111111111111ULL,
              "select treats a high-bit condition as true");
        Check(state.scalars[16] == 0x19, "16-bit bitfield extract");
        Check(state.scalars[17] == 0x5ACD, "16-bit bitfield insert");
        Check(state.scalars[18] == 0x00CD, "16-bit bitfield clear");
        Check(state.scalars[19] == 0x0C, "8-bit bitfield extract");
        Check(state.scalars[25] == 0xC3, "8-bit bitfield insert");
        Check(state.scalars[26] == 0x0D, "8-bit bitfield clear");
        Check(state.scalars[27] == 0, "zero-width extract");
        Check(state.scalars[28] == 0xABCD && state.scalars[29] == 0xABCD,
              "zero-width insert and clear preserve the masked parent");
    }
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

TEST_CASE("BitFieldsAndSelect", "[compile][loongarch64-compile][native]") {
    CHECK_NOTHROW(BitFieldsAndSelect());
}

TEST_CASE("Guest safepoint requires a linked execution context", "[compile][loongarch64-compile]") {
    for (auto scope : {Scope::Block, Scope::Function}) {
        Builder builder{scope};
        Begin(builder);
        Emit(builder, OpCode::GuestSafepoint, Type::Void, {Imm(0x1234)});
        auto program = Return(builder, Literal(builder, 42));
        CHECK(!la::Compile(program, Options()));
        auto options = Options();
        options.dispatcher = 0x1000;
        options.exit_handler = 0x2000;
        const auto code = la::Compile(program, options);
        REQUIRE(bool(code));
        CHECK(!code.words.empty() && code.faults.empty() && code.links.empty());
    }
}

TEST_CASE("LBT and LA664 assembler encodings match Binutils 2.44 word-for-word",
          "[compile][loongarch64-compile]") {
    std::vector<u32> words;
    la::Assembler as{words};
    // Exact sequence assembled on Loongson-3A6000 with GNU Binutils 2.44 ($a0=4, $a1=5, $a2=6).
    as.X86ADD_B(la::a1, la::a2);
    as.X86ADD_H(la::a1, la::a2);
    as.X86ADD_W(la::a1, la::a2);
    as.X86ADD_D(la::a1, la::a2);
    as.X86SUB_B(la::a1, la::a2);
    as.X86SUB_H(la::a1, la::a2);
    as.X86SUB_W(la::a1, la::a2);
    as.X86SUB_D(la::a1, la::a2);
    as.X86ADC_B(la::a1, la::a2);
    as.X86ADC_H(la::a1, la::a2);
    as.X86ADC_W(la::a1, la::a2);
    as.X86ADC_D(la::a1, la::a2);
    as.X86SBC_B(la::a1, la::a2);
    as.X86SBC_H(la::a1, la::a2);
    as.X86SBC_W(la::a1, la::a2);
    as.X86SBC_D(la::a1, la::a2);
    as.X86AND_B(la::a1, la::a2);
    as.X86AND_H(la::a1, la::a2);
    as.X86AND_W(la::a1, la::a2);
    as.X86AND_D(la::a1, la::a2);
    as.X86OR_B(la::a1, la::a2);
    as.X86OR_H(la::a1, la::a2);
    as.X86OR_W(la::a1, la::a2);
    as.X86OR_D(la::a1, la::a2);
    as.X86XOR_B(la::a1, la::a2);
    as.X86XOR_H(la::a1, la::a2);
    as.X86XOR_W(la::a1, la::a2);
    as.X86XOR_D(la::a1, la::a2);
    as.X86INC_B(la::a1);
    as.X86INC_H(la::a1);
    as.X86INC_W(la::a1);
    as.X86INC_D(la::a1);
    as.X86DEC_B(la::a1);
    as.X86DEC_H(la::a1);
    as.X86DEC_W(la::a1);
    as.X86DEC_D(la::a1);
    as.X86SLL_B(la::a1, la::a2);
    as.X86SLL_H(la::a1, la::a2);
    as.X86SLL_W(la::a1, la::a2);
    as.X86SLL_D(la::a1, la::a2);
    as.X86SRL_B(la::a1, la::a2);
    as.X86SRL_H(la::a1, la::a2);
    as.X86SRL_W(la::a1, la::a2);
    as.X86SRL_D(la::a1, la::a2);
    as.X86SRA_B(la::a1, la::a2);
    as.X86SRA_H(la::a1, la::a2);
    as.X86SRA_W(la::a1, la::a2);
    as.X86SRA_D(la::a1, la::a2);
    as.X86ROTR_B(la::a1, la::a2);
    as.X86ROTR_H(la::a1, la::a2);
    as.X86ROTR_W(la::a1, la::a2);
    as.X86ROTR_D(la::a1, la::a2);
    as.X86ROTL_B(la::a1, la::a2);
    as.X86ROTL_H(la::a1, la::a2);
    as.X86ROTL_W(la::a1, la::a2);
    as.X86ROTL_D(la::a1, la::a2);
    as.X86SLLI_B(la::a1, 3);
    as.X86SLLI_H(la::a1, 3);
    as.X86SLLI_W(la::a1, 3);
    as.X86SLLI_D(la::a1, 3);
    as.X86SRLI_B(la::a1, 3);
    as.X86SRLI_H(la::a1, 3);
    as.X86SRLI_W(la::a1, 3);
    as.X86SRLI_D(la::a1, 3);
    as.X86SRAI_B(la::a1, 3);
    as.X86SRAI_H(la::a1, 3);
    as.X86SRAI_W(la::a1, 3);
    as.X86SRAI_D(la::a1, 3);
    as.X86ROTRI_B(la::a1, 3);
    as.X86ROTRI_H(la::a1, 3);
    as.X86ROTRI_W(la::a1, 3);
    as.X86ROTRI_D(la::a1, 3);
    as.X86ROTLI_B(la::a1, 3);
    as.X86ROTLI_H(la::a1, 3);
    as.X86ROTLI_W(la::a1, 3);
    as.X86ROTLI_D(la::a1, 3);
    as.ADC_B(la::a0, la::a1, la::a2);
    as.ADC_H(la::a0, la::a1, la::a2);
    as.ADC_W(la::a0, la::a1, la::a2);
    as.ADC_D(la::a0, la::a1, la::a2);
    as.SBC_B(la::a0, la::a1, la::a2);
    as.SBC_H(la::a0, la::a1, la::a2);
    as.SBC_W(la::a0, la::a1, la::a2);
    as.SBC_D(la::a0, la::a1, la::a2);
    as.ROTR_B(la::a0, la::a1, la::a2);
    as.ROTR_H(la::a0, la::a1, la::a2);
    as.ROTR_W(la::a0, la::a1, la::a2);
    as.ROTR_D(la::a0, la::a1, la::a2);
    as.ROTRI_B(la::a0, la::a1, 3);
    as.ROTRI_H(la::a0, la::a1, 3);
    as.ROTRI_W(la::a0, la::a1, 3);
    as.ROTRI_D(la::a0, la::a1, 3);
    as.X86MFFLAG(la::a0, 0x2a);
    as.X86MFFLAG(la::a0, 0x3f);
    as.X86MTFLAG(la::a0, 0x2a);
    as.X86MTFLAG(la::a0, 0x3f);
    as.SETX86J(la::a0, 0);
    as.SETX86J(la::a0, 9);
    as.SETX86J(la::a0, 15);
    as.CRCC_W_B_W(la::a0, la::a1, la::a2);
    as.CRCC_W_H_W(la::a0, la::a1, la::a2);
    as.CRCC_W_W_W(la::a0, la::a1, la::a2);
    as.CRCC_W_D_W(la::a0, la::a1, la::a2);
    as.EXT_W_B(la::a0, la::a1);
    as.EXT_W_H(la::a0, la::a1);
    as.ADDI_W(la::a0, la::a1, 0);
    as.ALSL_D(la::a0, la::a1, la::a2, 3);

    constexpr std::array<u32, 107> expected{
            0x003f18a4u, 0x003f18a5u, 0x003f18a6u, 0x003f18a7u,  // x86add
            0x003f18a8u, 0x003f18a9u, 0x003f18aau, 0x003f18abu,  // x86sub
            0x003f18acu, 0x003f18adu, 0x003f18aeu, 0x003f18afu,  // x86adc
            0x003f18b0u, 0x003f18b1u, 0x003f18b2u, 0x003f18b3u,  // x86sbc
            0x003f98b0u, 0x003f98b1u, 0x003f98b2u, 0x003f98b3u,  // x86and
            0x003f98b4u, 0x003f98b5u, 0x003f98b6u, 0x003f98b7u,  // x86or
            0x003f98b8u, 0x003f98b9u, 0x003f98bau, 0x003f98bbu,  // x86xor
            0x000080a0u, 0x000080a1u, 0x000080a2u, 0x000080a3u,  // x86inc
            0x000080a4u, 0x000080a5u, 0x000080a6u, 0x000080a7u,  // x86dec
            0x003f18b4u, 0x003f18b5u, 0x003f18b6u, 0x003f18b7u,  // x86sll
            0x003f18b8u, 0x003f18b9u, 0x003f18bau, 0x003f18bbu,  // x86srl
            0x003f18bcu, 0x003f18bdu, 0x003f18beu, 0x003f18bfu,  // x86sra
            0x003f98a0u, 0x003f98a1u, 0x003f98a3u, 0x003f98a2u,  // x86rotr (.w/.d swapped in ISA)
            0x003f98a4u, 0x003f98a5u, 0x003f98a6u, 0x003f98a7u,  // x86rotl
            0x00542ca0u, 0x00544ca1u, 0x00548ca2u, 0x00550ca3u,  // x86slli
            0x00542ca4u, 0x00544ca5u, 0x00548ca6u, 0x00550ca7u,  // x86srli
            0x00542ca8u, 0x00544ca9u, 0x00548caau, 0x00550cabu,  // x86srai
            0x00542cacu, 0x00544cadu, 0x00548caeu, 0x00550cafu,  // x86rotri
            0x00542cb4u, 0x00544cb5u, 0x00548cb6u, 0x00550cb7u,  // x86rotli
            0x003018a4u, 0x003098a4u, 0x003118a4u, 0x003198a4u,  // adc
            0x003218a4u, 0x003298a4u, 0x003318a4u, 0x003398a4u,  // sbc
            0x001a18a4u, 0x001a98a4u, 0x001b18a4u, 0x001b98a4u,  // rotr
            0x004c2ca4u, 0x004c4ca4u, 0x004c8ca4u, 0x004d0ca4u,  // rotri
            0x005ca804u, 0x005cfc04u,                            // x86mfflag
            0x005ca824u, 0x005cfc24u,                            // x86mtflag
            0x00368004u, 0x0036a404u, 0x0036bc04u,               // setx86j
            0x002618a4u, 0x002698a4u, 0x002718a4u, 0x002798a4u,  // crcc.w.*.w
            0x00005ca4u, 0x000058a4u, 0x028000a4u, 0x002d18a4u,  // ext.w.*, addi.w, alsl.d
    };
    REQUIRE(words.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("instruction index " << i);
        CHECK(words[i] == expected[i]);
    }
}

TEST_CASE("HostFeatures dual-gate policy and CodeImageKey capability isolation",
          "[compile][loongarch64-compile][native]") {
    constexpr std::uint64_t hwcap_all = (1ULL << 2) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) |
                                        (1ULL << 10) | (1ULL << 11) | (1ULL << 12);
    constexpr std::uint32_t cpucfg2_lbt = (1u << 18) | (1u << 19) | (1u << 20);

    // LBT requires BOTH kernel HWCAP (context switch) and CPUCFG2 hardware bit.
    const auto kernel_only = la::HostFeatures::FromHWCAPAndCPUCFG2(hwcap_all, 0);
    CHECK(!kernel_only.lbt_x86);
    CHECK(!kernel_only.lbt_arm);
    CHECK(!kernel_only.lbt_mips);
    CHECK(kernel_only.ual);
    CHECK(kernel_only.lsx);
    CHECK(kernel_only.crc32);

    const auto cpu_only = la::HostFeatures::FromHWCAPAndCPUCFG2(0, cpucfg2_lbt);
    CHECK(!cpu_only.lbt_x86);
    CHECK(!cpu_only.lbt_arm);

    const auto both = la::HostFeatures::FromHWCAPAndCPUCFG2(hwcap_all, cpucfg2_lbt);
    CHECK(both.lbt_x86);
    CHECK(both.lbt_arm);
    CHECK(both.lbt_mips);

    // Environment policy override never enables features absent from hardware.
    const auto off = la::HostFeatures::ApplyEnvOverride("off", both);
    CHECK(off.IsaFeatureMask() == 0);
    const auto only_lbt = la::HostFeatures::ApplyEnvOverride("off, +lbt_x86", both);
    CHECK(only_lbt.lbt_x86);
    CHECK(!only_lbt.ual);
    CHECK(!only_lbt.lsx);
    CHECK(!only_lbt.crc32);
    const auto denied_by_hw = la::HostFeatures::ApplyEnvOverride("off, +lbt_x86", kernel_only);
    CHECK(!denied_by_hw.lbt_x86);
    const auto no_lsx = la::HostFeatures::ApplyEnvOverride("-lsx, -lbt", both);
    CHECK(!no_lsx.lsx);
    CHECK(!no_lsx.lasx);
    CHECK(!no_lsx.lbt_x86);
    CHECK(no_lsx.ual);

    // CodeImageKey must isolate artifacts built with different LA64 capability masks.
    namespace ex = swift::runtime::execution;
    swift::Digest guest{};
    guest[0] = 0x5a;
    ex::RuntimeOptions runtime_options;
    ex::HostBackendOps baseline_ops;
    baseline_ops.target = la::KnownTargetInfo();
    baseline_ops.isa_features = off.IsaFeatureMask();
    ex::HostBackendOps lbt_ops = baseline_ops;
    lbt_ops.isa_features = only_lbt.IsaFeatureMask();
    lbt_ops.capabilities.native_eflags = true;
    const auto key_off = ex::CodeImageKeyDigest(
            ex::MakeCodeImageKey(guest, 0, baseline_ops, runtime_options, swift::runtime::GuestISA::X86_64));
    const auto key_lbt = ex::CodeImageKeyDigest(
            ex::MakeCodeImageKey(guest, 0, lbt_ops, runtime_options, swift::runtime::GuestISA::X86_64));
    CHECK(key_off != key_lbt);

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
    if (NativeMode()) {
        const auto live = la::HostFeatures::DetectHardware();
        // Verify AT_HWCAP & CPUCFG2 probe runs cleanly without SIGILL on real hardware.
        CHECK((live.IsaFeatureMask() & ~0x7fULL) == 0);
    }
#endif
}

namespace {
std::uint64_t HelperPreserveFlagsCallback(std::uint64_t x) { return x * 3 + 7; }

Program BuildAllConditionsAndTestsProgram(OpCode op,
                                          Type width,
                                          FlagProfile profile,
                                          u64 flag_outputs,
                                          bool imm_rhs_one = false) {
    Builder b{Scope::Block};
    Begin(b);
    const auto lhs_raw = Load(b, 0);
    const auto rhs_raw = Load(b, 1);
    const Operand lhs = Operand::ValueOf(lhs_raw, width);
    const Operand rhs = imm_rhs_one ? Imm(1, width) : Operand::ValueOf(rhs_raw, width);
    Value res{};
    if (op == OpCode::Neg) {
        res = Emit(b, op, width, {lhs}, {flag_outputs, profile});
    } else {
        res = Emit(b, op, width, {lhs, rhs}, {flag_outputs, profile});
    }
    // Pack all 16 Condition outputs into scalar[2]
    Value cond_pack = Literal(b, 0);
    for (unsigned c = 0; c < 16; ++c) {
        const auto bit = Emit(b,
                              OpCode::CondSet,
                              Type::Bool,
                              {Operand::Condition(static_cast<Condition>(c))});
        const auto bit64 = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(bit)});
        const auto shifted = Emit(b, OpCode::LslImm, Type::U64, {Operand::ValueOf(bit64), Imm(c)});
        cond_pack = Emit(
                b, OpCode::Or, Type::U64, {Operand::ValueOf(cond_pack), Operand::ValueOf(shifted)});
    }
    Store(b, 2, cond_pack);

    // Pack TestFlags & TestNotFlags across key masks into scalar[3]
    const std::array<u64, 7> masks{
            FlagBits(Flag::Zero),
            FlagBits(Flag::Carry),
            FlagBits(Flag::Carry) | FlagBits(Flag::Zero),
            FlagBits(Flag::Parity),
            FlagBits(Flag::Negate),
            FlagBits(Flag::Overflow),
            FlagBits(Flag::Direction) | FlagBits(Flag::Carry),
    };
    Value test_pack = Literal(b, 0);
    for (unsigned i = 0; i < masks.size(); ++i) {
        const auto tf = Emit(b, OpCode::TestFlags, Type::Bool, {Operand::Flags(masks[i])});
        const auto tnf = Emit(b, OpCode::TestNotFlags, Type::Bool, {Operand::Flags(masks[i])});
        const auto tf64 = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(tf)});
        const auto tnf64 = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(tnf)});
        const auto s0 = Emit(b, OpCode::LslImm, Type::U64, {Operand::ValueOf(tf64), Imm(i * 2)});
        const auto s1 =
                Emit(b, OpCode::LslImm, Type::U64, {Operand::ValueOf(tnf64), Imm(i * 2 + 1)});
        test_pack =
                Emit(b, OpCode::Or, Type::U64, {Operand::ValueOf(test_pack), Operand::ValueOf(s0)});
        test_pack =
                Emit(b, OpCode::Or, Type::U64, {Operand::ValueOf(test_pack), Operand::ValueOf(s1)});
    }
    Store(b, 3, test_pack);

    // Scalar CondSelect on HI and LE
    const auto v_hi = Literal(b, 0x5a, width);
    const auto v_lo = Literal(b, 0xa5, width);
    const auto sel_hi = Emit(b,
                             OpCode::CondSelect,
                             width,
                             {Operand::Condition(Condition::HI),
                              Operand::ValueOf(v_hi),
                              Operand::ValueOf(v_lo)});
    const auto sel_le = Emit(b,
                             OpCode::CondSelect,
                             width,
                             {Operand::Condition(Condition::LE),
                              Operand::ValueOf(v_hi),
                              Operand::ValueOf(v_lo)});
    const auto sel_hi64 = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(sel_hi)});
    const auto sel_le64 = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(sel_le)});
    const auto sel_comb = Emit(
            b,
            OpCode::Or,
            Type::U64,
            {Operand::ValueOf(sel_hi64),
             Operand::ValueOf(Emit(b, OpCode::LslImm, Type::U64, {Operand::ValueOf(sel_le64), Imm(32)}))});
    Store(b, 4, sel_comb);

    const auto snap = Emit(b, OpCode::GetFlags, Type::U64, {Operand::ValueOf(res), Operand::Flags(Flag::All)});
    Store(b, 5, snap);
    return Return(b, res);
}

bool HasLbtOpcode(const std::vector<u32>& words) {
    for (const u32 w : words) {
        // x86add..x86sra (0x003f0000), x86rotr..x86xor (0x003f8000),
        // x86inc/dec (0x00008000), setx86j (0x00368000), x86mf/mtflag (0x005c0000)
        if ((w & 0xffff8000u) == 0x003f0000u || (w & 0xffff8000u) == 0x003f8000u ||
            (w & 0xfffffc00u) == 0x00008000u || (w & 0xffffc000u) == 0x00368000u ||
            (w & 0xfffc0000u) == 0x005c0000u) {
            return true;
        }
    }
    return false;
}
}  // namespace

TEST_CASE("LBT flags engine matches software reference across widths, profiles, and CFGs",
          "[compile][loongarch64-compile][native]") {
    auto sw_opts = Options();
    sw_opts.lbt_x86 = false;
    auto lbt_opts = Options();
    lbt_opts.lbt_x86 = true;

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
    const bool run_lbt_native = NativeMode() && la::HostFeatures::DetectHardware().lbt_x86;
#else
    const bool run_lbt_native = false;
#endif

    const std::array<Type, 4> widths{Type::U8, Type::U16, Type::U32, Type::U64};
    const std::array<FlagProfile, 2> profiles{FlagProfile::ArithmeticCanonicalCarry,
                                              FlagProfile::ArithmeticRaw};
    const std::array<OpCode, 9> ops{OpCode::Add,
                                    OpCode::Sub,
                                    OpCode::Neg,
                                    OpCode::Adc,
                                    OpCode::Sbb,
                                    OpCode::And,
                                    OpCode::Or,
                                    OpCode::Xor,
                                    OpCode::AndNot};

    for (const auto width : widths) {
        const unsigned bits = TypeBits(width);
        const u64 max_u = bits == 64 ? ~u64{0} : ((u64{1} << bits) - 1);
        const u64 sign_u = u64{1} << (bits - 1);
        const std::array<u64, 6> samples{
                0u, 1u, 0x0fu, sign_u - 1, sign_u, max_u};

        for (const auto profile : profiles) {
            for (const auto op : ops) {
                const auto prog =
                        BuildAllConditionsAndTestsProgram(op, width, profile, FlagBits(Flag::All));
                const auto code_sw = la::Compile(prog, sw_opts);
                const auto code_lbt = la::Compile(prog, lbt_opts);
                REQUIRE(bool(code_sw));
                REQUIRE(bool(code_lbt));
                CHECK(!HasLbtOpcode(code_sw.words));
                CHECK(HasLbtOpcode(code_lbt.words));
                CHECK(code_lbt.words.size() < code_sw.words.size());

                if (run_lbt_native) {
#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
                    NativeBlock nat_sw{code_sw};
                    NativeBlock nat_lbt{code_lbt};
                    for (const u64 init_cf : {u64{0}, flags::Carry}) {
                        for (const u64 a : samples) {
                            for (const u64 b : samples) {
                                State s_sw{};
                                s_sw.scalars[0] = a;
                                s_sw.scalars[1] = b;
                                s_sw.flags = (u64{1} << 55) | flags::Direction | init_cf | 1u;
                                State s_lbt = s_sw;
                                const u64 r_sw = nat_sw.Run(s_sw);
                                const u64 r_lbt = nat_lbt.Run(s_lbt);
                                CHECK(r_lbt == r_sw);
                                CHECK(s_lbt.scalars[2] == s_sw.scalars[2]);
                                CHECK(s_lbt.scalars[3] == s_sw.scalars[3]);
                                CHECK(s_lbt.scalars[4] == s_sw.scalars[4]);
                                CHECK(flags::ToRflags(s_lbt.flags) == flags::ToRflags(s_sw.flags));
                                CHECK((s_lbt.flags & ~u64{255}) == (s_sw.flags & ~u64{255}));
                                CHECK(flags::ToRflags(s_lbt.scalars[5]) ==
                                      flags::ToRflags(s_sw.scalars[5]));
                                if (op == OpCode::Neg) break;
                            }
                        }
                    }
#endif
                }
            }
        }

        // Inc / Dec (0x3e: All & ~Carry) preserving incoming Carry, plus ARM NZCV-only preserving [7:0]
        for (const auto op : {OpCode::Add, OpCode::Sub}) {
            const auto inc_prog = BuildAllConditionsAndTestsProgram(
                    op,
                    width,
                    FlagProfile::ArithmeticCanonicalCarry,
                    FlagBits(Flag::All) & ~FlagBits(Flag::Carry),
                    true);
            const auto inc_sw = la::Compile(inc_prog, sw_opts);
            const auto inc_lbt = la::Compile(inc_prog, lbt_opts);
            REQUIRE(bool(inc_sw));
            REQUIRE(bool(inc_lbt));
            CHECK(HasLbtOpcode(inc_lbt.words));

            const u64 nzcv_mask = FlagBits(Flag::Carry) | FlagBits(Flag::Overflow) |
                                  FlagBits(Flag::Zero) | FlagBits(Flag::Negate);
            const auto nzcv_prog = BuildAllConditionsAndTestsProgram(
                    op, width, FlagProfile::ArithmeticRaw, nzcv_mask);
            const auto nzcv_sw = la::Compile(nzcv_prog, sw_opts);
            const auto nzcv_lbt = la::Compile(nzcv_prog, lbt_opts);
            REQUIRE(bool(nzcv_sw));
            REQUIRE(bool(nzcv_lbt));

            if (run_lbt_native) {
#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
                NativeBlock nat_inc_sw{inc_sw}, nat_inc_lbt{inc_lbt};
                NativeBlock nat_nzcv_sw{nzcv_sw}, nat_nzcv_lbt{nzcv_lbt};
                for (const u64 init_cf : {u64{0}, flags::Carry}) {
                    for (const u64 a : samples) {
                        State s_sw{}, s_lbt{};
                        s_sw.scalars[0] = a;
                        s_sw.flags = (u64{1} << 55) | init_cf | 1u;
                        s_lbt = s_sw;
                        CHECK(nat_inc_lbt.Run(s_lbt) == nat_inc_sw.Run(s_sw));
                        CHECK(s_lbt.scalars[2] == s_sw.scalars[2]);
                        CHECK(flags::ToRflags(s_lbt.flags) == flags::ToRflags(s_sw.flags));

                        State arm_sw{}, arm_lbt{};
                        arm_sw.scalars[0] = a;
                        arm_sw.scalars[1] = sign_u;
                        arm_sw.flags = (u64{1} << 55) | 0x25u | init_cf;
                        arm_lbt = arm_sw;
                        CHECK(nat_nzcv_lbt.Run(arm_lbt) == nat_nzcv_sw.Run(arm_sw));
                        // Untouched low byte 0x25 and bit 55 must match bit-for-bit!
                        CHECK(arm_lbt.flags == arm_sw.flags);
                        CHECK(arm_lbt.scalars[2] == arm_sw.scalars[2]);
                    }
                }
#endif
            }
        }
    }

    // Multi-block Scope::Function loop with ArithmeticRaw carry_xor, InvertCarry, PublishFCmpFlags,
    // SetCarry/SetOverflow/ClearFlags, Goto/BindLabel, and CallLambda helper preservation.
    {
        Builder b{Scope::Function};
        const auto entry = Block(b), loop = Block(b), done = Block(b);
        Need(b.SetEntry(entry));
        Need(b.BeginBlock(entry));
        const auto init_fcmp = Load(b, 2);
        Emit(b, OpCode::PublishFCmpFlags, Type::Void, {Operand::ValueOf(init_fcmp)});
        const auto fcmp_ja_b = Emit(
                b, OpCode::TestNotFlags, Type::Bool, {Operand::Flags(FlagBits(Flag::Carry) | FlagBits(Flag::Zero))});
        const auto fcmp_jp_b =
                Emit(b, OpCode::TestFlags, Type::Bool, {Operand::Flags(FlagBits(Flag::Parity))});
        const auto fcmp_ja = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(fcmp_ja_b)});
        const auto fcmp_jp = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(fcmp_jp_b)});
        Store(b, 3, Emit(b, OpCode::Add, Type::U64, {Operand::ValueOf(fcmp_ja), Operand::ValueOf(Emit(b, OpCode::LslImm, Type::U64, {Operand::ValueOf(fcmp_jp), Imm(4)}))}));
        Need(b.EndBlock(Terminal::Jump(loop)));

        Need(b.BeginBlock(loop));
        const auto cnt = Load(b, 0);
        const auto acc = Load(b, 1);
        const auto one = Literal(b, 1);
        const auto next_cnt = Emit(b,
                                   OpCode::Sub,
                                   Type::U64,
                                   {Operand::ValueOf(cnt), Operand::ValueOf(one)},
                                   {FlagBits(Flag::All), FlagProfile::ArithmeticRaw});
        Store(b, 0, next_cnt);
        // CallLambda in the middle of live carry_xor=true LBT state!
        const auto helper_val = Emit(
                b,
                OpCode::CallLambda,
                Type::U64,
                {Operand::Lambda(0),
                 Imm(reinterpret_cast<std::uintptr_t>(&HelperPreserveFlagsCallback)),
                 Operand::ValueOf(acc)});
        // Consume flags AFTER CallLambda via Condition::HI (tests both carry_xor & CallRegister!)
        const auto hi_bit_b =
                Emit(b, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::HI)});
        const auto hi_bit = Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(hi_bit_b)});
        const auto next_acc = Emit(
                b, OpCode::Add, Type::U64, {Operand::ValueOf(helper_val), Operand::ValueOf(hi_bit)});
        Store(b, 1, next_acc);
        // Local Goto / BindLabel inside loop block
        const auto skip = Emit(b, OpCode::Goto, Type::Bool, {Operand::ValueOf(hi_bit)});
        Emit(b, OpCode::InvertCarry, Type::Void, {});
        Emit(b, OpCode::BindLabel, Type::Void, {Operand::ValueOf(skip)});
        const auto keep_going =
                Emit(b, OpCode::CondSet, Type::Bool, {Operand::Condition(Condition::NE)});
        Need(b.EndBlock(Terminal::Branch(keep_going, loop, done)));

        Need(b.BeginBlock(done));
        // Chained Sbb reading carry across CFG edge!
        const auto final_acc = Load(b, 1);
        const auto borrow_adj = Emit(b,
                                     OpCode::Sbb,
                                     Type::U64,
                                     {Operand::ValueOf(final_acc), Operand::ValueOf(Literal(b, 0))},
                                     {FlagBits(Flag::All), FlagProfile::ArithmeticRaw});
        const auto prog = Return(b, borrow_adj);
        const auto fn_sw = la::Compile(prog, sw_opts);
        const auto fn_lbt = la::Compile(prog, lbt_opts);
        REQUIRE(bool(fn_sw));
        REQUIRE(bool(fn_lbt));
        if (run_lbt_native) {
#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
            NativeBlock nat_sw{fn_sw}, nat_lbt{fn_lbt};
            for (const u64 fcmp_code : {u64{0}, u64{1}, u64{4}, u64{7}}) {
                State s_sw{}, s_lbt{};
                s_sw.scalars[0] = 5;
                s_sw.scalars[1] = 2;
                s_sw.scalars[2] = fcmp_code;
                s_sw.flags = flags::Direction;
                s_lbt = s_sw;
                CHECK(nat_lbt.Run(s_lbt) == nat_sw.Run(s_sw));
                CHECK(s_lbt.scalars[0] == s_sw.scalars[0]);
                CHECK(s_lbt.scalars[1] == s_sw.scalars[1]);
                CHECK(s_lbt.scalars[3] == s_sw.scalars[3]);
                CHECK(flags::ToRflags(s_lbt.flags) == flags::ToRflags(s_sw.flags));
            }
#endif
        }
    }
}

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
namespace {
std::vector<std::uint8_t> MakeTinyElf64(unsigned machine, std::span<const std::uint8_t> code) {
    constexpr std::uint64_t base = 0x400000;
    constexpr std::size_t hdrs = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    Elf64_Ehdr eh{};
    std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_EXEC;
    eh.e_machine = machine;
    eh.e_version = EV_CURRENT;
    eh.e_entry = base + hdrs;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 1;
    Elf64_Phdr ph{};
    ph.p_type = PT_LOAD;
    ph.p_flags = PF_R | PF_X;
    ph.p_vaddr = ph.p_paddr = base;
    ph.p_filesz = ph.p_memsz = hdrs + code.size();
    ph.p_align = 4096;
    std::vector<std::uint8_t> out(ph.p_filesz);
    std::memcpy(out.data(), &eh, sizeof(eh));
    std::memcpy(out.data() + sizeof(eh), &ph, sizeof(ph));
    std::memcpy(out.data() + hdrs, code.data(), code.size());
    return out;
}
}  // namespace

TEST_CASE("LBT synchronous guest fault recovery reconstructs exact live flags from sigcontext",
          "[native][loongarch64-compile]") {
    if (!NativeMode() || !la::HostFeatures::DetectHardware().lbt_x86) return;
    namespace ex = swift::runtime::execution;

    // 1. x86_64 guest: sub rax, 1 (0 - 1 = -1 -> CF=1, PF=1, AF=1, SF=1, ZF=0, OF=0);
    //    mov rax, [rcx] (faults on PROT_NONE page before block exit so $r31 is still stale!); ret
    {
        const std::array<std::uint8_t, 8> x86_bytes{
                0x48, 0x83, 0xe8, 0x01,  // sub rax, 1
                0x48, 0x8b, 0x01,        // mov rax, [rcx]
                0xc3                     // ret
        };
        const auto elf = MakeTinyElf64(EM_X86_64, x86_bytes);
        ex::RuntimeOptions opts;
        ex::RuntimeStats stats;
        swift::runtime::platform::Loongarch64Host host{false};
        const auto backend = host.Bind();
        CHECK(backend.capabilities.native_eflags == la::HostFeatures::Detect().lbt_x86);
        ex::VM vm{opts, stats, backend};
        vm.PrepareGuest(elf, "lbt-x86-fault-test");
        const auto bad_page = vm.Memory().Map(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
        REQUIRE(bad_page != UINT64_MAX);
        const auto entry = vm.State().pc;
        vm.State().gprs[swift::runtime::x86::RAX] = 0;
        vm.State().gprs[swift::runtime::x86::RCX] = bad_page;
        // Seed stale flags with DF=1, bit 55=1, CF=0, PF=0 (low byte 1), SF=0
        vm.State().packed_flags = (u64{1} << 55) | flags::Direction | 1u;
        CHECK(vm.RunPrepared() == 128 + 11);
        CHECK(vm.StopReason() == swift::runtime::GuestStopReason::GuestSignalTermination);
        const auto fault = vm.CaptureException();
        REQUIRE(fault != nullptr);
        CHECK(fault->info.address == bad_page);
        CHECK(fault->info.instruction_pc == entry + 4);
        const u64 got = vm.State().packed_flags;
        CHECK(flags::GetCF(got));
        CHECK(flags::GetPF(got));
        CHECK(flags::GetAF(got));
        CHECK(flags::GetSF(got));
        CHECK(!flags::GetZF(got));
        CHECK(!flags::GetOF(got));
        CHECK(flags::GetDF(got));
        CHECK((got & (u64{1} << 55)) != 0);
        CHECK(vm.Close().Complete());
    }

    // 2. AArch64 guest: subs x0, x0, #1 (0 - 1 = -1 -> raw LBT CF=1, carry_xor=true => ARM C=0,
    //    N=1, Z=0, V=0, while untouched 0x25 in [7:0] and bit 55 stay intact!);
    //    ldr x1, [x2] (faults on PROT_NONE page); ret
    {
        const std::array<std::uint32_t, 3> arm_insns{
                0xf1000400u,  // subs x0, x0, #1
                0xf9400041u,  // ldr x1, [x2]
                0xd65f03c0u   // ret
        };
        const auto elf = MakeTinyElf64(
                EM_AARCH64,
                {reinterpret_cast<const std::uint8_t*>(arm_insns.data()), sizeof(arm_insns)});
        ex::RuntimeOptions opts;
        ex::RuntimeStats stats;
        swift::runtime::platform::Loongarch64Host host{false};
        ex::VM vm{opts, stats, host.Bind()};
        vm.PrepareGuest(elf, "lbt-arm64-fault-test");
        const auto bad_page = vm.Memory().Map(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
        REQUIRE(bad_page != UINT64_MAX);
        const auto entry = vm.ArmState().pc;
        vm.ArmState().gprs[0] = 0;
        vm.ArmState().gprs[2] = bad_page;
        // Seed stale Carry=1 (opposite of expected ARM C=0!) and sentinel 0x25 in [7:0]
        vm.ArmState().packed_flags = (u64{1} << 55) | flags::Carry | 0x25u;
        CHECK(vm.RunPrepared() == 128 + 11);
        CHECK(vm.StopReason() == swift::runtime::GuestStopReason::GuestSignalTermination);
        const auto fault = vm.CaptureException();
        REQUIRE(fault != nullptr);
        CHECK(fault->info.address == bad_page);
        CHECK(fault->info.instruction_pc == entry + 4);
        // Expected: N=1 (bit 31), Z=0 (bit 30), C=0 (bit 29, inverted from LBT borrow=1!), V=0 (bit 28),
        // plus preserved bit 55 and 0x25!
        CHECK(vm.ArmState().packed_flags == ((u64{1} << 55) | flags::Sign | 0x25u));
        CHECK(vm.Close().Complete());
    }
}
#endif

TEST_CASE("UAL memory, LA664 TSO dbar hints, direct branches, and scalar idioms",
          "[compile][loongarch64-compile][native]") {
    CHECK(la::KnownTargetInfo().Has(TargetFeature::ShiftedAddress));

    // 1. UAL direct unaligned memory + disp12 folding + ALSL.D + TSO directional dbar hints
    {
        alignas(16) std::array<std::uint8_t, 512> buf_sw{}, buf_ual{};
        for (std::size_t i = 0; i < buf_sw.size(); ++i) {
            buf_sw[i] = static_cast<std::uint8_t>((i * 37 + 13) & 0xff);
            buf_ual[i] = buf_sw[i];
        }

        Builder b{Scope::Block, la::KnownTargetInfo()};
        Begin(b);
        Emit(b, OpCode::FaultLocation, Type::Void, {Imm(0x401000)});
        const auto base = Load(b, 0);
        const auto idx = Load(b, 1);
        const auto v64 = Load(b, 2);
        const auto v128 = LoadVector(b, 0);

        const auto scaled4 = Emit(b,
                                  OpCode::GetOperand,
                                  Type::U64,
                                  {Operand::Expression(OperandOp::AddShift, 2, 2),
                                   Operand::ValueOf(base),
                                   Operand::ValueOf(idx)});
        const auto scaled8 = Emit(b,
                                  OpCode::GetOperand,
                                  Type::U64,
                                  {Operand::Expression(OperandOp::AddShift, 2, 3),
                                   Operand::ValueOf(base),
                                   Operand::ValueOf(idx)});

        // StoreMemoryTSO [scaled4 + 3] = v64 (unaligned 64-bit, folded disp12=+3, dbar 0x12)
        Emit(b,
             OpCode::StoreMemoryTSO,
             Type::Void,
             {Operand::Expression(OperandOp::Add, 2),
              Operand::ValueOf(scaled4),
              Imm(3),
              Operand::ValueOf(v64)});

        // StoreMemory [scaled8 - 5] = v128 (unaligned 128-bit, folded disp12=-5)
        Emit(b,
             OpCode::StoreMemory,
             Type::Void,
             {Operand::Expression(OperandOp::Sub, 2),
              Operand::ValueOf(scaled8),
              Imm(5),
              Operand::ValueOf(v128)});

        // LoadMemory U32 directly from [base + idx*2] (AddShift in memory address)
        const auto ld_shifted = Emit(b,
                                     OpCode::LoadMemory,
                                     Type::U32,
                                     {Operand::Expression(OperandOp::AddShift, 2, 1),
                                      Operand::ValueOf(base),
                                      Operand::ValueOf(idx)});
        Store(b, 5, Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(ld_shifted)}));

        // Large displacement (> 2047) StoreMemory & LoadMemory: [base - 0x3000 + 0x3010] == [base + 0x10]
        const auto neg_base = Emit(b,
                                   OpCode::Sub,
                                   Type::U64,
                                   {Operand::ValueOf(base), Imm(0x3000)});
        Emit(b,
             OpCode::StoreMemory,
             Type::Void,
             {Operand::Expression(OperandOp::Add, 2),
              Operand::ValueOf(neg_base),
              Imm(0x3010),
              Operand::ValueOf(v64)});
        const auto ld_large = Emit(b,
                                   OpCode::LoadMemory,
                                   Type::U64,
                                   {Operand::Expression(OperandOp::Add, 2),
                                    Operand::ValueOf(neg_base),
                                    Imm(0x3010)});
        Store(b, 6, ld_large);

        // LoadMemoryTSO U64 from [base + 7] (unaligned, folded disp12=+7, dbar 0x14)
        const auto ld64 = Emit(b,
                               OpCode::LoadMemoryTSO,
                               Type::U64,
                               {Operand::Expression(OperandOp::Add, 2),
                                Operand::ValueOf(base),
                                Imm(7)});
        Store(b, 3, ld64);

        // LoadMemoryTSO U16 from [base + 11] and U32 from [base + 15]
        const auto ld16 = Emit(b,
                               OpCode::LoadMemoryTSO,
                               Type::U16,
                               {Operand::Expression(OperandOp::Add, 2),
                                Operand::ValueOf(base),
                                Imm(11)});
        const auto ld32 = Emit(b,
                               OpCode::LoadMemoryTSO,
                               Type::U32,
                               {Operand::Expression(OperandOp::Add, 2),
                                Operand::ValueOf(base),
                                Imm(15)});
        Store(b,
              4,
              Emit(b,
                   OpCode::Or,
                   Type::U64,
                   {Operand::ValueOf(Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(ld16)})),
                    Operand::ValueOf(Emit(
                            b,
                            OpCode::LslImm,
                            Type::U64,
                            {Operand::ValueOf(Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(ld32)})),
                             Imm(32)}))}));

        // LoadMemory V128 from [scaled8 - 5] (unaligned 128-bit load back)
        const auto ld128 = Emit(b,
                                OpCode::LoadMemory,
                                Type::V128,
                                {Operand::Expression(OperandOp::Sub, 2),
                                 Operand::ValueOf(scaled8),
                                 Imm(5)});
        StoreVector(b, 1, ld128);

        // Explicit directional fences
        Emit(b, OpCode::MemoryLoadFence, Type::Void, {});
        Emit(b, OpCode::MemoryStoreFence, Type::Void, {});
        Emit(b, OpCode::MemoryBarrierTSO, Type::Void, {});

        const auto prog = Return(b, ld64);
        auto sw_opts = Options();
        sw_opts.ual = false;
        auto ual_opts = Options();
        ual_opts.ual = true;

        const auto code_sw = la::Compile(prog, sw_opts);
        const auto code_ual = la::Compile(prog, ual_opts);
        REQUIRE(bool(code_sw));
        REQUIRE(bool(code_ual));
        CHECK(code_ual.words.size() * 2 < code_sw.words.size());

        unsigned dbar_acq = 0, dbar_rel = 0, dbar_rr = 0, dbar_ww = 0, dbar_rw = 0, dbar_zero = 0;
        unsigned alsl_cnt = 0, auipc_t6 = 0;
        for (const u32 w : code_ual.words) {
            if (w == 0x38720014u) ++dbar_acq;
            if (w == 0x38720012u) ++dbar_rel;
            if (w == 0x38720015u) ++dbar_rr;
            if (w == 0x3872001au) ++dbar_ww;
            if (w == 0x38720010u) ++dbar_rw;
            if (w == 0x38720000u) ++dbar_zero;
            if ((w & 0xfffe0000u) == 0x002c0000u) ++alsl_cnt;  // alsl.d
            if (w == 0x1c000012u) ++auipc_t6;                   // pcaddu12i $t6, 0
        }
        CHECK(dbar_acq == 3);
        CHECK(dbar_rel == 1);
        CHECK(dbar_rr == 1);
        CHECK(dbar_ww == 1);
        CHECK(dbar_rw == 1);
        CHECK(dbar_zero == 0);
        CHECK(alsl_cnt == 3);
        CHECK(auipc_t6 == 0);

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
        if (NativeMode() && la::HostFeatures::DetectHardware().ual) {
            NativeBlock nat_sw{code_sw}, nat_ual{code_ual};
            State s_sw{}, s_ual{};
            s_sw.scalars[0] = reinterpret_cast<std::uintptr_t>(buf_sw.data()) + 64;
            s_ual.scalars[0] = reinterpret_cast<std::uintptr_t>(buf_ual.data()) + 64;
            s_sw.scalars[1] = 7;
            s_ual.scalars[1] = 7;
            s_sw.scalars[2] = 0x0123456789abcdefULL;
            s_ual.scalars[2] = s_sw.scalars[2];
            s_sw.vectors[0] = {0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
            s_ual.vectors[0] = s_sw.vectors[0];
            CHECK(nat_ual.Run(s_ual) == nat_sw.Run(s_sw));
            CHECK(buf_ual == buf_sw);
            CHECK(s_ual.scalars[3] == s_sw.scalars[3]);
            CHECK(s_ual.scalars[4] == s_sw.scalars[4]);
            CHECK(s_ual.scalars[5] == s_sw.scalars[5]);
            CHECK(s_ual.scalars[6] == 0x0123456789abcdefULL);
            CHECK(s_sw.scalars[6] == 0x0123456789abcdefULL);
            CHECK(s_ual.vectors[1] == s_sw.vectors[1]);
        }
#endif
    }

    // 2. Hardware CRC32C, ROTR/ROTRI (U8/U16/U32/U64), and SignExtend (8/16/32)
    {
        Builder b{Scope::Block, la::KnownTargetInfo()};
        Begin(b);
        const auto crc_in = Load(b, 0);
        const auto data_in = Load(b, 1);
        const auto cnt_in = Load(b, 2);

        const auto c8 = Emit(b, OpCode::Crc32c, Type::U32, {Operand::ValueOf(crc_in, Type::U32), Operand::ValueOf(data_in, Type::U8), Imm(8)});
        const auto c16 = Emit(b, OpCode::Crc32c, Type::U32, {Operand::ValueOf(c8), Operand::ValueOf(data_in, Type::U16), Imm(16)});
        const auto c32 = Emit(b, OpCode::Crc32c, Type::U32, {Operand::ValueOf(c16), Operand::ValueOf(data_in, Type::U32), Imm(32)});
        const auto c64 = Emit(b, OpCode::Crc32c, Type::U32, {Operand::ValueOf(c32), Operand::ValueOf(data_in), Imm(64)});
        Store(b, 3, Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(c64)}));

        // Variable & immediate rotates across U8, U16, U32, U64
        const auto r8 = Emit(b, OpCode::RorValue, Type::U8, {Operand::ValueOf(data_in, Type::U8), Operand::ValueOf(cnt_in, Type::U8)});
        const auto r16 = Emit(b, OpCode::RorValue, Type::U16, {Operand::ValueOf(data_in, Type::U16), Operand::ValueOf(cnt_in, Type::U16)});
        const auto r32 = Emit(b, OpCode::RorValue, Type::U32, {Operand::ValueOf(data_in, Type::U32), Operand::ValueOf(cnt_in, Type::U32)});
        const auto r64 = Emit(b, OpCode::RorValue, Type::U64, {Operand::ValueOf(data_in), Operand::ValueOf(cnt_in)});
        const auto ri8 = Emit(b, OpCode::RorImm, Type::U8, {Operand::ValueOf(r8), Imm(3, Type::U8)});
        const auto ri16 = Emit(b, OpCode::RorImm, Type::U16, {Operand::ValueOf(r16), Imm(5, Type::U16)});
        Store(b, 4, Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(ri8)}));
        Store(b, 5, Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(ri16)}));
        Store(b, 6, Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(r32)}));
        Store(b, 7, r64);

        // SignExtend 8/16/32 -> 64
        Store(b, 8, Emit(b, OpCode::SignExtend, Type::U64, {Operand::ValueOf(data_in, Type::U8)}));
        Store(b, 9, Emit(b, OpCode::SignExtend, Type::U64, {Operand::ValueOf(data_in, Type::U16)}));
        Store(b, 10, Emit(b, OpCode::SignExtend, Type::U64, {Operand::ValueOf(data_in, Type::U32)}));

        const auto prog = Return(b, Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(c64)}));
        auto sw_opts = Options();
        sw_opts.crc32 = false;
        sw_opts.lbt_x86 = false;
        auto hw_opts = Options();
        hw_opts.crc32 = true;
        hw_opts.lbt_x86 = true;

        const auto code_sw = la::Compile(prog, sw_opts);
        const auto code_hw = la::Compile(prog, hw_opts);
        REQUIRE(bool(code_sw));
        REQUIRE(bool(code_hw));
        // Hardware CRCC collapses >450 software CRC instructions into 8 words!
        CHECK(code_hw.words.size() * 4 < code_sw.words.size());

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
        if (NativeMode() && la::HostFeatures::DetectHardware().crc32 &&
            la::HostFeatures::DetectHardware().lbt_x86) {
            NativeBlock nat_sw{code_sw}, nat_hw{code_hw};
            for (const u64 cnt : {u64{0}, u64{1}, u64{7}, u64{13}, u64{31}, u64{65}}) {
                State s_sw{}, s_hw{};
                s_sw.scalars[0] = 0xdeadbeef12345678ULL;
                s_sw.scalars[1] = 0xfedcba9887654381ULL;
                s_sw.scalars[2] = cnt;
                s_hw = s_sw;
                CHECK(nat_hw.Run(s_hw) == nat_sw.Run(s_sw));
                for (unsigned k = 3; k <= 10; ++k) {
                    CHECK(s_hw.scalars[k] == s_sw.scalars[k]);
                }
            }
        }
#endif
    }

    // 3. Multi-block Function direct PC-relative B / BEQZ / BNEZ and fallthrough elision
    {
        Builder b{Scope::Function, la::KnownTargetInfo()};
        const auto entry = Block(b), loop = Block(b), exit = Block(b);
        Need(b.SetEntry(entry));
        Need(b.BeginBlock(entry));
        const auto step = Load(b, 1);
        const auto one = Literal(b, 1);
        Need(b.EndBlock(Terminal::Jump(loop)));  // Fallthrough to loop -> 0 instructions!

        Need(b.BeginBlock(loop));
        const auto cnt = Load(b, 0);
        const auto acc = Load(b, 2);
        const auto next_acc = Emit(b, OpCode::Add, Type::U64, {Operand::ValueOf(acc), Operand::ValueOf(step)});
        Store(b, 2, next_acc);
        const auto next_cnt = Emit(b, OpCode::Sub, Type::U64, {Operand::ValueOf(cnt), Operand::ValueOf(one)});
        Store(b, 0, next_cnt);
        const auto keep_going = Emit(b, OpCode::TestNotZero, Type::Bool, {Operand::ValueOf(next_cnt)});
        // Fallthrough to exit when false -> single BNEZ backward to loop!
        Need(b.EndBlock(Terminal::Branch(keep_going, loop, exit)));

        Need(b.BeginBlock(exit));
        const auto prog = Return(b, Load(b, 2));
        const auto code = la::Compile(prog, Options());
        REQUIRE(bool(code));

        unsigned auipc_t6 = 0, bnez_cnt = 0;
        for (const u32 w : code.words) {
            if (w == 0x1c000012u) ++auipc_t6;                   // pcaddu12i $t6, 0 (legacy indirect jump)
            if ((w & 0xfc000000u) == 0x44000000u) ++bnez_cnt;  // bnez
        }
        CHECK(auipc_t6 == 0);
        CHECK(bnez_cnt >= 1);

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
        if (NativeMode()) {
            NativeBlock nat{code};
            State s{};
            s.scalars[0] = 6;
            s.scalars[1] = 19;
            s.scalars[2] = 0;
            CHECK(nat.Run(s) == 114);
            CHECK(s.scalars[0] == 0);
            CHECK(s.scalars[2] == 114);
        }
#endif
    }
}

TEST_CASE("LSX 128-bit vectorization, PopCount, and dead-input register reuse",
          "[compile][loongarch64-compile][native]") {
    // 1. Exact GNU Binutils 2.44 machine-word verification for custom LSX encodings
    {
        std::vector<u32> words;
        la::Assembler as{words};
        as.VPCNT_D(la::vr1, la::vr2);
        as.VANDI_B(la::vr1, la::vr2, 15);
        as.VSHUF_B(la::vr1, la::vr2, la::vr3, la::vr4);
        as.VMSKLTZ(8, la::vr1, la::vr2);
        as.VMSKLTZ(64, la::vr1, la::vr2);
        as.VSLTI_B(la::vr1, la::vr2, 0);
        as.VSLTI_BU(la::vr1, la::vr2, 16);
        as.VSUB(8, la::vr1, la::vr2, la::vr3);
        as.VSUB(64, la::vr1, la::vr2, la::vr3);
        as.VSLT(16, true, la::vr1, la::vr2, la::vr3);
        as.VSLT(32, false, la::vr1, la::vr2, la::vr3);
        as.VSADD(8, true, la::vr1, la::vr2, la::vr3);
        as.VSSUB(64, false, la::vr1, la::vr2, la::vr3);
        as.VAVGR_U(16, la::vr1, la::vr2, la::vr3);
        as.VMUH_H(la::vr1, la::vr2, la::vr3);
        as.VMUH_HU(la::vr1, la::vr2, la::vr3);
        as.VMULWEV(16, true, la::vr1, la::vr2, la::vr3);
        as.VMULWEV(32, false, la::vr1, la::vr2, la::vr3);
        as.VMADDWOD_W_H(la::vr1, la::vr2, la::vr3);
        as.VABSD_BU(la::vr1, la::vr2, la::vr3);
        as.VHADDW_DU_WU(la::vr1, la::vr2, la::vr3);
        as.VSLLWIL_ZERO(8, true, la::vr1, la::vr2);
        as.VSLLWIL_ZERO(32, false, la::vr1, la::vr2);
        as.VSSRANI_ZERO(16, false, la::vr1, la::vr2);
        as.VSSRANI_ZERO(64, true, la::vr1, la::vr2);
        as.VSLL(16, la::vr1, la::vr2, la::vr3);
        as.VSRL(32, la::vr1, la::vr2, la::vr3);
        as.VSRA(64, la::vr1, la::vr2, la::vr3);

        const std::array<u32, 28> expected{
                0x729c2c41u, 0x73d03c41u, 0x0d520c41u, 0x729c4041u, 0x729c4c41u, 0x72860041u,
                0x72884041u, 0x700c0c41u, 0x700d8c41u, 0x70068c41u, 0x70090c41u, 0x70460c41u,
                0x704d8c41u, 0x706a8c41u, 0x70868c41u, 0x70888c41u, 0x70908c41u, 0x70990c41u,
                0x70ae8c41u, 0x70620c41u, 0x70590c41u, 0x73082041u, 0x730c8041u, 0x73604041u,
                0x73650041u, 0x70e88c41u, 0x70eb0c41u, 0x70ed8c41u};
        REQUIRE(words.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) CHECK(words[i] == expected[i]);
    }

    // 2. LSX PopCount (vpcnt.d) + Dead-input register reuse (0 spill slots under full bank)
    {
        Builder b{Scope::Block, la::KnownTargetInfo()};
        Begin(b);
        std::array<Value, 7> live{};
        for (unsigned i = 0; i < live.size(); ++i) live[i] = Load(b, i);
        auto acc = Load(b, 7);
        // Sequential chain with 7 live values pinning s0..s6 and acc reusing s7 in place!
        for (unsigned step = 0; step < 10; ++step) {
            acc = Emit(b, OpCode::Add, Type::U64, {Operand::ValueOf(acc), Operand::ValueOf(live[step % 7])});
            // Exercise narrow BitInsert & compound GetOperand when destination aliases dying second input!
            const auto ins16 = Emit(
                    b,
                    OpCode::BitInsert,
                    Type::U16,
                    {Operand::ValueOf(live[(step + 1) % 7], Type::U16),
                     Operand::ValueOf(acc, Type::U16),
                     Imm(4, Type::U16),
                     Imm(8, Type::U16)});
            acc = Emit(b,
                       OpCode::GetOperand,
                       Type::U64,
                       {Operand::Expression(OperandOp::AddShift, 2, 1),
                        Operand::ValueOf(live[(step + 2) % 7]),
                        Operand::ValueOf(Emit(b, OpCode::ZeroExtend64, Type::U64, {Operand::ValueOf(ins16)}))});
        }
        for (const auto v : live)
            acc = Emit(b, OpCode::Xor, Type::U64, {Operand::ValueOf(acc), Operand::ValueOf(v)});
        const auto pop_raw = Emit(b, OpCode::PopCount, Type::U64, {Operand::ValueOf(acc, Type::U16)});
        Store(b, 8, pop_raw);
        const auto pop_u32 = Emit(
                b,
                OpCode::PopCount,
                Type::U64,
                {Operand::ValueOf(Emit(b, OpCode::ZeroExtend32, Type::U32, {Operand::ValueOf(acc)}))});
        Store(b, 9, pop_u32);
        const auto prog = Return(b, acc);

        auto sw_opts = Options();
        sw_opts.lsx = false;
        auto lsx_opts = Options();
        lsx_opts.lsx = true;

        const auto code_sw = la::Compile(prog, sw_opts);
        const auto code_lsx = la::Compile(prog, lsx_opts);
        REQUIRE(bool(code_sw));
        REQUIRE(bool(code_lsx));
        CHECK(code_lsx.spill_slots == 0);
        CHECK(code_sw.spill_slots == 0);
        CHECK(code_lsx.words.size() < code_sw.words.size());

        unsigned vpcnt_cnt = 0;
        for (const u32 w : code_lsx.words)
            if ((w & 0xfffffc00u) == 0x729c2c00u) ++vpcnt_cnt;
        CHECK(vpcnt_cnt == 2);

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
        if (NativeMode() && la::HostFeatures::DetectHardware().lsx) {
            NativeBlock nat_sw{code_sw}, nat_lsx{code_lsx};
            State s_sw{}, s_lsx{};
            for (unsigned i = 0; i < 8; ++i) {
                s_sw.scalars[i] = 0x9e3779b97f4a7c15ULL * (i + 3) ^ (u64{i} << 41);
                s_lsx.scalars[i] = s_sw.scalars[i];
            }
            CHECK(nat_lsx.Run(s_lsx) == nat_sw.Run(s_sw));
            CHECK(s_lsx.scalars[8] == s_sw.scalars[8]);
            CHECK(s_lsx.scalars[9] == s_sw.scalars[9]);
        }
#endif
    }

    // 3. Dead-input V128 pair reuse (0 spills with 3 pinned V128s + sequential chain)
    //    plus spilled V128 VLD/VST stack fast path when 6 V128s are live simultaneously
    {
        Builder b{Scope::Block, la::KnownTargetInfo()};
        Begin(b);
        std::array<Value, 3> vlive{LoadVector(b, 0), LoadVector(b, 1), LoadVector(b, 2)};
        auto vacc = LoadVector(b, 3);
        for (unsigned step = 0; step < 8; ++step) {
            vacc = Emit(b, OpCode::VecAdd, Type::V128, {Operand::ValueOf(vacc), Operand::ValueOf(vlive[step % 3]), Imm(32)});
            vacc = Emit(b, OpCode::VecAndNot, Type::V128, {Operand::ValueOf(vacc), Operand::ValueOf(vlive[(step + 1) % 3])});
        }
        for (const auto v : vlive)
            vacc = Emit(b, OpCode::VecXor, Type::V128, {Operand::ValueOf(vacc), Operand::ValueOf(v)});
        StoreVector(b, 4, vacc);
        const auto prog = Return(b);

        auto lsx_opts = Options();
        lsx_opts.lsx = true;
        const auto code_lsx = la::Compile(prog, lsx_opts);
        REQUIRE(bool(code_lsx));
        CHECK(code_lsx.spill_slots == 0);

        Builder b_spill{Scope::Block, la::KnownTargetInfo()};
        Begin(b_spill);
        std::array<Value, 6> v6{};
        for (unsigned i = 0; i < 6; ++i) v6[i] = LoadVector(b_spill, i);
        auto sp_acc = Emit(b_spill, OpCode::VecAdd, Type::V128, {Operand::ValueOf(v6[4]), Operand::ValueOf(v6[5]), Imm(16)});
        for (unsigned i = 0; i < 6; ++i)
            sp_acc = Emit(b_spill, OpCode::VecSub, Type::V128, {Operand::ValueOf(sp_acc), Operand::ValueOf(v6[i]), Imm(16)});
        StoreVector(b_spill, 6, sp_acc);
        const auto prog_spill = Return(b_spill);
        auto sw_opts = Options();
        sw_opts.lsx = false;
        const auto spill_sw = la::Compile(prog_spill, sw_opts);
        const auto spill_lsx = la::Compile(prog_spill, lsx_opts);
        REQUIRE(bool(spill_sw));
        REQUIRE(bool(spill_lsx));
        CHECK(spill_lsx.spill_slots > 0);
        unsigned vld_cnt = 0;
        for (const u32 w : spill_lsx.words)
            if ((w & 0xffc00000u) == 0x2c000000u) ++vld_cnt;
        CHECK(vld_cnt > 0);

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
        if (NativeMode() && la::HostFeatures::DetectHardware().lsx) {
            NativeBlock nat_sw{spill_sw}, nat_lsx{spill_lsx};
            State s_sw{}, s_lsx{};
            for (unsigned i = 0; i < 6; ++i) {
                s_sw.vectors[i] = {0x123456789abcdef0ULL * (i + 1), 0xfedcba9876543210ULL ^ (u64{i} << 29)};
                s_lsx.vectors[i] = s_sw.vectors[i];
            }
            nat_sw.Run(s_sw);
            nat_lsx.Run(s_lsx);
            CHECK(s_lsx.vectors[6] == s_sw.vectors[6]);
        }
#endif
    }

    // 4. Differential LSX vs Scalar Fallback Batch A:
    //    Bitwise (VecAndNot), Arithmetic, SatAdd/SatSub, MulHigh16, MulWiden,
    //    SignedMulWidenLow, Pack (16->8 & 64->32), Extend (8->32), AbsDiffSum8, Madd16
    {
        Builder b{Scope::Block, la::KnownTargetInfo()};
        Begin(b);
        const auto v0 = LoadVector(b, 0);
        const auto v1 = LoadVector(b, 1);

        StoreVector(b, 0, Emit(b, OpCode::VecAndNot, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1)}));
        StoreVector(b, 1, Emit(b, OpCode::VecSub, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(8)}));
        StoreVector(b, 2, Emit(b, OpCode::VecMul, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(32)}));
        StoreVector(b, 3, Emit(b, OpCode::VecCmpEq, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(16)}));
        StoreVector(b, 4, Emit(b, OpCode::VecCmpGt, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(8)}));
        StoreVector(b, 5, Emit(b, OpCode::VecAvg, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(8)}));
        StoreVector(b, 6, Emit(b, OpCode::VecMin, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(16), Imm(1)}));
        StoreVector(b, 7, Emit(b, OpCode::VecMax, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(32), Imm(0)}));
        StoreVector(b, 8, Emit(b, OpCode::VecSatAdd, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(8), Imm(1)}));
        StoreVector(b, 9, Emit(b, OpCode::VecSatSub, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(64), Imm(1)}));
        StoreVector(b, 10, Emit(b, OpCode::VecMulHigh16, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(1)}));
        StoreVector(b, 11, Emit(b, OpCode::VecMulWiden, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(16), Imm(1)}));
        StoreVector(b, 12, Emit(b, OpCode::VecSignedMulWidenLow, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(16)}));
        StoreVector(b, 13, Emit(b, OpCode::VecPack, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(64), Imm(1)}));
        StoreVector(b, 14, Emit(b, OpCode::VecExtend, Type::V128, {Operand::ValueOf(v0), Imm(8), Imm(32), Imm(1)}));
        const auto sad = Emit(b, OpCode::VecAbsDiffSum8, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1)});
        const auto madd = Emit(b, OpCode::VecMadd16, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1)});
        StoreVector(b, 15, Emit(b, OpCode::VecXor, Type::V128, {Operand::ValueOf(sad), Operand::ValueOf(madd)}));

        const auto prog = Return(b);
        auto sw_opts = Options();
        sw_opts.lsx = false;
        auto lsx_opts = Options();
        lsx_opts.lsx = true;

        const auto code_sw = la::Compile(prog, sw_opts);
        const auto code_lsx = la::Compile(prog, lsx_opts);
        REQUIRE(bool(code_sw));
        REQUIRE(bool(code_lsx));
        CHECK(code_lsx.words.size() * 4 < code_sw.words.size());

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
        if (NativeMode() && la::HostFeatures::DetectHardware().lsx) {
            NativeBlock nat_sw{code_sw}, nat_lsx{code_lsx};
            const std::array<std::pair<Pair, Pair>, 3> inputs{{
                    {{0x800080007fff8001ULL, 0xfedcba9876543210ULL},
                     {0x800080000001fffeULL, 0x0123456789abcdefULL}},
                    {{0x7fffffffffffffffULL, 0x8000000000000000ULL},
                     {0xffffffffffffffffULL, 0x0000000100000000ULL}},
                    {{0x00ff807f01fe817eULL, 0x807fff0055aa33ccULL},
                     {0x00ff7f80fe017e81ULL, 0x7f80ff00aa55cc33ULL}},
            }};
            for (const auto& [in0, in1] : inputs) {
                State s_sw{}, s_lsx{};
                s_sw.vectors[0] = in0;
                s_sw.vectors[1] = in1;
                s_lsx = s_sw;
                nat_sw.Run(s_sw);
                nat_lsx.Run(s_lsx);
                for (unsigned i = 0; i < 16; ++i) CHECK(s_lsx.vectors[i] == s_sw.vectors[i]);
            }
        }
#endif
    }

    // 5. Differential LSX vs Scalar Fallback Batch B:
    //    VectorShift (Imm / Scalar < bits & >= bits / Per-lane Vector < bits & >= bits),
    //    VecByteShift, VecExtractBytes, Shuffles (32/TwoSrc/16/DupPairs/Zip/Unzip),
    //    VecTableLookup8 vs VecShuffle32Indexed, VecMovMask, and branchless V128 Select
    {
        Builder b{Scope::Block, la::KnownTargetInfo()};
        Begin(b);
        const auto v0 = LoadVector(b, 0);
        const auto v1 = LoadVector(b, 1);
        const auto v_ctrl = LoadVector(b, 2);
        const auto sc_cnt = Load(b, 0);
        const auto sel_cond = Load(b, 1);

        StoreVector(b, 0, Emit(b, OpCode::VecShiftLeftImm, Type::V128, {Operand::ValueOf(v0), Imm(3), Imm(16)}));
        StoreVector(b, 1, Emit(b, OpCode::VecShiftRightImm, Type::V128, {Operand::ValueOf(v0), Imm(16), Imm(16)}));
        StoreVector(b, 2, Emit(b, OpCode::VecShiftRightArithmeticImm, Type::V128, {Operand::ValueOf(v0), Imm(25), Imm(16)}));
        StoreVector(b, 3, Emit(b, OpCode::VecShiftLeft, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(sc_cnt), Imm(16)}));
        StoreVector(b, 4, Emit(b, OpCode::VecShiftRightArithmetic, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(sc_cnt), Imm(16)}));
        StoreVector(b, 5, Emit(b, OpCode::VecShiftLeft, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v_ctrl), Imm(8)}));
        StoreVector(b, 6, Emit(b, OpCode::VecShiftRightArithmetic, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v_ctrl), Imm(32)}));
        StoreVector(b, 7, Emit(b, OpCode::VecByteShift, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(5), Imm(1)}));
        StoreVector(b, 8, Emit(b, OpCode::VecExtractBytes, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(11)}));
        StoreVector(b, 9, Emit(b, OpCode::VecShuffle32TwoSrc, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(0xb1)}));
        const auto sh16_lo = Emit(b, OpCode::VecShuffle16, Type::V128, {Operand::ValueOf(v0), Imm(0x1b), Imm(0)});
        const auto sh16_hi = Emit(b, OpCode::VecShuffle16, Type::V128, {Operand::ValueOf(sh16_lo), Imm(0xb1), Imm(1)});
        StoreVector(b, 10, sh16_hi);
        StoreVector(b, 11, Emit(b, OpCode::VecZip, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(16), Imm(0)}));
        StoreVector(b, 12, Emit(b, OpCode::VecUnzip, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v1), Imm(8), Imm(1)}));
        StoreVector(b, 13, Emit(b, OpCode::VecTableLookup8, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v_ctrl)}));
        StoreVector(b, 14, Emit(b, OpCode::VecShuffle32Indexed, Type::V128, {Operand::ValueOf(v0), Operand::ValueOf(v_ctrl)}));
        StoreVector(b, 15, Emit(b, OpCode::Select, Type::V128, {Operand::ValueOf(sel_cond, Type::U8), Operand::ValueOf(v0), Operand::ValueOf(v1)}));

        Store(b, 2, Emit(b, OpCode::VecMovMask, Type::U64, {Operand::ValueOf(v0), Imm(8)}));
        Store(b, 3, Emit(b, OpCode::VecMovMask, Type::U64, {Operand::ValueOf(v0), Imm(16)}));
        Store(b, 4, Emit(b, OpCode::VecMovMask, Type::U64, {Operand::ValueOf(v0), Imm(32)}));
        Store(b, 5, Emit(b, OpCode::VecMovMask, Type::U64, {Operand::ValueOf(v0), Imm(64)}));

        const auto prog = Return(b);
        auto sw_opts = Options();
        sw_opts.lsx = false;
        auto lsx_opts = Options();
        lsx_opts.lsx = true;

        const auto code_sw = la::Compile(prog, sw_opts);
        const auto code_lsx = la::Compile(prog, lsx_opts);
        REQUIRE(bool(code_sw));
        REQUIRE(bool(code_lsx));
        CHECK(code_lsx.words.size() * 4 < code_sw.words.size());

#if defined(__loongarch__) && __loongarch_grlen == 64 && defined(__linux__)
        if (NativeMode() && la::HostFeatures::DetectHardware().lsx) {
            NativeBlock nat_sw{code_sw}, nat_lsx{code_lsx};
            for (const u64 cnt : {u64{0}, u64{5}, u64{15}, u64{16}, u64{99}}) {
                for (const u64 cond : {u64{0x100}, u64{0x101}}) {  // Exercise narrow U8 view (0x100 -> false, 0x101 -> true)
                    State s_sw{}, s_lsx{};
                    s_sw.vectors[0] = {0x807f00ff817e55aaULL, 0xfedcba9876543210ULL};
                    s_sw.vectors[1] = {0x0123456789abcdefULL, 0xaa55cc330f0f00ffULL};
                    // Control mixes <8 (0x02, 0x07), 8..15 (0x09, 0x0f), positive >=16 (0x13, 0x25), and negative (0x83, 0xff)
                    s_sw.vectors[2] = {0x8325130f09070200ULL, 0x0000002500000005ULL};
                    s_sw.scalars[0] = cnt;
                    s_sw.scalars[1] = cond;
                    s_lsx = s_sw;
                    nat_sw.Run(s_sw);
                    nat_lsx.Run(s_lsx);
                    for (unsigned i = 0; i < 16; ++i) CHECK(s_lsx.vectors[i] == s_sw.vectors[i]);
                    for (unsigned k = 2; k <= 5; ++k) CHECK(s_lsx.scalars[k] == s_sw.scalars[k]);
                }
            }
        }
#endif
    }
}
