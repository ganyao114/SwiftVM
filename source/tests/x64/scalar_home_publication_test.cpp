#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/function_plan.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
constexpr u32 extent = 1024, lanes = 8;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13); state->host_cpu_flags = initial_flags; }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
};
backend::FunctionPlan Plan() { backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); return plan; }
ir::Value Load(ir::Assembler& as, u32 offset, T type = T::U64) { return as.LoadUniform(ir::Uniform{offset, type}).SetType(type); }
ir::Value View(ir::Value value, T type) { return ir::Value{value.Def()}.SetCastType(type); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
void Carry(ir::Assembler& as) { as.SetCarry(as.LoadImm(ir::Imm{u64{1}}).SetType(T::U8)); }
u64 Mask(T type) { const u32 bits = ir::GetValueSizeByte(type) * 8; return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
u64 Helper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return a * 3 + b * 5 + c * 7;
}
unsigned __int128 Pair(u64 a, u64 b, u64 tag) {
    const u64 low = a + b + tag, high = a ^ (b * 19) ^ 0x8123456789abcdefULL;
    return (static_cast<unsigned __int128>(high) << 64) | low;
}
// Tail-call the actual JIT after filling the stack below the incoming RSP.
// This covers its six saved registers and complete frame, without touching
// the caller's return address. Uniform poison alone cannot catch stale homes.
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize() + 4096 + 15) & ~u32{15};
        mov(rdx, rdi); mov(r11, rsp); sub(rsp, bytes); mov(rdi, rsp);
        mov(rcx, bytes / 8); mov(rax, 0xa5e39b7142c68df0ULL); cld(); rep(); stosq();
        mov(rsp, r11); mov(rdi, rdx); mov(rax, reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine) { REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost); }
};
void Same(const Machine& baseline, const Machine& actual, const Machine& expected, u64 flags) {
    REQUIRE(std::memcmp(baseline.bytes.data(), actual.bytes.data(), actual.bytes.size()) == 0);
    REQUIRE(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, extent) == 0);
    REQUIRE(actual.state->host_cpu_flags == flags);
}
u64 AddFlags(u64 input, T type) {
    const u32 bits = ir::GetValueSizeByte(type) * 8; const u64 mask = Mask(type), sign = u64{1} << (bits - 1);
    const u64 lhs = input & mask, result = (lhs + 1) & mask;
    const u64 packed = (u64(bool(result & sign)) << 31) | (u64(result == 0) << 30) |
                       (u64(lhs == mask) << 29) | (u64(lhs == sign - 1) << 28);
    return (initial_flags & ~(u64{15} << 28)) | packed;
}
} // namespace

TEST_CASE("scalar spills publish complete masked low words across helpers and delayed flags in poisoned frames", "[x64][home][scalar][poison][flags][helper]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; std::array<ir::Value, lanes> values;
        for (u32 i = 0; i < lanes; ++i) values[i] = Load(as, 16 + i * 8, type);
        auto sum = as.Add(values[0], ir::Operand{ir::Imm{u64{1}}}).SetType(type);
        auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, values[0], values[1], values[2]).SetType(type);
        for (u32 i = 0; i < lanes; ++i) Store(as, 128 + i * 8, View(values[i], T::U64));
        Store(as, 208, View(sum, T::U64)); Store(as, 216, View(returned, T::U64)); as.SaveFlags(sum, ir::Flags::NZCV);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator compact{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; compact.Translate();
        REQUIRE(compact.CfgPlan().Enabled()); REQUIRE(compact.FlagsResident() == flags);
        REQUIRE(legacy.Statistics().scalar_high_word_store_omissions == 0);
        REQUIRE(compact.Statistics().scalar_high_word_store_omissions > 0);
        u32 spilled{};
        for (auto value : values) if (!compact.CfgPlan().Register(value.Def())) {
            REQUIRE(compact.FramePlan().Home(value.Def())); REQUIRE_FALSE(compact.CfgPlan().RequiresFullContainer(value.Def())); ++spilled;
        }
        REQUIRE(spilled > 0); REQUIRE(compact.FramePlan().Home(sum.Def()));
        PoisonCall old_call{legacy}, new_call{compact};
        const u64 sign = u64{1} << (ir::GetValueSizeByte(type) * 8 - 1), mask = Mask(type);
        for (u64 seed : {u64{0}, mask, sign - 1, u64{0x8123456789abcdef}}) {
            CAPTURE(type, flags, seed); Machine baseline, actual, expected; std::array<u64, lanes> model;
            for (u32 i = 0; i < lanes; ++i) {
                const u64 input = seed + i * 29; model[i] = input & mask;
                for (auto* machine : {&baseline, &actual, &expected}) machine->Put(16 + i * 8, input);
                expected.Put(128 + i * 8, model[i]);
            }
            expected.Put(208, (model[0] + 1) & mask); expected.Put(216, (model[0] * 3 + model[1] * 5 + model[2] * 7) & mask);
            old_call.Run(baseline); new_call.Run(actual); Same(baseline, actual, expected, AddFlags(seed, type));
        }
    }
}

TEST_CASE("raw views and narrow BitCast inputs keep scalar high words initialized in poisoned frames", "[x64][home][raw][bitcast][poison]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto loaded = Load(as, 16, type);
        auto sum = as.Add(loaded, ir::Operand{ir::Imm{u64{1}}}).SetType(type);
        // BitCast's input view is narrow, but its emitter reads all 16 bytes.
        auto copied = as.BitCast(View(sum, type)).SetType(T::V128);
        auto full = Load(as, 32, T::V128); auto narrow_alias = as.BitCast(full).SetType(type);
        auto full_again = as.BitCast(View(narrow_alias, type)).SetType(T::V128);
        Carry(as); Store(as, 128, View(loaded, T::V128)); Store(as, 144, View(sum, T::V128));
        Store(as, 160, copied); Store(as, 176, View(narrow_alias, T::V128)); Store(as, 192, full_again);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator compact{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; compact.Translate();
        for (auto value : {loaded, sum, narrow_alias}) REQUIRE(compact.CfgPlan().RequiresFullContainer(value.Def()));
        PoisonCall old_call{legacy}, new_call{compact};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            CAPTURE(type, flags, seed); Machine baseline, actual, expected;
            constexpr u64 high = 0xdebca9876543210fULL;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(16, seed); machine->Put(32, seed ^ 0xa578c319ed4260bfULL); machine->Put(40, high); }
            expected.Put(128, seed & Mask(type)); expected.Put(136, 0);
            for (u32 offset : {u32{144}, u32{160}}) { expected.Put(offset, ((seed & Mask(type)) + 1) & Mask(type)); expected.Put(offset + 8, 0); }
            for (u32 offset : {u32{176}, u32{192}}) { expected.Put(offset, seed ^ 0xa578c319ed4260bfULL); expected.Put(offset + 8, high); }
            old_call.Run(baseline); new_call.Run(actual); Same(baseline, actual, expected, initial_flags | (u64{1} << 29));
        }
    }
}

TEST_CASE("vector Phi receives zero high words from pressured scalar producers on both edges", "[x64][home][phi][raw][poison]") {
    for (auto type : {T::U8, T::U32, T::U64}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto condition = Load(as, 0, T::U8);
        std::array<ir::Value, lanes> first, second, phis;
        for (u32 i = 0; i < lanes; ++i) first[i] = Load(as, 32 + i * 16, type);
        auto skip = as.Goto(ir::BOOL{condition});
        for (u32 i = 0; i < lanes; ++i) second[i] = as.Add(first[i], ir::Operand{ir::Imm{u64{17 + i}}}).SetType(type);
        as.BindLabel(skip);
        for (u32 i = 0; i < lanes; ++i) { ir::Params inputs; inputs.Push(View(first[i], T::V128)); inputs.Push(View(second[i], T::V128)); phis[i] = as.AddPhi(inputs).SetType(T::V128); }
        Carry(as); for (u32 i = 0; i < lanes; ++i) Store(as, 512 + i * 16, phis[i]);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator compact{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; compact.Translate();
        for (u32 i = 0; i < lanes; ++i) { REQUIRE(compact.CfgPlan().RequiresFullContainer(first[i].Def())); REQUIRE(compact.CfgPlan().RequiresFullContainer(second[i].Def())); }
        PoisonCall old_call{legacy}, new_call{compact};
        for (u64 selector : {u64{0}, u64{1}}) {
            CAPTURE(type, flags, selector); Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) {
                machine->Put(0, selector); for (u32 i = 0; i < lanes; ++i) { machine->Put(32 + i * 16, UINT64_MAX - i * 137); machine->Put(40 + i * 16, 0xa5a5a5a5a5a5a5a5ULL + i); }
            }
            for (u32 i = 0; i < lanes; ++i) { expected.Put(512 + i * 16, (((UINT64_MAX - i * 137) & Mask(type)) + (selector ? 0 : 17 + i)) & Mask(type)); expected.Put(520 + i * 16, 0); }
            old_call.Run(baseline); new_call.Run(actual); Same(baseline, actual, expected, initial_flags | (u64{1} << 29));
        }
    }
}

TEST_CASE("pair producers retain real upper results and early pseudo containers across helpers", "[x64][home][pair][pseudo][helper][poison]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto high = Load(as, 0), low = Load(as, 16), divisor = Load(as, 24);
        auto quotient = as.Div128(high, low, divisor, ir::Imm{u64{0}}).SetType(T::U64);
        auto pair = as.Cpuid(quotient, divisor, ir::Imm{u64{7}}, ir::Imm{reinterpret_cast<u64>(&Pair)}).SetType(T::U64);
        (void)as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, low, divisor, high).SetType(T::U64);
        auto remainder = as.Div128Remainder(quotient).SetType(T::U64), upper = as.CpuidUpper(pair).SetType(T::U64);
        Carry(as); Store(as, 128, View(quotient, T::V128)); Store(as, 144, View(remainder, T::V128));
        Store(as, 160, View(pair, T::V128)); Store(as, 176, View(upper, T::V128));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator compact{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; compact.Translate();
        for (auto value : {quotient, remainder, pair, upper}) { REQUIRE(compact.CfgPlan().RequiresFullContainer(value.Def())); REQUIRE(compact.FramePlan().Home(value.Def())); }
        REQUIRE(compact.FramePlan().Home(quotient.Def()) != compact.FramePlan().Home(remainder.Def()));
        REQUIRE(compact.FramePlan().Home(pair.Def()) != compact.FramePlan().Home(upper.Def()));
        PoisonCall old_call{legacy}, new_call{compact};
        for (u64 high_input : {u64{0}, u64{5}, UINT64_MAX}) for (u64 divisor_input : {u64{0}, u64{3}, UINT64_MAX}) {
            CAPTURE(flags, high_input, divisor_input); Machine baseline, actual, expected; constexpr u64 low_input = 0xef123456789abcdeULL;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, high_input); machine->Put(16, low_input); machine->Put(24, divisor_input); }
            const unsigned __int128 dividend = (static_cast<unsigned __int128>(high_input) << 64) | low_input;
            const u64 q = divisor_input ? u64(dividend / divisor_input) : 0, r = divisor_input ? u64(dividend % divisor_input) : 0;
            const u64 pair_low = q + divisor_input + 7, pair_high = q ^ (divisor_input * 19) ^ 0x8123456789abcdefULL;
            expected.Put(128, q); expected.Put(136, r); expected.Put(144, r); expected.Put(152, 0);
            expected.Put(160, pair_low); expected.Put(168, pair_high); expected.Put(176, pair_high); expected.Put(184, 0);
            old_call.Run(baseline); new_call.Run(actual); Same(baseline, actual, expected, initial_flags | (u64{1} << 29));
        }
    }
}
