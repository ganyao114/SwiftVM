#include <array>
#include <cstring>
#include <set>
#include <unordered_set>
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
using D = x64::SparseHomeDecision;
constexpr u32 extent = 1024, pressure_count = 4;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
constexpr u64 carried_flags = initial_flags | (u64{1} << 29);
constexpr std::array<T, 8> types{T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64};
u64 Mask(T type) { const u32 bits = ir::GetValueSizeByte(type) * 8; return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() {
        for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13);
        state->host_cpu_flags = initial_flags;
    }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    void Finished(HaltReason reason = HaltReason::CallHost, u64 flags = carried_flags) {
        state->current_loc = ir::Location{0x1000}; state->prev_loc = ir::Location{0x1000};
        state->halt_reason = reason; state->host_cpu_flags = flags;
    }
};
backend::FunctionPlan Plan() { backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); return plan; }
ir::Value Load(ir::Assembler& as, u32 offset, T type = T::U64) { return as.LoadUniform(ir::Uniform{offset, type}).SetType(type); }
ir::Value Imm(ir::Assembler& as, u64 bits, T producer = T::U64, T payload = T::U64) {
    return as.LoadImm(ir::Imm{bits, payload}).SetType(producer);
}
ir::Value View(ir::Value value, T type) { return ir::Value{value.Def()}.SetCastType(type); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
void Carry(ir::Assembler& as) { as.SetCarry(Load(as, 16, T::U8)); }
std::array<ir::Value, pressure_count> Pressure(ir::Assembler& as) {
    std::array<ir::Value, pressure_count> values;
    for (u32 i = 0; i < pressure_count; ++i) values[i] = Load(as, 32 + i * 8);
    return values;
}
void ConsumePressure(ir::Assembler& as, const std::array<ir::Value, pressure_count>& values) {
    // These audited reads occupy the unchanged CFG register budget while
    // lower-benefit constants remain genuinely unassigned in both variants.
    for (u32 repeat = 0; repeat < 40; ++repeat)
        for (u32 i = 0; i < pressure_count; ++i) Store(as, 512 + i * 8, values[i]);
}
void Initialize(Machine& machine, u64 seed = 0x8123456789abcdefULL) {
    machine.Put(16, 1); for (u32 i = 0; i < pressure_count; ++i) machine.Put(32 + i * 8, seed + i * 29);
}
void PressureModel(Machine& machine, u64 seed = 0x8123456789abcdefULL) {
    for (u32 i = 0; i < pressure_count; ++i) machine.Put(512 + i * 8, seed + i * 29);
}
u64 Helper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return a * 3 + b * 5 + c * 7;
}
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize() + 4096 + 15) & ~u32{15};
        mov(rdx, rdi); mov(r11, rsp); sub(rsp, bytes); mov(rdi, rsp);
        mov(rcx, bytes / 8); mov(rax, 0xa5e39b7142c68df0ULL); cld(); rep(); stosq();
        mov(rsp, r11); mov(rdi, rdx); mov(rax, reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine, HaltReason reason = HaltReason::CallHost) {
        const auto mxcsr = _mm_getcsr(); REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == reason); REQUIRE(_mm_getcsr() == mxcsr);
    }
};
void Complete(const Machine& off, const Machine& on, Machine& model,
              HaltReason reason = HaltReason::CallHost, u64 flags = carried_flags) {
    model.Finished(reason, flags);
    REQUIRE(std::memcmp(off.bytes.data(), on.bytes.data(), on.bytes.size()) == 0);
    REQUIRE(std::memcmp(on.bytes.data(), model.bytes.data(), model.bytes.size()) == 0);
}
void SameAllocation(const x64::Translator& off, const x64::Translator& on) {
    const auto& first = off.CfgPlan(); const auto& second = on.CfgPlan();
    REQUIRE(first.Validate()); REQUIRE(second.Validate()); REQUIRE(first.PhysicalRegisters() == second.PhysicalRegisters());
    REQUIRE(first.Instructions() == second.Instructions()); REQUIRE(first.Allocations().size() == second.Allocations().size());
    for (const auto& allocation : first.Allocations()) {
        REQUIRE(first.Register(allocation.producer) == second.Register(allocation.producer));
        REQUIRE(first.RequiresCanonicalHome(allocation.producer) == second.RequiresCanonicalHome(allocation.producer));
        REQUIRE(first.RequiresFullContainer(allocation.producer) == second.RequiresFullContainer(allocation.producer));
        for (const auto& other : first.Allocations()) REQUIRE(first.Interferes(allocation.producer, other.producer) == second.Interferes(allocation.producer, other.producer));
    }
}
void Rematerialized(const x64::Translator& off, const x64::Translator& on, ir::Value constant, u64 normalized) {
    REQUIRE_FALSE(off.CfgPlan().Register(constant.Def())); REQUIRE_FALSE(on.CfgPlan().Register(constant.Def()));
    REQUIRE(off.FramePlan().Home(constant.Def())); REQUIRE_FALSE(off.FramePlan().RematerializedValue(constant.Def()));
    REQUIRE(on.FramePlan().Decision(constant.Def()) == D::Rematerialized); REQUIRE_FALSE(on.FramePlan().Home(constant.Def()));
    const auto value = on.FramePlan().RematerializedValue(constant.Def()); REQUIRE(value); REQUIRE(*value == normalized);
}
void LoopLabel(ir::Assembler& as, backend::FunctionPlan& plan, ir::Value edge, ir::Value first) {
    as.BindLabel(edge); auto* label = &plan.body->GetInstList().back(); plan.body->RemoveInst(label); plan.body->InsertBefore(label, first.Def());
}
void Inputs(ir::Value destination, ir::Value initial, ir::Value backedge) {
    ir::Params inputs; inputs.Push(initial); inputs.Push(backedge); destination.Def()->SetArg(0, inputs);
}
Config Configuration() { return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
    .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16}; }
} // namespace

TEST_CASE("scalar constants rematerialize normalized producer bits then all signed and unsigned views", "[x64][scalar][rematerialization][width][cast][imm][poison]") {
    constexpr std::array<u64, 3> bits{0, UINT64_MAX, 0x8123456789abcdefULL};
    for (auto producer : types) for (auto payload : types) for (bool flags : {false, true}) {
        CAPTURE(producer, payload, flags); auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); const auto pressure = Pressure(as);
        std::array<ir::Value, bits.size()> constants;
        for (u32 i = 0; i < bits.size(); ++i) constants[i] = Imm(as, bits[i], producer, payload);
        for (u32 i = 0; i < bits.size(); ++i) for (u32 j = 0; j < types.size(); ++j)
            Store(as, 128 + i * 64 + j * 8, View(constants[i], types[j]));
        ConsumePressure(as, pressure); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, false}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true}; on.Translate(); SameAllocation(off, on);
        REQUIRE_FALSE(off.FramePlan().RematerializationEnabled()); REQUIRE(on.FramePlan().RematerializationEnabled());
        REQUIRE(off.FramePlan().Statistics().omitted_rematerialized == 0); REQUIRE(on.FramePlan().Statistics().omitted_rematerialized == bits.size());
        REQUIRE(on.Statistics().scalar_rematerialized_definition_sites == bits.size()); REQUIRE((on.Statistics().scalar_rematerialized_read_sites + on.Statistics().scalar_binary_rematerialized_immediate_sites) == bits.size() * types.size());
        const auto& before = off.Statistics().At(x64::TransferRegion::CanonicalSSA); const auto& after = on.Statistics().At(x64::TransferRegion::CanonicalSSA);
        REQUIRE(before.scalar_load_sites == after.scalar_load_sites + bits.size() * types.size());
        REQUIRE(before.scalar_store_sites == after.scalar_store_sites + bits.size()); REQUIRE(on.FrameSize() < off.FrameSize());
        Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine); PressureModel(expected);
        for (u32 i = 0; i < bits.size(); ++i) {
            // Imm signed types expose their width's raw bits, never sign extension.
            const u64 normalized = bits[i] & Mask(payload) & Mask(producer); Rematerialized(off, on, constants[i], normalized);
            for (u32 j = 0; j < types.size(); ++j) {
                const u64 value = normalized & Mask(types[j]); const auto bytes = ir::GetValueSizeByte(types[j]);
                std::memcpy(expected.state->uniform_buffer_begin + 128 + i * 64 + j * 8, &value, bytes);
            }
        }
        PoisonCall old_call{off}, new_call{on}; old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected);
    }
}

TEST_CASE("rematerialized scalar constants survive loop Phi edges helpers and local conditions", "[x64][scalar][rematerialization][phi][loop][helper][condition]") {
    for (auto type : types) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto trips = Load(as, 0); Carry(as); const auto pressure = Pressure(as);
        auto seed = Imm(as, UINT64_MAX, type, T::S16), increment = Imm(as, 0x100000011ULL, type), selector = Imm(as, 0x100ULL, T::U64);
        auto phi = as.AddPhi(ir::Params{}).SetType(type), count = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, View(seed, T::U64), View(increment, T::U64), View(phi, T::U64)).SetType(T::U64);
        Store(as, 128, returned); auto skipped = as.Goto(ir::BOOL{selector});
        Store(as, 136, View(seed, T::U64)); as.BindLabel(skipped);
        auto next = as.Add(phi, ir::Operand{increment}).SetType(type);
        auto remaining = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto edge = as.Goto(as.TestNotZero(remaining)); LoopLabel(as, plan, edge, phi);
        Inputs(phi, seed, next); Inputs(count, trips, remaining); Store(as, 144, View(phi, T::U64)); Store(as, 152, View(next, T::U64)); Store(as, 160, remaining);
        ConsumePressure(as, pressure); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, false}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true}; on.Translate(); SameAllocation(off, on);
        const u64 first = 0xffff & Mask(type), step = 0x100000011ULL & Mask(type);
        Rematerialized(off, on, seed, first); Rematerialized(off, on, increment, step); Rematerialized(off, on, selector, 0x100);
        REQUIRE(on.Statistics().scalar_rematerialized_read_sites > 0); REQUIRE(on.Statistics().phi_value_copy_sites > 0);
        PoisonCall old_call{off}, new_call{on};
        for (u64 iterations : {u64{1}, u64{2}, u64{3}, u64{11}}) {
            CAPTURE(type, flags, iterations); Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { Initialize(*machine); machine->Put(0, iterations); }
            PressureModel(expected); const u64 value = (first + (iterations - 1) * step) & Mask(type);
            expected.Put(128, first * 3 + step * 5 + value * 7); expected.Put(136, first);
            expected.Put(144, value); expected.Put(152, (value + step) & Mask(type)); expected.Put(160, 0);
            old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected);
        }
    }
}

TEST_CASE("rematerialized constant terminal selectors retain exact cast bits and halt results", "[x64][scalar][rematerialization][terminal]") {
    for (u64 bits : {u64{0}, u64{1}, u64{0x100}, UINT64_MAX}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); const auto pressure = Pressure(as); auto selector = Imm(as, bits, T::S64);
        // Keep pressure live at the terminal so its selector cannot receive a
        // register after the final store, despite its lexical position.
        plan.body->SetTerminal(ir::terminal::If{ir::BOOL{selector},
            ir::terminal::If{ir::BOOL{pressure[0]}, ir::terminal::ReturnToHost{}, ir::terminal::Invalid{}},
            ir::terminal::If{ir::BOOL{pressure[1]}, ir::terminal::Invalid{}, ir::terminal::ReturnToHost{}}});
        ConsumePressure(as, pressure);
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, false}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true}; on.Translate(); SameAllocation(off, on); Rematerialized(off, on, selector, bits);
        REQUIRE((on.Statistics().scalar_rematerialized_read_sites + on.Statistics().scalar_binary_rematerialized_immediate_sites) == 1);
        for (u64 pressure_seed : {u64{0}, u64{1}, u64{0x100}}) {
            CAPTURE(bits, flags, pressure_seed); Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine, pressure_seed); PressureModel(expected, pressure_seed);
            const bool valid = u8(bits) ? bool(u8(pressure_seed)) : !bool(u8(pressure_seed + 29));
            const auto reason = valid ? HaltReason::CallHost : HaltReason::IllegalCode;
            PoisonCall old_call{off}, new_call{on}; old_call.Run(baseline, reason); new_call.Run(actual, reason); Complete(baseline, actual, expected, reason);
        }
    }
}

TEST_CASE("rematerialization excludes opaque raw decorated colored flags and nonconstant values", "[x64][scalar][rematerialization][exclusion]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); const auto pressure = Pressure(as);
    auto plain = Imm(as, 0), raw = Imm(as, UINT64_MAX, T::S8), input = Imm(as, 0x8123456789abcdefULL);
    auto target = Imm(as, reinterpret_cast<u64>(&Helper)), extra = Imm(as, 0x76543210ULL), flags_value = Imm(as, 0x80, T::U8);
    auto decorated = Imm(as, 0x1234); decorated.Def()->SetArg(1, ir::Imm{u64{99}});
    auto nonplain = as.Add(input, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64); extra.Def()->SetArg(1, input);
    ir::Params params; params.Push(input); params.Push(extra); params.Push(input);
    auto returned = as.CallDynamic(ir::Lambda{target}, params).SetType(T::U64);
    Store(as, 128, View(raw, T::V128)); Store(as, 144, returned); Store(as, 152, nonplain); Store(as, 160, extra); Store(as, 168, plain); Store(as, 176, decorated);
    as.SaveFlags(flags_value, ir::Flags::NZ); ConsumePressure(as, pressure); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    x64::CfgRegisterOptions budget; budget.physical_registers.clear(); const auto spill = x64::CfgRegisterPlan::Build(plan.body.get(), plan, budget); REQUIRE(spill.Validate());
    const std::unordered_set<ir::Inst*> flags_set{flags_value.Def()};
    const auto frame = x64::SparseFramePlan::Build(plan.body.get(), &plan, spill, flags_set, true);
    REQUIRE(frame.Validate(plan.body.get(), &plan, spill, flags_set, true)); REQUIRE(frame.RematerializedValue(plain.Def())); REQUIRE(*frame.RematerializedValue(plain.Def()) == 0);
    for (auto value : {raw, input, target, extra, flags_value, nonplain, returned, decorated}) {
        REQUIRE_FALSE(frame.RematerializedValue(value.Def())); REQUIRE(frame.Home(value.Def()));
    }
    x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, true, false}; off.Translate();
    x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, true, true}; on.Translate(); SameAllocation(off, on); Rematerialized(off, on, plain, 0);
    for (auto value : {raw, input, target, extra, nonplain, returned, decorated}) REQUIRE_FALSE(on.FramePlan().RematerializedValue(value.Def()));
    REQUIRE_FALSE(on.CfgPlan().Register(decorated.Def())); REQUIRE_FALSE(on.CfgPlan().RequiresFullContainer(decorated.Def())); REQUIRE(on.FramePlan().Home(decorated.Def()));
    // The standalone authority flags_set above deliberately pins this value.
    // Production only pins native metadata, so result-based LoadImm NZ can
    // safely reconstruct its input (covered separately across helpers below).
    Rematerialized(off, on, flags_value, 0x80);
    Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine); PressureModel(expected);
    expected.Put(128, 255); expected.Put(136, 0); expected.Put(144, 0x8123456789abcdefULL * 10 + 0x76543210ULL * 5);
    expected.Put(152, 0x8123456789abcdefULL + 7); expected.Put(160, 0x76543210ULL); expected.Put(168, 0); expected.Put(176, 0x1234);
    const u64 flags = (carried_flags & ~((u64{1} << 31) | (u64{1} << 30))) | (u64{1} << 31);
    PoisonCall old_call{off}, new_call{on}; old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected, HaltReason::CallHost, flags);
    // Assigned constants retain register publication; the optimization never
    // changes coloring or makes a register-resident value an omitted home.
    auto colored_plan = Plan(); ir::Assembler colored_as{colored_plan.body.get()}; auto colored = Imm(colored_as, UINT64_MAX, T::S16); Store(colored_as, 128, View(colored, T::U64)); colored_plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    x64::Translator colored_off{colored_plan.body.get(), {}, nullptr, &colored_plan, true, x64::ExecutionMode::Ordinary, true, false, false}; colored_off.Translate();
    x64::Translator colored_code{colored_plan.body.get(), {}, nullptr, &colored_plan, true, x64::ExecutionMode::Ordinary, true, false, true}; colored_code.Translate(); SameAllocation(colored_off, colored_code);
    REQUIRE(colored_code.CfgPlan().Register(colored.Def())); REQUIRE_FALSE(colored_code.FramePlan().RematerializedValue(colored.Def())); REQUIRE(colored_code.Statistics().scalar_rematerialized_definition_sites == 0);
    x64::Translator cfg_off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, true, true}; cfg_off.Translate();
    REQUIRE_FALSE(cfg_off.FramePlan().Enabled()); REQUIRE(cfg_off.Statistics().scalar_rematerialized_definition_sites == 0); REQUIRE(cfg_off.Statistics().scalar_rematerialized_read_sites == 0);
    Machine cfg_off_state, cfg_off_model; Initialize(cfg_off_state); Initialize(cfg_off_model); PressureModel(cfg_off_model);
    cfg_off_model.Put(128, 255); cfg_off_model.Put(136, 0); cfg_off_model.Put(144, 0x8123456789abcdefULL * 10 + 0x76543210ULL * 5);
    cfg_off_model.Put(152, 0x8123456789abcdefULL + 7); cfg_off_model.Put(160, 0x76543210ULL); cfg_off_model.Put(168, 0); cfg_off_model.Put(176, 0x1234);
    PoisonCall cfg_off_call{cfg_off}; cfg_off_call.Run(cfg_off_state); cfg_off_model.Finished(HaltReason::CallHost, flags);
    REQUIRE(std::memcmp(cfg_off_state.bytes.data(), cfg_off_model.bytes.data(), cfg_off_state.bytes.size()) == 0);
    Machine colored_baseline, colored_state, colored_model; colored_model.Put(128, 0xffff);
    PoisonCall colored_old_call{colored_off}, colored_call{colored_code}; colored_old_call.Run(colored_baseline); colored_call.Run(colored_state);
    Complete(colored_baseline, colored_state, colored_model, HaltReason::CallHost, initial_flags);
}

TEST_CASE("result based constant flags rematerialize after a returning helper without native metadata", "[x64][scalar][rematerialization][flags][helper]") {
    for (auto type : types) for (u64 bits : {u64{0}, u64{0x80}, u64{0x100}, UINT64_MAX}) for (bool flags : {false, true}) {
        CAPTURE(type, bits, flags); auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); const auto pressure = Pressure(as);
        auto constant = Imm(as, bits, type), sum = as.Add(pressure[0], ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, View(constant, T::U64), View(constant, T::U64), View(constant, T::U64)).SetType(T::U64);
        Store(as, 128, returned); Store(as, 136, View(constant, T::U64));
        // Native C/V comes from the arithmetic snapshot; N/Z/parity comes
        // from the normalized constant result reconstructed after the helper.
        as.SaveFlags(sum, ir::Flags::CV); as.SaveFlags(constant, ir::Flags::NZ | ir::Flags::Parity);
        ConsumePressure(as, pressure); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, false}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true}; on.Translate(); SameAllocation(off, on);
        const u64 value = bits & Mask(type); Rematerialized(off, on, constant, value);
        REQUIRE(on.FramePlan().Home(sum.Def())); REQUIRE_FALSE(on.FramePlan().RematerializedValue(sum.Def())); REQUIRE(on.Statistics().flags_snapshot_sites > 0);
        REQUIRE(on.Statistics().scalar_rematerialized_definition_sites == 1); REQUIRE((on.Statistics().scalar_rematerialized_read_sites + on.Statistics().scalar_binary_rematerialized_immediate_sites) == 5);
        Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine); PressureModel(expected);
        expected.Put(128, value * 15); expected.Put(136, value);
        const u64 sign = u64{1} << (ir::GetValueSizeByte(type) * 8 - 1);
        // Fixed pressure seed is negative and remains negative after +7: its
        // native addition has neither carry nor signed overflow.
        const u64 mask = (u64{15} << 28) | 255;
        const u64 updated = (initial_flags & ~mask) | (u64(bool(value & sign)) << 31) | (u64(value == 0) << 30) | (value & 255);
        PoisonCall old_call{off}, new_call{on}; old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected, HaltReason::CallHost, updated);
    }
}

TEST_CASE("function preparation propagates the scalar rematerialization setting with unchanged CFG", "[x64][scalar][rematerialization][preparation]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); const auto pressure = Pressure(as); auto constant = Imm(as, UINT64_MAX, T::S16);
    Store(as, 128, View(constant, T::U64)); ConsumePressure(as, pressure); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    auto off = x64::Translator::PrepareFunction(plan, {}, nullptr, true, true, true, {.scalar_rematerialization = false}); off->Translate();
    auto on = x64::Translator::PrepareFunction(plan, {}, nullptr, true, true, true, {.scalar_rematerialization = true}); on->Translate(); SameAllocation(*off, *on); Rematerialized(*off, *on, constant, 0xffff);
    REQUIRE(off->PreparationStats().full_allocation_builds == on->PreparationStats().full_allocation_builds);
    REQUIRE(off->Statistics().scalar_rematerialized_read_sites == 0); REQUIRE((on->Statistics().scalar_rematerialized_read_sites + on->Statistics().scalar_binary_rematerialized_immediate_sites) == 1);
    Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine); PressureModel(expected); expected.Put(128, 0xffff);
    PoisonCall old_call{*off}, new_call{*on}; old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected);
}

TEST_CASE("rematerialized constants preserve complete state through a real protected-page recovery", "[x64][scalar][rematerialization][fault][production]") {
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool cfg : {false, true}) for (bool flags : {false, true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, cfg); policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); const auto pressure = Pressure(as);
        auto constant = Imm(as, 0x8123456789abcdefULL, T::S16); Store(as, 128, View(constant, T::U64)); ConsumePressure(as, pressure);
        as.AdvancePC(ir::Imm{u32{4}}); auto loaded = as.LoadMemory(ir::Operand{Load(as, 0)}).SetType(T::U64); Store(as, 136, loaded);
        Store(as, 144, View(constant, T::U8)); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator inventory{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, cfg, flags, true}; inventory.Translate();
        if (cfg) { REQUIRE(inventory.FramePlan().RematerializedValue(constant.Def())); REQUIRE((inventory.Statistics().scalar_rematerialized_read_sites + inventory.Statistics().scalar_binary_rematerialized_immediate_sites) == 2); }
        REQUIRE(module->Push(plan.body.get())); REQUIRE(backend::TranslateIR(module, plan.body) != nullptr);
        auto* state = runtime.GetState(); state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX; state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false, true, false}) {
            REQUIRE(mprotect(mapping.page, 4096, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 memory = 0x9988776655443322ULL; if (accessible) std::memcpy(mapping.page, &memory, 8);
            Machine input; Initialize(input); input.Put(0, reinterpret_cast<u64>(mapping.page)); std::memcpy(runtime.GetUniformBuffer().data(), input.state->uniform_buffer_begin, extent);
            state->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000); Machine expected; std::memcpy(expected.bytes.data(), state, expected.bytes.size()); PressureModel(expected);
            expected.Put(128, 0xcdef); if (accessible) { expected.Put(136, memory); expected.state->uniform_buffer_begin[144] = 0xef; }
            expected.state->current_loc = ir::Location{0x1004}; expected.state->prev_loc = ir::Location{0x1000}; expected.state->halt_reason = HaltReason::None; expected.state->host_cpu_flags = carried_flags;
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0; expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0; expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto mxcsr = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal)); REQUIRE(_mm_getcsr() == mxcsr);
            REQUIRE(std::memcmp(state, expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}
