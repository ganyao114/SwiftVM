#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/flags_profitability.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using F = ir::Flags;
constexpr u32 extent = 1024;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Inst* Label() {
        auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label;
    }
    ir::Value Load(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, T::U64}, value.SetCastType(T::U64)); }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
};
void SameDecision(const x64::FlagsResidencyDecision& actual, const x64::FlagsResidencyDecision& expected) {
    REQUIRE(actual.selected == expected.selected); REQUIRE(actual.reason == expected.reason);
    REQUIRE(actual.weighted_flag_reads == expected.weighted_flag_reads); REQUIRE(actual.weighted_flag_writes == expected.weighted_flag_writes);
    REQUIRE(actual.weighted_carry_reads == expected.weighted_carry_reads); REQUIRE(actual.weighted_helper_calls == expected.weighted_helper_calls);
    REQUIRE(actual.weighted_exit_boundaries == expected.weighted_exit_boundaries); REQUIRE(actual.resident_overhead == expected.resident_overhead);
    REQUIRE(actual.scalar_lost_benefit == expected.scalar_lost_benefit); REQUIRE(actual.net_savings == expected.net_savings);
}
void SameAllocation(const x64::CfgRegisterPlan& actual, const x64::CfgRegisterPlan& expected) {
    REQUIRE(actual.Validate()); REQUIRE(expected.Validate()); REQUIRE(actual.Instructions() == expected.Instructions());
    REQUIRE(actual.PhysicalRegisters() == expected.PhysicalRegisters()); REQUIRE(actual.Allocations().size() == expected.Allocations().size());
    REQUIRE(actual.LoopWeightedCostsEnabled() == expected.LoopWeightedCostsEnabled());
    REQUIRE(actual.ScalarUnaryPhasesEnabled() == expected.ScalarUnaryPhasesEnabled());
    for (auto* left : actual.Instructions()) {
        REQUIRE(actual.Slot(left) == expected.Slot(left)); REQUIRE(actual.Register(left) == expected.Register(left));
        REQUIRE(actual.RequiresCanonicalHome(left) == expected.RequiresCanonicalHome(left));
        REQUIRE(actual.RequiresFullContainer(left) == expected.RequiresFullContainer(left));
        for (auto* right : actual.Instructions()) {
            REQUIRE(actual.Interferes(left, right) == expected.Interferes(left, right));
            REQUIRE(actual.CanReuseBinaryInput(left, right) == expected.CanReuseBinaryInput(left, right));
            REQUIRE(actual.CanReuseUnaryInput(left, right) == expected.CanReuseUnaryInput(left, right));
        }
    }
}
Config Configuration() {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
            .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16};
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13); state->host_cpu_flags = initial_flags; }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    void Complete() { state->current_loc = ir::Location{0x1000}; state->prev_loc = ir::Location{0x1000}; state->halt_reason = HaltReason::CallHost; }
};
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize() + 4096 + 15) & ~u32{15};
        mov(rdx, rdi); mov(r11, rsp); sub(rsp, bytes); mov(rdi, rsp); mov(rcx, bytes / 8);
        mov(rax, 0xa5e39b7142c68df0ULL); cld(); rep(); stosq(); mov(rsp, r11); mov(rdi, rdx);
        mov(rax, reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine) {
        const auto saved = _mm_getcsr(); REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost);
        REQUIRE(_mm_getcsr() == saved);
    }
};
struct Request { backend::State* state{}; u64 replacement{}; std::vector<u64> observed; };
u64 Helper(u64 pointer, u64 input, u64 invariant) {
    auto& request = *reinterpret_cast<Request*>(pointer); request.observed.push_back(request.state->host_cpu_flags);
    request.state->host_cpu_flags = request.replacement;
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return input ^ invariant;
}
void Inputs(ir::Value phi, ir::Value initial, ir::Value next) { ir::Params params; params.Push(initial); params.Push(next); phi.Def()->SetArg(0, params); }
void NativeLoop(Program& p) {
    auto pointer = p.Load(0), seed = p.Load(8), duplicate = p.Load(8), trips = p.Load(16), invariant = p.Load(24);
    p.Store(176, duplicate); p.Store(184, seed); auto* header = p.Label();
    auto acc = p.as.AddPhi(ir::Params{}).SetType(T::U64), count = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    // Unary and binary affinities are both part of the reused analysis.
    auto rotated = p.as.RorImm(acc, ir::Imm{u32{7}}).SetType(T::U64);
    auto returned = p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, pointer, rotated, invariant).SetType(T::U64);
    auto next = p.as.Adc(returned, ir::Operand{invariant}).SetType(T::U64); p.Store(128, returned); p.Store(136, next);
    // Carry is read by ADC and then toggled sixteen times. No arithmetic flags
    // are published, making full packed-word expectations independent of JIT metadata.
    for (u32 i = 0; i < 16; ++i) p.as.InvertCarry();
    auto remaining = p.as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    auto branch = p.as.Goto(ir::BOOL{p.as.TestNotZero(remaining).SetType(T::U8)}); p.function.targets[branch.Def()] = header;
    Inputs(acc, seed, next); Inputs(count, trips, remaining); p.Store(144, remaining); p.Store(152, invariant);
    p.Store(160, p.as.GetFlags(next, F::All).SetType(T::U64)); p.Finish();
}
void Model(Machine& expected, const Request& request, u64 seed, u64 trips, u64 invariant) {
    u64 next = seed, returned{};
    for (u64 i = 0; i < trips; ++i) { returned = std::rotr(next, 7) ^ invariant; next = returned + invariant + ((request.replacement >> 29) & 1); }
    expected.Put(128, returned); expected.Put(136, next); expected.Put(144, 0); expected.Put(152, invariant);
    expected.Put(176, seed); expected.Put(184, seed);
    expected.Put(160, request.replacement); expected.state->host_cpu_flags = request.replacement; expected.Complete();
}
} // namespace

TEST_CASE("flags selection recoloring preserves every cost and decision across policy inputs", "[x64][flags-recolor][equivalence]") {
    for (bool weighted : {false, true}) for (bool unary : {false, true}) for (u64 storage : {u64{0}, u64{8 * 1024 * 1024}})
        for (u32 shape = 0; shape < 6; ++shape) {
            Program p; auto source = p.Load(0);
            if (shape == 0) p.Store(128, source);
            if (shape == 1) p.as.ClearFlags(F::Carry);
            if (shape == 2) for (u32 i = 0; i < 16; ++i) p.as.ClearFlags(F::Carry);
            if (shape == 3) {
                std::array<ir::Value, 3> pressure{source, p.Load(8), p.Load(16)};
                for (u32 repeat = 0; repeat < 24; ++repeat) for (u32 i = 0; i < pressure.size(); ++i) p.Store(128 + i * 8, pressure[i]);
                p.as.ClearFlags(F::Carry);
            }
            if (shape == 4) {
                auto* header = p.Label(); p.as.ClearFlags(F::Carry);
                auto branch = p.as.Goto(ir::BOOL{p.Load(8)}); p.function.targets[branch.Def()] = header;
            }
            if (shape == 5) {
                auto vector = p.as.LoadUniform(ir::Uniform{256, T::V128}).SetType(T::V128); p.as.VecXor(vector, vector).SetType(T::V128);
                for (u32 i = 0; i < 16; ++i) p.as.ClearFlags(F::Carry);
            }
            p.Finish(); const auto text = p.function.body->ToString();
            const x64::CfgRegisterOptions options{.interference_storage_budget_bytes = storage, .loop_weighted_costs = weighted, .scalar_unary_phases = unary};
            const auto full = x64::SelectFlagsResidency(p.function.body.get(), p.function, options, false);
            const auto recolored = x64::SelectFlagsResidency(p.function.body.get(), p.function, options, true);
            SameDecision(recolored, full); REQUIRE(full.allocation_builds == 2); REQUIRE(full.allocation_recolors == 0);
            REQUIRE(recolored.allocation_builds == 1); REQUIRE(recolored.allocation_recolors == 1);
            REQUIRE(p.function.body->ToString() == text);
        }
}

TEST_CASE("private flags selection adopts the chosen immutable budget without another allocation", "[x64][flags-recolor][preparation][adoption]") {
    const auto config = Configuration();
    for (bool weighted : {false, true}) for (bool unary : {false, true}) for (bool selected : {false, true}) for (bool reuse : {false, true}) {
        Program p; auto source = p.Load(0), next = p.as.Neg(source).SetType(T::U64); p.Store(128, next);
        for (u32 i = 0; i < (selected ? 16 : 1); ++i) p.as.ClearFlags(F::Carry); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, true,
            {.loop_weighted_costs = weighted, .profitable_flags_residency = true, .scalar_unary_phases = unary, .flags_selection_recolor = reuse});
        code->Translate(); REQUIRE(code->FlagsResident() == selected); REQUIRE(code->FlagsDecision().selected == selected);
        REQUIRE(code->PreparationStats().reused_final_allocation); REQUIRE(code->PreparationStats().full_allocation_builds == 0);
        REQUIRE(code->PreparationStats().flags_policy_allocation_builds == (reuse ? 1 : 2));
        REQUIRE(code->PreparationStats().flags_policy_allocation_recolors == (reuse ? 1 : 0));
        const auto budget = x64::Translator::AllocationBudget(p.function.body.get(), &p.function, selected, weighted, unary);
        SameAllocation(code->CfgPlan(), x64::CfgRegisterPlan::Build(p.function.body.get(), p.function, budget));
        PoisonCall call{*code}; for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8000000000000000}}) {
            Machine actual, expected; actual.Put(0, seed); expected.Put(0, seed); expected.Put(128, 0 - seed);
            expected.state->host_cpu_flags = initial_flags & ~(u64{1} << 29); expected.Complete(); call.Run(actual); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("flags selection discards pre-forwarding plans and adopts final requested policy", "[x64][flags-recolor][preparation][forwarding]") {
    const auto config = Configuration();
    for (bool weighted : {false, true}) for (bool unary : {false, true}) for (bool hot : {false, true}) for (bool reuse : {false, true}) {
        Program p; auto source = p.Load(0), duplicate = p.Load(0); p.Store(128, duplicate); p.Store(136, source); p.Store(144, source);
        for (u32 i = 0; i < (hot ? 16 : 1); ++i) p.as.ClearFlags(F::Carry); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, true,
            {.uniform_forwarding = true, .loop_weighted_costs = weighted, .profitable_flags_residency = true,
             .scalar_unary_phases = unary, .flags_selection_recolor = reuse}); code->Translate();
        REQUIRE(code->FlagsResident() == hot); REQUIRE(duplicate.Def()->GetOp() == O::BitExtract);
        REQUIRE(code->UniformForwardStats().loads_forwarded == 1); REQUIRE(code->PreparationStats().reused_final_allocation);
        REQUIRE(code->PreparationStats().full_allocation_builds == 1);
        REQUIRE(code->PreparationStats().flags_policy_allocation_builds == (reuse ? 1 : 2));
        REQUIRE(code->PreparationStats().flags_policy_allocation_recolors == (reuse ? 1 : 0));
        const auto budget = x64::Translator::AllocationBudget(p.function.body.get(), &p.function, hot, weighted, unary);
        SameAllocation(code->CfgPlan(), x64::CfgRegisterPlan::Build(p.function.body.get(), p.function, budget));
    }
}

TEST_CASE("flags selection analysis reuse preserves helper mutation carry Phi and full State", "[x64][flags-recolor][native][helper][phi]") {
    const auto config = Configuration();
    for (bool forwarding : {false, true}) for (bool unary : {false, true}) {
        Program baseline_program, full_program, recolor_program; NativeLoop(baseline_program); NativeLoop(full_program); NativeLoop(recolor_program);
        auto baseline = x64::Translator::PrepareFunction(baseline_program.function, {}, &config, true, true, false,
            {.uniform_forwarding = forwarding, .scalar_unary_phases = unary});
        auto full = x64::Translator::PrepareFunction(full_program.function, {}, &config, true, true, true,
            {.uniform_forwarding = forwarding, .profitable_flags_residency = true, .scalar_unary_phases = unary, .flags_selection_recolor = false});
        auto recolor = x64::Translator::PrepareFunction(recolor_program.function, {}, &config, true, true, true,
            {.uniform_forwarding = forwarding, .profitable_flags_residency = true, .scalar_unary_phases = unary, .flags_selection_recolor = true});
        baseline->Translate(); full->Translate(); recolor->Translate(); REQUIRE_FALSE(baseline->FlagsResident());
        REQUIRE(full->FlagsResident()); REQUIRE(recolor->FlagsResident()); SameDecision(recolor->FlagsDecision(), full->FlagsDecision());
        REQUIRE(recolor->Statistics().flags_resident_carry_read_sites == 1); REQUIRE(baseline->Statistics().flags_state_carry_read_sites == 1);
        for (const auto* code : {full.get(), recolor.get()}) {
            REQUIRE(code->CfgPlan().ScalarUnaryPhasesEnabled() == unary); REQUIRE(code->CfgPlan().LoopWeightedCostsEnabled());
            REQUIRE(code->PreparationStats().full_allocation_builds == (forwarding ? 1 : 0));
            REQUIRE(code->PreparationStats().reused_final_allocation);
            REQUIRE(code->UniformForwardStats().loads_forwarded == (forwarding ? 1 : 0));
        }
        PoisonCall old_call{*baseline}, full_call{*full}, recolor_call{*recolor};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8000000000000000}, u64{0x8123456789abcdef}})
            for (u64 trips : {u64{1}, u64{2}, u64{7}}) for (u64 carry : {u64{0}, u64{1}}) {
                CAPTURE(forwarding, unary, seed, trips, carry); constexpr u64 invariant = 17;
                Request request; request.replacement = (initial_flags & ~(u64{1} << 29)) | (carry << 29);
                Machine ordinary, rebuilt, actual, expected;
                for (auto* machine : {&ordinary, &rebuilt, &actual, &expected}) {
                    machine->Put(0, reinterpret_cast<u64>(&request)); machine->Put(8, seed); machine->Put(16, trips); machine->Put(24, invariant);
                }
                Model(expected, request, seed, trips, invariant);
                std::vector<u64> seen(trips, request.replacement); seen.front() = initial_flags;
                for (auto [call, machine] : std::array<std::pair<PoisonCall*, Machine*>, 3>{{{&old_call, &ordinary}, {&full_call, &rebuilt}, {&recolor_call, &actual}}}) {
                    request.state = machine->state; request.observed.clear(); call->Run(*machine);
                    REQUIRE(request.observed == seen); REQUIRE(machine->bytes == expected.bytes);
                }
            }
    }
}
