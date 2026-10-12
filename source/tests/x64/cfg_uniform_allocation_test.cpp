#include <array>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/cfg_uniform_forward.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
constexpr u32 extent = 1024;
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
void Data(std::ostream& out, const ir::DataClass& data) {
    if (data.IsValue()) out << "value:" << reinterpret_cast<uintptr_t>(data.value.Def()) << ':' << u32(data.value.Type());
    else if (data.IsImm()) out << "imm:" << u32(data.imm.GetType()) << ':' << data.imm.Get();
    else out << "void";
}
std::string Arguments(ir::Inst* inst) {
    std::ostringstream out;
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        auto& arg = inst->ArgAt(slot); out << slot << '=' << u32(arg.GetType()) << ':';
        if (arg.IsVoid() || arg.IsValue() || arg.IsImm()) Data(out, arg.ToDataClass());
        else if (arg.GetType() == ir::ArgType::Uniform) { const auto uniform = arg.Get<ir::Uniform>(); out << uniform.GetOffset() << ':' << u32(uniform.GetType()); }
        else if (arg.IsLambda()) { const auto lambda = arg.Get<ir::Lambda>(); Data(out, lambda.IsValue() ? ir::DataClass{lambda.GetValue()} : ir::DataClass{lambda.GetImm()}); out << ':' << u32(lambda.GetUniformEffectId()) << ':' << u32(lambda.GetHelperABI()); }
        else if (arg.IsParams()) for (const auto& parameter : arg.Get<ir::Params>()) { Data(out, parameter.data); out << ','; }
        else FAIL("fixture snapshot encountered an unexpected physical argument kind");
        out << ';';
    }
    return out.str();
}
struct Snapshot {
    ir::Inst* inst; u32 id; T type; O op; std::string args;
    explicit Snapshot(ir::Inst* value) : inst(value), id(value->Id()), type(value->ReturnType()), op(value->GetOp()), args(Arguments(value)) {}
    void Unchanged() const { REQUIRE(inst->Id() == id); REQUIRE(inst->ReturnType() == type); REQUIRE(inst->GetOp() == op); REQUIRE(Arguments(inst) == args); }
};
std::vector<Snapshot> StoreSnapshots(backend::FunctionPlan& plan) {
    std::vector<Snapshot> result; for (auto& inst : plan.body->GetInstList()) if (inst.GetOp() == O::StoreUniform) result.emplace_back(&inst); return result;
}
std::vector<ir::Inst*> Order(backend::FunctionPlan& plan) {
    std::vector<ir::Inst*> result; for (auto& inst : plan.body->GetInstList()) result.push_back(&inst); return result;
}
void Run(x64::Translator& code, Machine& machine) { REQUIRE(code.getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost); }
void Same(const Machine& baseline, const Machine& actual, const Machine& expected) {
    REQUIRE(std::memcmp(baseline.bytes.data(), actual.bytes.data(), actual.bytes.size()) == 0);
    REQUIRE(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, extent) == 0);
    REQUIRE(actual.state->host_cpu_flags == (initial_flags | (u64{1} << 29)));
}
u64 Mutate(u64 pointer, u64 value, u64) {
    auto* buffer = reinterpret_cast<u8*>(pointer); const u64 replacement = value ^ 0x8f123456789abcdeULL;
    std::memcpy(buffer + 16, &replacement, 8);
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return replacement + 7;
}
Config Configuration() { return {.backend_isa = kX86_64, .uniform_buffer_size = extent, .stack_alignment = 16}; }
void FreshEquivalent(x64::Translator& prepared, backend::FunctionPlan& plan, bool flags) {
    const auto fresh = x64::CfgRegisterPlan::Build(plan.body.get(), plan,
        x64::Translator::AllocationBudget(plan.body.get(), &plan, flags));
    REQUIRE(fresh.Enabled()); REQUIRE(fresh.Validate());
    const auto& reused = prepared.CfgPlan(); REQUIRE(reused.Validate());
    REQUIRE(fresh.PhysicalRegisters() == reused.PhysicalRegisters());
    REQUIRE(fresh.Allocations().size() == reused.Allocations().size());
    for (const auto& allocation : fresh.Allocations()) {
        REQUIRE(fresh.Register(allocation.producer) == reused.Register(allocation.producer));
        REQUIRE(fresh.RequiresCanonicalHome(allocation.producer) == reused.RequiresCanonicalHome(allocation.producer));
        REQUIRE(fresh.RequiresFullContainer(allocation.producer) == reused.RequiresFullContainer(allocation.producer));
        for (const auto& other : fresh.Allocations())
            REQUIRE(fresh.Interferes(allocation.producer, other.producer) == reused.Interferes(allocation.producer, other.producer));
    }
}
void EquivalentCode(x64::Translator& prepared, backend::FunctionPlan& prepared_plan,
                    x64::Translator& ordinary, backend::FunctionPlan& ordinary_plan) {
    const auto left = Order(prepared_plan), right = Order(ordinary_plan); REQUIRE(left.size() == right.size());
    REQUIRE(prepared.FrameSize() == ordinary.FrameSize()); REQUIRE(prepared.getSize() == ordinary.getSize());
    for (u32 i = 0; i < left.size(); ++i) {
        REQUIRE(left[i]->GetOp() == right[i]->GetOp()); REQUIRE(left[i]->ReturnType() == right[i]->ReturnType());
        REQUIRE(prepared.CfgPlan().Register(left[i]) == ordinary.CfgPlan().Register(right[i]));
        REQUIRE(prepared.CfgPlan().RequiresCanonicalHome(left[i]) == ordinary.CfgPlan().RequiresCanonicalHome(right[i]));
        REQUIRE(prepared.CfgPlan().RequiresFullContainer(left[i]) == ordinary.CfgPlan().RequiresFullContainer(right[i]));
        REQUIRE(prepared.FramePlan().Home(left[i]) == ordinary.FramePlan().Home(right[i]));
    }
    const auto& a = prepared.Statistics(); const auto& b = ordinary.Statistics();
    REQUIRE(a.flags_snapshot_sites == b.flags_snapshot_sites); REQUIRE(a.phi_value_copy_sites == b.phi_value_copy_sites);
    REQUIRE(a.helper_call_sites == b.helper_call_sites); REQUIRE(a.scalar_cache_read_sites == b.scalar_cache_read_sites);
    REQUIRE(a.scalar_cache_write_sites == b.scalar_cache_write_sites);
    for (u32 i = 0; i < u32(x64::TransferRegion::Count); ++i) {
        const auto& l = a.transfers[i]; const auto& r = b.transfers[i];
        REQUIRE(l.scalar_load_sites == r.scalar_load_sites); REQUIRE(l.scalar_store_sites == r.scalar_store_sites);
        REQUIRE(l.vector_load_sites == r.vector_load_sites); REQUIRE(l.vector_store_sites == r.vector_store_sites);
        REQUIRE(l.load_bytes == r.load_bytes); REQUIRE(l.store_bytes == r.store_bytes);
    }
}
backend::FunctionPlan ConditionalPlan() {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto source = Load(as, 8); Store(as, 16, source);
    auto selector = Load(as, 0, T::U8); auto skip = as.Goto(ir::BOOL{selector}); Carry(as); as.BindLabel(skip);
    plan.locations[&plan.body->GetInstList().back()] = ir::Location{0x1004};
    auto loaded = Load(as, 16); Store(as, 128, as.Add(loaded, ir::Operand{ir::Imm{u64{11}}}).SetType(T::U64));
    auto pointer = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
    auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Mutate)}}, pointer, loaded, loaded).SetType(T::U64);
    Store(as, 136, Load(as, 16)); Store(as, 144, returned); Carry(as);
    plan.body->SetTerminal(ir::terminal::ReturnToHost{}); plan.body->ReIdInstr(); return plan;
}
u64 LoopHelper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return a * 3 + b * 5 + c * 7;
}
} // namespace

TEST_CASE("allocation-aware forwarding rolls back uncolored sources and preserves decorated physical loads", "[x64][cfg][uniform][allocation][rollback]") {
    for (bool flags : {false, true}) for (u32 decoration : {u32{0}, u32{1}, u32{2}, u32{3}}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto raw = Load(as, 32, T::V128);
        // BitCast publishes a scalar annotation through WriteVector and is
        // deliberately outside the scalar allocator's cacheable inventory.
        auto source = as.BitCast(raw).SetType(T::U64); Store(as, 16, View(source, T::U64));
        auto load = Load(as, 16); Store(as, 128, load);
        auto cheap = Load(as, 48); Store(as, 64, cheap); auto decorated = Load(as, 64);
        if (decoration == 0) decorated.Def()->SetArg(4, View(cheap, T::U64));
        else if (decoration == 1) decorated.Def()->SetArg(4, View(cheap, T::V128));
        else if (decoration == 2) decorated.Def()->SetArg(4, ir::Lambda{cheap});
        else { ir::Params params; params.Push(View(cheap, T::U64)); params.Push(View(cheap, T::V128)); decorated.Def()->SetArg(4, params); }
        Store(as, 136, decorated); Carry(as); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        plan.body->ReIdInstr(); const auto order = Order(plan); const auto stores = StoreSnapshots(plan);
        const Snapshot before_load{load.Def()}, before_decorated{decorated.Def()};
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator baseline{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; baseline.Translate();
        auto graph = x64::CfgRegisterPlan::ValidateGraph(plan.body.get(), plan); REQUIRE(graph.Enabled());
        // The explicit budget matches adaptive flag residency in Translator.
        auto stats = x64::ForwardCfgUniformLoads(plan, extent, graph, x64::CfgUniformForwardOptions{
            .require_scalar_source_register = true,
            .allocation_budget = x64::Translator::AllocationBudget(plan.body.get(), &plan, flags)});
        REQUIRE(stats.accepted); REQUIRE(stats.loads_forwarded == 0); REQUIRE(stats.rejected_scalar_sources > 0);
        REQUIRE(stats.allocation_policy_enabled); REQUIRE(stats.final_scalar_sources_allocated);
        before_load.Unchanged(); before_decorated.Unchanged();
        REQUIRE(Order(plan) == order); for (const auto& store : stores) store.Unchanged();
        x64::Translator guarded{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; guarded.Translate();
        REQUIRE_FALSE(guarded.CfgPlan().Register(source.Def())); REQUIRE(guarded.FlagsResident() == flags);
        REQUIRE(guarded.CfgPlan().PhysicalRegisters() == (flags ? std::vector<unsigned>{5, 15} : std::vector<unsigned>{3, 5, 15}));
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            CAPTURE(flags, decoration, seed); Machine old, base, actual, expected;
            for (auto* machine : {&old, &base, &actual, &expected}) { machine->Put(32, seed); machine->Put(40, 0x8f123456789abcdeULL); machine->Put(48, seed ^ 0x714ae39b5c680df2ULL); }
            expected.Put(16, seed); expected.Put(128, seed); expected.Put(64, seed ^ 0x714ae39b5c680df2ULL); expected.Put(136, seed ^ 0x714ae39b5c680df2ULL);
            Run(legacy, old); Run(baseline, base); Run(guarded, actual); Same(old, actual, expected); Same(base, actual, expected);
        }
    }
}

TEST_CASE("allocation-aware conditional forwarding retains colored sources and inherits opaque helper barriers", "[x64][cfg][uniform][allocation][conditional][helper]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto source = Load(as, 8); Store(as, 16, source);
        auto selector = Load(as, 0, T::U8); auto skip = as.Goto(ir::BOOL{selector});
        Carry(as); as.BindLabel(skip); auto* join = &plan.body->GetInstList().back(); plan.locations[join] = ir::Location{0x1004};
        auto loaded = Load(as, 16); auto sum = as.Add(loaded, ir::Operand{ir::Imm{u64{11}}}).SetType(T::U64); Store(as, 128, sum);
        auto pointer = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
        auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Mutate)}}, pointer, loaded, loaded).SetType(T::U64);
        auto after = Load(as, 16); Store(as, 136, after); Store(as, 144, returned); Carry(as);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{}); plan.body->ReIdInstr();
        const auto order = Order(plan); const auto stores = StoreSnapshots(plan); const Snapshot load_before{loaded.Def()}, after_before{after.Def()};
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator baseline{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; baseline.Translate();
        const auto graph = x64::CfgRegisterPlan::ValidateGraph(plan.body.get(), plan); REQUIRE(graph.Enabled());
        const auto stats = x64::ForwardCfgUniformLoads(plan, extent, graph, x64::CfgUniformForwardOptions{
            .require_scalar_source_register = true,
            .allocation_budget = x64::Translator::AllocationBudget(plan.body.get(), &plan, flags)});
        REQUIRE(stats.accepted); REQUIRE(stats.loads_forwarded == 1); REQUIRE(stats.barriers >= 2);
        REQUIRE(stats.allocation_policy_enabled); REQUIRE(stats.final_scalar_sources_allocated);
        REQUIRE(loaded.Def()->GetOp() == O::BitExtract); REQUIRE(loaded.Def()->Id() == load_before.id); REQUIRE(loaded.Def()->ReturnType() == load_before.type);
        REQUIRE(loaded.Def()->GetArg<ir::Value>(0).Def() == source.Def()); REQUIRE(loaded.Def()->GetArg<ir::Imm>(1).Get() == 0); REQUIRE(loaded.Def()->GetArg<ir::Imm>(2).Get() == 64);
        for (u32 slot = 3; slot < ir::Inst::max_args; ++slot) REQUIRE(loaded.Def()->ArgAt(slot).IsVoid());
        after_before.Unchanged(); REQUIRE(Order(plan) == order); for (const auto& store : stores) store.Unchanged();
        x64::Translator guarded{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; guarded.Translate();
        REQUIRE(guarded.CfgPlan().Register(source.Def())); REQUIRE(guarded.FlagsResident() == flags);
        REQUIRE(guarded.CfgPlan().PhysicalRegisters() == (flags ? std::vector<unsigned>{5, 15} : std::vector<unsigned>{3, 5, 15}));
        for (u64 selector_value : {u64{0}, u64{1}}) for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            CAPTURE(flags, selector_value, seed); Machine old, base, actual, expected;
            for (auto* machine : {&old, &base, &actual, &expected}) { machine->Put(0, selector_value); machine->Put(8, seed); }
            const u64 replacement = seed ^ 0x8f123456789abcdeULL;
            expected.Put(16, replacement); expected.Put(128, seed + 11); expected.Put(136, replacement); expected.Put(144, replacement + 7);
            Run(legacy, old); Run(baseline, base); Run(guarded, actual); Same(old, actual, expected); Same(base, actual, expected);
        }
    }
}

TEST_CASE("function preparation reuses the final allocation with independent conditional pipeline equivalence", "[x64][cfg][prepare][reuse][conditional][helper]") {
    for (bool flags : {false, true}) {
        auto factory_plan = ConditionalPlan(), manual_plan = ConditionalPlan(); const auto config = Configuration();
        x64::Translator baseline{factory_plan.body.get(), {}, &config, &factory_plan, true, x64::ExecutionMode::Ordinary, true, flags}; baseline.Translate();
        const auto factory_order = Order(factory_plan); const auto factory_stores = StoreSnapshots(factory_plan);
        auto prepared = x64::Translator::PrepareFunction(factory_plan, {}, &config, true, true, flags,
            x64::FunctionPreparationOptions{.uniform_forwarding = true, .capture_forward_changes = true});
        prepared->Translate();
        const auto graph = x64::CfgRegisterPlan::ValidateGraph(manual_plan.body.get(), manual_plan);
        const auto manual_stats = x64::ForwardCfgUniformLoads(manual_plan, extent, graph, x64::CfgUniformForwardOptions{
            .require_scalar_source_register = true, .allocation_budget = x64::Translator::AllocationBudget(manual_plan.body.get(), &manual_plan, flags)});
        REQUIRE(manual_stats.accepted); REQUIRE(manual_stats.loads_forwarded == 1);
        x64::Translator ordinary{manual_plan.body.get(), {}, &config, &manual_plan, true, x64::ExecutionMode::Ordinary, true, flags}; ordinary.Translate();
        REQUIRE(prepared->PreparationStats().reused_final_allocation); REQUIRE(prepared->PreparationStats().full_allocation_builds > 0);
        REQUIRE(prepared->PreparationStats().full_allocation_builds == prepared->UniformForwardStats().allocation_rounds);
        REQUIRE(ordinary.PreparationStats().full_allocation_builds == 1); REQUIRE_FALSE(ordinary.PreparationStats().reused_final_allocation);
        REQUIRE(prepared->UniformForwardStats().loads_forwarded == manual_stats.loads_forwarded);
        REQUIRE(prepared->ForwardChanges().size() == prepared->UniformForwardStats().loads_forwarded);
        REQUIRE(prepared->FlagsResident() == flags); FreshEquivalent(*prepared, factory_plan, flags);
        EquivalentCode(*prepared, factory_plan, ordinary, manual_plan);
        REQUIRE(Order(factory_plan) == factory_order); for (const auto& store : factory_stores) store.Unchanged();
        for (u64 selector : {u64{0}, u64{1}}) for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            CAPTURE(flags, selector, seed); Machine base, actual, independent, expected;
            for (auto* machine : {&base, &actual, &independent, &expected}) { machine->Put(0, selector); machine->Put(8, seed); }
            const u64 replacement = seed ^ 0x8f123456789abcdeULL;
            expected.Put(16, replacement); expected.Put(128, seed + 11); expected.Put(136, replacement); expected.Put(144, replacement + 7);
            Run(baseline, base); Run(*prepared, actual); Run(ordinary, independent); Same(base, actual, expected); Same(independent, actual, expected);
        }
    }
}

TEST_CASE("prepared final IR still scans loop Phi helpers delayed flags and early pseudo homes", "[x64][cfg][prepare][loop][phi][flags][pseudo]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto trips = Load(as, 0), seed_a = Load(as, 8), seed_b = Load(as, 24);
        auto a = as.AddPhi(ir::Params{}).SetType(T::U64), b = as.AddPhi(ir::Params{}).SetType(T::U64), count = as.AddPhi(ir::Params{}).SetType(T::U64);
        Store(as, 32, a); auto forwarded = Load(as, 32);
        auto sum = as.Add(forwarded, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        auto quotient = as.Div128(as.LoadImm(ir::Imm{u64{0}}).SetType(T::U64), b, as.LoadImm(ir::Imm{u64{3}}).SetType(T::U64), ir::Imm{u64{0}}).SetType(T::U64);
        auto helper = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&LoopHelper)}}, forwarded, b, count).SetType(T::U64);
        auto remainder = as.Div128Remainder(quotient).SetType(T::U64);
        Store(as, 128, sum); Store(as, 136, helper); Store(as, 144, quotient); Store(as, 152, remainder); as.SaveFlags(sum, ir::Flags::NZCV);
        auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto edge = as.Goto(as.TestNotZero(next)); as.BindLabel(edge);
        auto* label = &plan.body->GetInstList().back(); plan.body->RemoveInst(label); plan.body->InsertBefore(label, a.Def());
        ir::Params a_inputs; a_inputs.Push(seed_a); a_inputs.Push(b); a.Def()->SetArg(0, a_inputs);
        ir::Params b_inputs; b_inputs.Push(seed_b); b_inputs.Push(a); b.Def()->SetArg(0, b_inputs);
        ir::Params count_inputs; count_inputs.Push(trips); count_inputs.Push(next); count.Def()->SetArg(0, count_inputs);
        Store(as, 160, a); Store(as, 168, b); Store(as, 176, next); plan.body->SetTerminal(ir::terminal::ReturnToHost{}); plan.body->ReIdInstr();
        const auto config = Configuration(); const auto stores = StoreSnapshots(plan);
        x64::Translator baseline{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; baseline.Translate();
        auto prepared = x64::Translator::PrepareFunction(plan, {}, &config, true, true, flags, x64::FunctionPreparationOptions{.uniform_forwarding = true}); prepared->Translate();
        x64::Translator fresh{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; fresh.Translate();
        FreshEquivalent(*prepared, plan, flags); EquivalentCode(*prepared, plan, fresh, plan); for (const auto& store : stores) store.Unchanged();
        REQUIRE(prepared->Statistics().flags_snapshot_sites > 0); REQUIRE(prepared->Statistics().phi_value_copy_sites > 0); REQUIRE(prepared->Statistics().helper_call_sites > 0);
        REQUIRE(prepared->FramePlan().Home(sum.Def())); REQUIRE(prepared->FramePlan().Home(quotient.Def())); REQUIRE(prepared->FramePlan().Home(remainder.Def()));
        REQUIRE(prepared->FramePlan().Home(quotient.Def()) != prepared->FramePlan().Home(remainder.Def()));
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) for (u64 iterations : {u64{1}, u64{2}, u64{7}}) {
            CAPTURE(flags, seed, iterations); Machine base, actual, independent, expected; const u64 other = seed ^ 0x714ae39b5c680df2ULL;
            for (auto* machine : {&base, &actual, &independent, &expected}) { machine->Put(0, iterations); machine->Put(8, seed); machine->Put(24, other); }
            const u64 final_a = iterations & 1 ? seed : other, final_b = iterations & 1 ? other : seed, result = final_a + 7;
            expected.Put(32, final_a); expected.Put(128, result); expected.Put(136, final_a * 3 + final_b * 5 + 7);
            expected.Put(144, final_b / 3); expected.Put(152, final_b % 3); expected.Put(160, final_a); expected.Put(168, final_b); expected.Put(176, 0);
            const u64 packed = (u64(result >> 63) << 31) | (u64(result == 0) << 30) | (u64(final_a > UINT64_MAX - 7) << 29) |
                (u64(!(final_a >> 63) && bool(result >> 63)) << 28);
            const u64 expected_flags = (initial_flags & ~(u64{15} << 28)) | packed;
            Run(baseline, base); Run(*prepared, actual); Run(fresh, independent);
            REQUIRE(std::memcmp(base.bytes.data(), actual.bytes.data(), actual.bytes.size()) == 0);
            REQUIRE(std::memcmp(independent.bytes.data(), actual.bytes.data(), actual.bytes.size()) == 0);
            REQUIRE(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, extent) == 0); REQUIRE(actual.state->host_cpu_flags == expected_flags);
        }
    }
}

TEST_CASE("function preparation handles empty candidates disabled allocation and rejected forwarding with fresh fallback", "[x64][cfg][prepare][fallback]") {
    for (bool flags : {false, true}) for (u32 route : {u32{0}, u32{1}, u32{2}}) {
        const bool cfg = route != 1; auto plan = Plan(); ir::Assembler as{plan.body.get()};
        Store(as, 128, Load(as, 8)); Carry(as); plan.body->SetTerminal(ir::terminal::ReturnToHost{}); plan.body->ReIdInstr();
        auto config = Configuration();
        // This exceeds the forwarding fact-storage cap before any rewrite;
        // the emitter only accesses the small, explicit fixture uniforms.
        if (route == 2) config.uniform_buffer_size = (1u << 20) + 1;
        const auto order = Order(plan); std::vector<Snapshot> original;
        for (auto* inst : order) if (inst->GetOp() == O::LoadUniform || inst->GetOp() == O::StoreUniform) original.emplace_back(inst);
        x64::Translator baseline{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, cfg, flags}; baseline.Translate();
        auto prepared = x64::Translator::PrepareFunction(plan, {}, &config, true, cfg, flags,
            x64::FunctionPreparationOptions{.uniform_forwarding = true, .capture_forward_changes = true}); prepared->Translate();
        REQUIRE_FALSE(prepared->PreparationStats().reused_final_allocation);
        REQUIRE(prepared->PreparationStats().full_allocation_builds == (cfg ? 1 : 0));
        REQUIRE(prepared->UniformForwardStats().accepted == (route != 2)); REQUIRE(prepared->UniformForwardStats().loads_forwarded == 0);
        REQUIRE(prepared->ForwardChanges().empty()); REQUIRE(Order(plan) == order); for (const auto& snapshot : original) snapshot.Unchanged();
        REQUIRE(prepared->CfgPlan().Enabled() == cfg); REQUIRE(prepared->FramePlan().Enabled() == cfg);
        if (cfg) { FreshEquivalent(*prepared, plan, flags); EquivalentCode(*prepared, plan, baseline, plan); }
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            CAPTURE(flags, route, seed); Machine base, actual, expected;
            for (auto* machine : {&base, &actual, &expected}) machine->Put(8, seed); expected.Put(128, seed);
            Run(baseline, base); Run(*prepared, actual); Same(base, actual, expected);
        }
    }
}
