#include <array>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
constexpr u32 extent = 1024, lanes = 8;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
constexpr u64 carried_flags = initial_flags | (u64{1} << 29);
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() {
        for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13);
        state->host_cpu_flags = initial_flags;
    }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    void Finished(u64 flags = carried_flags) {
        state->current_loc = ir::Location{0x1000}; state->prev_loc = ir::Location{0x1000};
        state->halt_reason = HaltReason::CallHost; state->host_cpu_flags = flags;
    }
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
// Fill the actual incoming JIT stack, including all recycled homes, before
// tail-calling it. This detects accidental reliance on a previous owner.
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize() + 4096 + 15) & ~u32{15};
        mov(rdx, rdi); mov(r11, rsp); sub(rsp, bytes); mov(rdi, rsp);
        mov(rcx, bytes / 8); mov(rax, 0xa5e39b7142c68df0ULL); cld(); rep(); stosq();
        mov(rsp, r11); mov(rdi, rdx); mov(rax, reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine) {
        const auto mxcsr = _mm_getcsr();
        REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost);
        REQUIRE(_mm_getcsr() == mxcsr);
    }
};
void Complete(const Machine& baseline, const Machine& actual, Machine& expected, u64 flags = carried_flags) {
    expected.Finished(flags);
    REQUIRE(std::memcmp(baseline.bytes.data(), actual.bytes.data(), actual.bytes.size()) == 0);
    REQUIRE(std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()) == 0);
}
void Layout(const x64::SparseFramePlan& frame, const x64::CfgRegisterPlan& registers,
            backend::FunctionPlan& plan, const std::unordered_set<ir::Inst*>& flags = {}) {
    REQUIRE(frame.Enabled()); REQUIRE(frame.Validate(plan.body.get(), &plan, registers, flags));
    std::map<u32, std::vector<const ir::Inst*>> owners;
    u32 retained{};
    for (const auto& inst : plan.body->GetInstList()) if (auto home = frame.Home(&inst)) {
        REQUIRE(*home % 32 == 0); REQUIRE(*home + 32 <= frame.Statistics().home_bytes);
        owners[*home].push_back(&inst); ++retained;
    }
    u32 shared{};
    for (const auto& [offset, values] : owners) if (values.size() > 1) {
        ++shared;
        for (u32 i = 0; i < values.size(); ++i) {
            REQUIRE_FALSE(registers.RequiresFullContainer(values[i]));
            REQUIRE(values[i]->GetOp() != O::AddPhi); REQUIRE_FALSE(flags.contains(const_cast<ir::Inst*>(values[i])));
            for (u32 j = i + 1; j < values.size(); ++j) REQUIRE_FALSE(registers.Interferes(values[i], values[j]));
        }
    }
    const auto& stats = frame.Statistics();
    REQUIRE(stats.retained_homes == retained); REQUIRE(stats.home_count == owners.size());
    REQUIRE(stats.reused_scalar_homes == retained - owners.size()); REQUIRE(stats.shared_scalar_home_slots == shared);
    REQUIRE(stats.home_bytes == owners.size() * 32);
}
void Pinned(const x64::Translator& code, backend::FunctionPlan& plan, ir::Value value) {
    const auto home = code.FramePlan().Home(value.Def()); REQUIRE(home);
    for (const auto& other : plan.body->GetInstList()) if (&other != value.Def()) {
        if (auto other_home = code.FramePlan().Home(&other)) REQUIRE(*other_home != *home);
    }
}
void LoopLabel(ir::Assembler& as, backend::FunctionPlan& plan, ir::Value edge, ir::Value first) {
    as.BindLabel(edge); auto* label = &plan.body->GetInstList().back();
    plan.body->RemoveInst(label); plan.body->InsertBefore(label, first.Def());
}
void Inputs(ir::Value destination, ir::Value initial, ir::Value backedge) {
    ir::Params inputs; inputs.Push(initial); inputs.Push(backedge); destination.Def()->SetArg(0, inputs);
}
u64 Binary(O op, u64 a, u64 b) {
    switch (op) {
        case O::Add: return a + b; case O::Sub: return a - b;
        case O::Adc: return a + b + 1; case O::Sbb: return a - b; // Guest carry is no-borrow.
        case O::And: return a & b; case O::Or: return a | b;
        case O::Xor: return a ^ b; case O::AndNot: return a & ~b;
        default: FAIL("uncovered independent binary model"); return 0;
    }
}
u64 AddFlags(u64 a, u64 b, T type) {
    const u64 mask = Mask(type), sign = u64{1} << (ir::GetValueSizeByte(type) * 8 - 1);
    a &= mask; b &= mask; const u64 result = (a + b) & mask;
    const bool overflow = !(a & sign) && (result & sign); // All callers add a positive constant.
    const u64 nzcv = (u64(bool(result & sign)) << 31) | (u64(result == 0) << 30) |
                     (u64(a > mask - b) << 29) | (u64(overflow) << 28);
    return (initial_flags & ~(u64{15} << 28)) | nzcv;
}
Config Configuration() { return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
    .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16}; }
} // namespace

TEST_CASE("scalar home reuse captures all eight binary inputs before dead lhs publication", "[x64][home][reuse][binary][cast][poison]") {
    for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::And, O::Or, O::Xor, O::AndNot})
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        CAPTURE(op, type, flags); const auto view = type == T::U64 ? T::U8 : T::U64;
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; std::array<ir::Value, lanes> pressure;
        for (u32 i = 0; i < lanes; ++i) pressure[i] = Load(as, 32 + i * 8);
        Carry(as); std::array<ir::Value, 9> chain; chain[0] = Load(as, 16, type);
        for (u32 i = 1; i < chain.size(); ++i)
            chain[i] = ir::Value{as.AppendInst(op, View(chain[i - 1], view), ir::Operand{View(chain[i - 1], T::U64)})}.SetType(type);
        Store(as, 128, View(chain.back(), T::U64));
        // Their higher benefit keeps the native allocation budget occupied
        // while the low-benefit chain spills; all eight remain simultaneously live.
        for (u32 repeat = 0; repeat < 10; ++repeat)
            for (u32 i = 0; i < lanes; ++i) Store(as, 256 + i * 8, pressure[i]);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::CfgRegisterOptions empty_budget; empty_budget.physical_registers.clear();
        const auto spilled = x64::CfgRegisterPlan::Build(plan.body.get(), plan, empty_budget);
        REQUIRE(spilled.Validate()); REQUIRE(spilled.PhysicalRegisters().empty());
        const auto forced = x64::SparseFramePlan::Build(plan.body.get(), &plan, spilled, {}); Layout(forced, spilled, plan);
        for (u32 i = 1; i < chain.size(); ++i) {
            REQUIRE(spilled.CanReuseBinaryInput(chain[i].Def(), chain[i - 1].Def()));
            REQUIRE(forced.Home(chain[i].Def()) == forced.Home(chain[i - 1].Def()));
        }
        for (u32 i = 0; i < lanes; ++i) for (u32 j = i + 1; j < lanes; ++j) {
            REQUIRE(spilled.Interferes(pressure[i].Def(), pressure[j].Def()));
            REQUIRE(forced.Home(pressure[i].Def()) != forced.Home(pressure[j].Def()));
        }
        x64::Translator baseline{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; baseline.Translate();
        x64::Translator reused{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; reused.Translate();
        Layout(reused.FramePlan(), reused.CfgPlan(), plan);
        u32 shared_binary{};
        for (u32 i = 1; i < chain.size(); ++i) {
            if (reused.FramePlan().Home(chain[i].Def()) && reused.FramePlan().Home(chain[i - 1].Def()) &&
                reused.FramePlan().Home(chain[i].Def()) == reused.FramePlan().Home(chain[i - 1].Def())) ++shared_binary;
        }
        REQUIRE(shared_binary > 0); REQUIRE(reused.FramePlan().Statistics().reused_scalar_homes > 0);
        REQUIRE(reused.FramePlan().Statistics().home_bytes < reused.FramePlan().Statistics().retained_homes * 32);
        REQUIRE(reused.FrameSize() < baseline.FrameSize());
        // This proves layout reduction with real SSA publication sites; it
        // deliberately makes no claim that reuse removes those transfers.
        REQUIRE(reused.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites > 0);
        REQUIRE(reused.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_store_sites > 0);
        PoisonCall old_call{baseline}, new_call{reused};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}, u64{0x100000100}}) {
            CAPTURE(seed); Machine old_state, actual, expected;
            for (auto* machine : {&old_state, &actual, &expected}) {
                machine->Put(16, seed); for (u32 i = 0; i < lanes; ++i) machine->Put(32 + i * 8, seed + i * 29);
            }
            u64 result = seed & Mask(type);
            for (u32 i = 1; i < chain.size(); ++i) result = Binary(op, result & Mask(view), result) & Mask(type);
            expected.Put(128, result); for (u32 i = 0; i < lanes; ++i) expected.Put(256 + i * 8, seed + i * 29);
            old_call.Run(old_state); new_call.Run(actual); Complete(old_state, actual, expected);
        }
    }
}

TEST_CASE("scalar home reuse protects simultaneous values and old sources after a returning helper", "[x64][home][reuse][helper][live]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; std::array<ir::Value, lanes> first, second;
        for (u32 i = 0; i < lanes; ++i) first[i] = Load(as, 32 + i * 8, type);
        for (u32 i = 0; i < lanes; ++i) second[i] = as.Add(first[i], ir::Operand{ir::Imm{u64{17 + i}}}).SetType(type);
        Carry(as); auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, first[0], second[1], first[2]).SetType(type);
        for (u32 i = 0; i < lanes; ++i) { Store(as, 256 + i * 8, View(first[i], T::U64)); Store(as, 384 + i * 8, View(second[i], T::U64)); }
        Store(as, 512, View(returned, T::U64)); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator baseline{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; baseline.Translate();
        x64::Translator reused{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; reused.Translate();
        Layout(reused.FramePlan(), reused.CfgPlan(), plan); REQUIRE(reused.Statistics().helper_call_sites > 0);
        u32 spilled_pairs{};
        for (u32 i = 0; i < lanes; ++i) {
            REQUIRE(reused.CfgPlan().Interferes(first[i].Def(), second[i].Def()));
            if (reused.FramePlan().Home(first[i].Def()) && reused.FramePlan().Home(second[i].Def())) {
                REQUIRE(reused.FramePlan().Home(first[i].Def()) != reused.FramePlan().Home(second[i].Def())); ++spilled_pairs;
            }
        }
        REQUIRE(spilled_pairs > 0); PoisonCall old_call{baseline}, new_call{reused};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            CAPTURE(type, flags, seed); Machine old_state, actual, expected; std::array<u64, lanes> a, b;
            for (u32 i = 0; i < lanes; ++i) {
                const u64 input = seed + i * 29; a[i] = input & Mask(type); b[i] = (a[i] + 17 + i) & Mask(type);
                for (auto* machine : {&old_state, &actual, &expected}) machine->Put(32 + i * 8, input);
                expected.Put(256 + i * 8, a[i]); expected.Put(384 + i * 8, b[i]);
            }
            expected.Put(512, (a[0] * 3 + b[1] * 5 + a[2] * 7) & Mask(type));
            old_call.Run(old_state); new_call.Run(actual); Complete(old_state, actual, expected);
        }
    }
}

TEST_CASE("scalar home reuse keeps diamond Phi destinations independent from spilled incoming values", "[x64][home][reuse][phi][diamond]") {
    for (auto type : {T::U8, T::U32, T::U64}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto selected = Load(as, 0, T::U8);
        std::array<ir::Value, lanes> first, second, phis;
        for (u32 i = 0; i < lanes; ++i) first[i] = Load(as, 32 + i * 8, type);
        auto skip = as.Goto(ir::BOOL{selected});
        for (u32 i = 0; i < lanes; ++i) second[i] = as.Add(first[i], ir::Operand{ir::Imm{u64{17 + i}}}).SetType(type);
        as.BindLabel(skip);
        for (u32 i = 0; i < lanes; ++i) { ir::Params inputs; inputs.Push(first[i]); inputs.Push(second[i]); phis[i] = as.AddPhi(inputs).SetType(type); }
        Carry(as); for (u32 i = 0; i < lanes; ++i) Store(as, 256 + i * 8, View(phis[i], T::U64));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator baseline{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; baseline.Translate();
        x64::Translator reused{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; reused.Translate();
        Layout(reused.FramePlan(), reused.CfgPlan(), plan); u32 spilled_sources{}, spilled_phis{};
        for (u32 i = 0; i < lanes; ++i) {
            if (reused.FramePlan().Home(first[i].Def())) ++spilled_sources;
            if (reused.FramePlan().Home(second[i].Def())) ++spilled_sources;
            if (reused.FramePlan().Home(phis[i].Def())) { Pinned(reused, plan, phis[i]); ++spilled_phis; }
        }
        REQUIRE(spilled_sources > 0); REQUIRE(spilled_phis > 0); REQUIRE(reused.Statistics().direct_phi_copy_sites > 0);
        PoisonCall old_call{baseline}, new_call{reused};
        for (u64 selector : {u64{0}, u64{1}}) for (u64 seed : {u64{0}, UINT64_MAX - 23, u64{0x8123456789abcdef}}) {
            CAPTURE(type, flags, selector, seed); Machine old_state, actual, expected;
            for (auto* machine : {&old_state, &actual, &expected}) {
                machine->Put(0, selector); for (u32 i = 0; i < lanes; ++i) machine->Put(32 + i * 8, seed + i * 29);
            }
            for (u32 i = 0; i < lanes; ++i) expected.Put(256 + i * 8, (((seed + i * 29) & Mask(type)) + (selector ? 0 : 17 + i)) & Mask(type));
            old_call.Run(old_state); new_call.Run(actual); Complete(old_state, actual, expected);
        }
    }
}

TEST_CASE("scalar home reuse preserves loop Phi edge snapshots around helpers", "[x64][home][reuse][phi][loop][helper]") {
    for (auto type : {T::U8, T::U32, T::U64}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto trips = Load(as, 0);
        std::array<ir::Value, lanes> seeds, phis, next;
        for (u32 i = 0; i < lanes; ++i) seeds[i] = Load(as, 32 + i * 8, type);
        for (auto& phi : phis) phi = as.AddPhi(ir::Params{}).SetType(type);
        auto count = as.AddPhi(ir::Params{}).SetType(T::U64); Carry(as);
        auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, phis[0], phis[1], phis[2]).SetType(T::U64); Store(as, 512, returned);
        for (u32 i = 0; i < lanes; ++i) next[i] = as.Add(phis[i], ir::Operand{ir::Imm{u64{17 + i}}}).SetType(type);
        auto remaining = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto edge = as.Goto(as.TestNotZero(remaining)); LoopLabel(as, plan, edge, phis.front());
        for (u32 i = 0; i < lanes; ++i) Inputs(phis[i], seeds[i], next[i]); Inputs(count, trips, remaining);
        for (u32 i = 0; i < lanes; ++i) { Store(as, 256 + i * 8, View(phis[i], T::U64)); Store(as, 384 + i * 8, View(next[i], T::U64)); }
        Store(as, 520, remaining); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator baseline{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; baseline.Translate();
        x64::Translator reused{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; reused.Translate();
        Layout(reused.FramePlan(), reused.CfgPlan(), plan); u32 spilled{};
        for (u32 i = 0; i < lanes; ++i) if (reused.FramePlan().Home(phis[i].Def())) { Pinned(reused, plan, phis[i]); ++spilled; }
        REQUIRE(spilled > 0); REQUIRE(reused.Statistics().direct_phi_copy_sites > 0); PoisonCall old_call{baseline}, new_call{reused};
        for (u64 iterations : {u64{1}, u64{2}, u64{3}, u64{11}}) {
            CAPTURE(type, flags, iterations); Machine old_state, actual, expected; std::array<u64, lanes> values;
            for (auto* machine : {&old_state, &actual, &expected}) { machine->Put(0, iterations); for (u32 i = 0; i < lanes; ++i) machine->Put(32 + i * 8, UINT64_MAX - i * 137); }
            for (u32 i = 0; i < lanes; ++i) {
                values[i] = ((UINT64_MAX - i * 137) + (iterations - 1) * (17 + i)) & Mask(type);
                expected.Put(256 + i * 8, values[i]); expected.Put(384 + i * 8, (values[i] + 17 + i) & Mask(type));
            }
            expected.Put(512, values[0] * 3 + values[1] * 5 + values[2] * 7); expected.Put(520, 0);
            old_call.Run(old_state); new_call.Run(actual); Complete(old_state, actual, expected);
        }
    }
}

TEST_CASE("scalar home reuse excludes raw opaque flags and paired producer storage", "[x64][home][reuse][raw][opaque][flags][pair]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto input = Load(as, 16), raw = Load(as, 32, T::V128);
        auto narrowed = as.BitCast(raw).SetType(T::U8); Store(as, 128, View(narrowed, T::V128));
        auto flagged = as.Add(input, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        auto target = as.LoadImm(ir::Imm{reinterpret_cast<u64>(&Helper)}).SetType(T::U64);
        ir::Params params; params.Push(input); params.Push(flagged); params.Push(input);
        auto returned = as.CallDynamic(ir::Lambda{target}, params).SetType(T::U64);
        auto quotient = as.Div128(as.LoadImm(ir::Imm{u64{0}}).SetType(T::U64), input,
                                 as.LoadImm(ir::Imm{u64{3}}).SetType(T::U64), ir::Imm{u64{0}}).SetType(T::U64);
        auto remainder = as.Div128Remainder(quotient).SetType(T::U64);
        auto paired = as.Cpuid(quotient, remainder, ir::Imm{u64{7}}, ir::Imm{reinterpret_cast<u64>(&Pair)}).SetType(T::U64);
        auto upper = as.CpuidUpper(paired).SetType(T::U64);
        Store(as, 160, View(quotient, T::V128)); Store(as, 176, View(remainder, T::V128));
        Store(as, 192, View(paired, T::V128)); Store(as, 208, View(upper, T::V128));
        Store(as, 224, returned); Store(as, 232, flagged); as.SaveFlags(flagged, ir::Flags::NZCV);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator baseline{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; baseline.Translate();
        x64::Translator reused{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; reused.Translate();
        const std::unordered_set<ir::Inst*> metadata{flagged.Def()}; Layout(reused.FramePlan(), reused.CfgPlan(), plan, metadata);
        for (auto value : {input, raw, narrowed, target, quotient, remainder, paired, upper, returned, flagged}) Pinned(reused, plan, value);
        REQUIRE(reused.CfgPlan().RequiresFullContainer(input.Def())); REQUIRE(reused.CfgPlan().RequiresFullContainer(target.Def()));
        REQUIRE(reused.Statistics().flags_snapshot_sites > 0); PoisonCall old_call{baseline}, new_call{reused};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x7ffffffffffffffb}, u64{0x8123456789abcdef}}) {
            CAPTURE(flags, seed); Machine old_state, actual, expected; constexpr u64 high = 0xdecba9876543210fULL;
            for (auto* machine : {&old_state, &actual, &expected}) { machine->Put(16, seed); machine->Put(32, seed ^ 0xa578c319ed4260bfULL); machine->Put(40, high); }
            expected.Put(128, seed ^ 0xa578c319ed4260bfULL); expected.Put(136, high);
            const u64 q = seed / 3, r = seed % 3, low = q + r + 7, upper_value = q ^ (r * 19) ^ 0x8123456789abcdefULL;
            expected.Put(160, q); expected.Put(168, r); expected.Put(176, r); expected.Put(184, 0);
            expected.Put(192, low); expected.Put(200, upper_value); expected.Put(208, upper_value); expected.Put(216, 0);
            expected.Put(224, seed * 3 + (seed + 7) * 5 + seed * 7); expected.Put(232, seed + 7);
            old_call.Run(old_state); new_call.Run(actual); Complete(old_state, actual, expected, AddFlags(seed, 7, T::U64));
        }
    }
}

TEST_CASE("scalar home reuse recovers a real protected page fault and reenters with complete state", "[x64][home][reuse][fault][production]") {
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool cfg : {false, true}) for (bool flags : {false, true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, cfg); policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; auto plan = Plan(); ir::Assembler as{plan.body.get()};
        std::array<ir::Value, lanes> pressure; for (u32 i = 0; i < lanes; ++i) pressure[i] = Load(as, 32 + i * 8);
        auto value = Load(as, 16); for (u32 i = 0; i < 8; ++i) value = as.Add(value, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        Store(as, 128, value); Carry(as);
        for (u32 repeat = 0; repeat < 10; ++repeat) for (u32 i = 0; i < lanes; ++i) Store(as, 256 + i * 8, pressure[i]);
        as.AdvancePC(ir::Imm{u32{4}}); auto loaded = as.LoadMemory(ir::Operand{Load(as, 0)}).SetType(T::U64); Store(as, 136, loaded);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator inventory{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, cfg, flags}; inventory.Translate();
        if (cfg) { Layout(inventory.FramePlan(), inventory.CfgPlan(), plan); REQUIRE(inventory.FramePlan().Statistics().reused_scalar_homes > 0); }
        REQUIRE(module->Push(plan.body.get())); REQUIRE(backend::TranslateIR(module, plan.body) != nullptr);
        // A high host pointer must reach the generated memory instruction:
        // bypass the software guest-window check in both protected/access runs.
        auto* state = runtime.GetState(); state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX;
        state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false, true, false}) {
            REQUIRE(mprotect(mapping.page, 4096, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 memory = 0x9988776655443322ULL, seed = UINT64_MAX - 35;
            if (accessible) std::memcpy(mapping.page, &memory, 8);
            Machine input; input.Put(0, reinterpret_cast<u64>(mapping.page)); input.Put(16, seed);
            for (u32 i = 0; i < lanes; ++i) input.Put(32 + i * 8, seed + i * 29);
            std::memcpy(runtime.GetUniformBuffer().data(), input.state->uniform_buffer_begin, extent);
            state->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000);
            Machine expected; std::memcpy(expected.bytes.data(), state, expected.bytes.size());
            expected.Put(128, seed + 56); if (accessible) expected.Put(136, memory);
            for (u32 i = 0; i < lanes; ++i) expected.Put(256 + i * 8, seed + i * 29);
            expected.state->current_loc = ir::Location{0x1004}; expected.state->prev_loc = ir::Location{0x1000};
            expected.state->halt_reason = HaltReason::None; expected.state->host_cpu_flags = carried_flags;
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0; expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0;
            expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto mxcsr = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal)); REQUIRE(_mm_getcsr() == mxcsr);
            REQUIRE(std::memcmp(state, expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}
