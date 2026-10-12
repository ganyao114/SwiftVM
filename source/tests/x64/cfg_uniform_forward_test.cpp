#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/cfg_uniform_forward.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
using O = ir::OpCode;
namespace {
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
backend::FunctionPlan Plan() {
    backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); return plan;
}
auto Graph(backend::FunctionPlan& plan) {
    return x64::CfgRegisterPlan::Build(plan.body.get(), plan, {.physical_registers = {}});
}
auto Run(backend::FunctionPlan& plan, Storage& storage) {
    plan.body->ReIdInstr();
    x64::Translator translator{plan.body.get(), {}, nullptr, &plan}; translator.Translate();
    return translator.getCode<HaltReason (*)(backend::State*)>()(storage.state);
}
std::vector<ir::Inst*> Stores(backend::FunctionPlan& plan) {
    std::vector<ir::Inst*> result;
    for (auto& inst : plan.body->GetInstList()) if (inst.GetOp() == O::StoreUniform) result.push_back(&inst);
    return result;
}
} // namespace

TEST_CASE("x64 CFG uniform forwarding meets identical facts through a flags diamond", "[x64][cfg][uniform]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()};
    auto input = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, input);
    auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
    auto skip = as.Goto(ir::BOOL{condition});
    as.SetCarry(as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8));
    as.BindLabel(skip);
    auto* label = &plan.body->GetInstList().back();
    // Guest-edge polling is an exit-only observer. It cannot change a value
    // on this frame's continuing path, and every store remains committed.
    plan.locations[label] = ir::Location{0x1004};
    auto loaded = as.LoadUniform(ir::Uniform{16, T::U32}).SetType(T::U32);
    as.StoreUniform(ir::Uniform{64, T::U32}, loaded);
    plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    const auto before = Stores(plan); const auto graph = Graph(plan); REQUIRE(graph.Enabled());
    const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph);
    REQUIRE(stats.accepted); REQUIRE(stats.loads_forwarded == 1);
    REQUIRE(loaded.Def()->GetOp() == O::BitExtract); REQUIRE(Stores(plan) == before);
    REQUIRE(Graph(plan).Enabled());
    for (u64 condition_value : {0, 1}) for (u64 value : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}}) {
        Storage storage; storage.Put(0, condition_value); storage.Put(8, value);
        REQUIRE(Run(plan, storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(16) == value); REQUIRE(storage.Get(64) == u32(value));
        REQUIRE(storage.state->host_cpu_flags == (condition_value ? 0 : u64{1} << 29));
    }
    Storage interrupted; interrupted.Put(8, 19); interrupted.state->exit_request = u64{1} << 63;
    REQUIRE(Run(plan, interrupted) == HaltReason::Signal);
    // Entry polling may stop before any guest operation, so no output commits.
    REQUIRE(interrupted.Get(16) == 0); REQUIRE(interrupted.Get(64) == 0);
}

TEST_CASE("x64 CFG uniform facts retain byte overlap and disagreeing predecessor loads", "[x64][cfg][uniform]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()};
    auto value = as.LoadImm(ir::Imm{u64{0x8877665544332211}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, value);
    auto skip = as.Goto(ir::BOOL{as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8)});
    as.StoreUniform(ir::Uniform{20, T::U32}, as.LoadImm(ir::Imm{u32{0xaabbccdd}}).SetType(T::U32));
    as.BindLabel(skip);
    auto wide = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    auto low = as.LoadUniform(ir::Uniform{16, T::U32}).SetType(T::U32);
    as.StoreUniform(ir::Uniform{64, T::U64}, wide); as.StoreUniform(ir::Uniform{72, T::U32}, low);
    plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    auto graph = Graph(plan); REQUIRE(graph.Enabled());
    const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph);
    REQUIRE(stats.accepted); REQUIRE(wide.Def()->GetOp() == O::LoadUniform);
    // The low bytes retain their entry fact; the wide load snapshots only
    // unknown high bytes. The later low read can still reuse the entry value.
    REQUIRE(low.Def()->GetOp() == O::BitExtract);
    for (u64 control : {0, 1}) {
        Storage storage; storage.Put(0, control); REQUIRE(Run(plan, storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == (control ? 0x8877665544332211ULL : 0xaabbccdd44332211ULL));
        REQUIRE(storage.Get(72) == 0x44332211ULL);
    }
}

TEST_CASE("x64 CFG uniform forwarding retains identities across a redundant branch load", "[x64][cfg][uniform]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()};
    auto entry = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto skip = as.Goto(ir::BOOL{as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8)});
    auto branch = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::U64}, branch);
    as.BindLabel(skip);
    auto joined = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{72, T::U64}, joined);
    plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    const auto stores = Stores(plan);
    const auto graph = Graph(plan);
    REQUIRE(graph.Enabled());
    const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph);
    REQUIRE(stats.accepted); REQUIRE(stats.loads_forwarded == 2);
    REQUIRE(branch.Def()->GetOp() == O::BitExtract); REQUIRE(joined.Def()->GetOp() == O::BitExtract);
    REQUIRE(branch.Def()->GetArg<ir::Value>(0).Def() == entry.Def());
    REQUIRE(joined.Def()->GetArg<ir::Value>(0).Def() == entry.Def());
    REQUIRE(Stores(plan) == stores); REQUIRE(Graph(plan).Enabled());
    for (u64 control : {0, 1}) for (u64 value : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}}) {
        Storage storage; storage.Put(0, control); storage.Put(8, value);
        REQUIRE(Run(plan, storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == (control ? 0 : value)); REQUIRE(storage.Get(72) == value);
    }
}

TEST_CASE("x64 CFG uniform reads fill unknown bytes without replacing partial scalar facts", "[x64][cfg][uniform][vector]") {
    for (auto wide_type : {T::U64, T::V128}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto low_entry = as.LoadUniform(ir::Uniform{8, T::U32}).SetType(T::U32);
        auto skip = as.Goto(ir::BOOL{as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8)});
        auto branch_snapshot = as.LoadUniform(ir::Uniform{8, wide_type}).SetType(wide_type);
        as.StoreUniform(ir::Uniform{64, wide_type}, branch_snapshot);
        as.BindLabel(skip);
        auto low_joined = as.LoadUniform(ir::Uniform{8, T::U32}).SetType(T::U32);
        auto whole_joined = as.LoadUniform(ir::Uniform{8, wide_type}).SetType(wide_type);
        as.StoreUniform(ir::Uniform{96, T::U32}, low_joined);
        as.StoreUniform(ir::Uniform{128, wide_type}, whole_joined);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto stores = Stores(plan);
        const auto graph = Graph(plan);
        REQUIRE(graph.Enabled());
        const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph);
        REQUIRE(stats.accepted); REQUIRE(stats.loads_forwarded == 1);
        REQUIRE(low_joined.Def()->GetOp() == O::BitExtract);
        REQUIRE(low_joined.Def()->GetArg<ir::Value>(0).Def() == low_entry.Def());
        // No full-width source exists on every path, and combining producer
        // containers would not preserve the raw/narrow-vector contract.
        REQUIRE(branch_snapshot.Def()->GetOp() == O::LoadUniform);
        REQUIRE(whole_joined.Def()->GetOp() == O::LoadUniform);
        REQUIRE(Stores(plan) == stores); REQUIRE(Graph(plan).Enabled());
        for (u64 control : {0, 1}) {
            Storage storage; storage.Put(0, control);
            storage.Put(8, 0x8877665544332211ULL); storage.Put(16, 0xffeeddccbbaa0099ULL);
            REQUIRE(Run(plan, storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(96) == 0x44332211ULL); REQUIRE(storage.Get(128) == 0x8877665544332211ULL);
            REQUIRE(storage.Get(64) == (control ? 0 : 0x8877665544332211ULL));
            if (wide_type == T::V128) {
                REQUIRE(storage.Get(136) == 0xffeeddccbbaa0099ULL);
                REQUIRE(storage.Get(72) == (control ? 0 : 0xffeeddccbbaa0099ULL));
            }
        }
    }
}

TEST_CASE("x64 CFG uniform forwarding does not bootstrap a loop carried state value", "[x64][cfg][uniform][loop]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()};
    auto entry = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
    as.BindLabel(entry); auto* label = &plan.body->GetInstList().back();
    plan.stops.insert(entry.Def());
    auto counter = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto next = as.Sub(counter, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, next);
    auto again = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto back = as.Goto(as.TestNotZero(again).SetType(T::U8)); plan.targets[back.Def()] = label;
    plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    const auto graph = Graph(plan); REQUIRE(graph.Enabled());
    const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph);
    REQUIRE(stats.accepted); REQUIRE(counter.Def()->GetOp() == O::LoadUniform);
    REQUIRE(again.Def()->GetOp() == O::BitExtract); REQUIRE(Graph(plan).Enabled());
    for (u64 initial : {1, 2, 17}) {
        Storage storage; storage.Put(8, initial); REQUIRE(Run(plan, storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(8) == 0);
    }
}

TEST_CASE("x64 CFG uniform forwarding kills facts at opaque memory and escape boundaries", "[x64][cfg][uniform]") {
    for (auto boundary : {O::UniformBarrier, O::GetUniformAddress, O::MemoryBarrierTSO, O::Cpuid}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto original = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, original);
        if (boundary == O::GetUniformAddress)
            as.GetUniformAddress(ir::Imm{u32{16}}).SetType(T::U64);
        else if (boundary == O::Cpuid) {
            // A valid opaque call has no special uniform effects inventory.
            as.Cpuid(original, original, ir::Imm{u32{0}}, ir::Imm{u64{1}}).SetType(T::U64);
        } else as.AppendInst(boundary);
        auto loaded = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{64, T::U64}, loaded);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto graph = Graph(plan); REQUIRE(graph.Enabled());
        const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph);
        REQUIRE(stats.accepted); REQUIRE(stats.barriers == 1);
        REQUIRE(loaded.Def()->GetOp() == O::LoadUniform);
    }
}

TEST_CASE("x64 CFG uniform forwarding refuses a graph for another body", "[x64][cfg][uniform]") {
    auto first = Plan(), other = Plan();
    for (auto* plan : {&first, &other}) {
        ir::Assembler as{plan->body.get()};
        auto value = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, value);
        plan->body->SetTerminal(ir::terminal::ReturnToHost{});
    }
    const auto graph = Graph(first); REQUIRE(graph.Enabled());
    REQUIRE_FALSE(x64::ForwardCfgUniformLoads(other, 4096, graph).accepted);
}

TEST_CASE("CFG forwarding allocation feedback restores scalar loads and retains vector rules", "[x64][cfg][uniform][allocation-policy]") {
    for (bool policy : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto scalar = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{24, T::U64}, scalar);
        auto target = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{64, T::U64}, target);
        auto vector = as.LoadUniform(ir::Uniform{128, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{160, T::V128}, vector);
        auto vector_target = as.LoadUniform(ir::Uniform{160, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{192, T::V128}, vector_target);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{}); plan.body->ReIdInstr();
        const auto target_id = target.Def()->Id(), vector_id = vector_target.Def()->Id();
        const auto stores = Stores(plan); const auto graph = Graph(plan); REQUIRE(graph.Enabled());
        const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph,
            {.require_scalar_source_register = policy, .allocation_budget = {.physical_registers = {}}});
        REQUIRE(stats.accepted); REQUIRE(stats.candidate_loads == 2); REQUIRE(stats.scalar_candidates == 1);
        REQUIRE(stats.rejected_scalar_sources == (policy ? 1 : 0));
        REQUIRE(stats.allocation_policy_enabled == policy); REQUIRE(stats.final_scalar_sources_allocated == policy);
        REQUIRE(stats.scalar_loads == (policy ? 0 : 1)); REQUIRE(stats.vector_loads == 1);
        REQUIRE_FALSE(stats.allocation_limit_reached); REQUIRE(stats.allocation_rounds == (policy ? 2 : 0));
        REQUIRE(target.Def()->GetOp() == (policy ? O::LoadUniform : O::BitExtract));
        REQUIRE(vector_target.Def()->GetOp() == O::BitCast);
        REQUIRE(target.Def()->ReturnType() == T::U64); REQUIRE(target.Def()->Id() == target_id);
        REQUIRE(vector_target.Def()->Id() == vector_id); REQUIRE(Stores(plan) == stores); REQUIRE(Graph(plan).Enabled());
        if (policy) {
            REQUIRE(target.Def()->GetArg<ir::Uniform>(0).GetOffset() == 24);
            for (u32 slot = 1; slot < ir::Inst::max_args; ++slot) REQUIRE(target.Def()->ArgAt(slot).IsVoid());
        }
        Storage storage; storage.Put(8, 0x8877665544332211ULL); storage.Put(128, UINT64_MAX); storage.Put(136, 0x123456789abcdef0ULL);
        REQUIRE(Run(plan, storage) == HaltReason::CallHost); REQUIRE(storage.Get(64) == 0x8877665544332211ULL);
        REQUIRE(storage.Get(192) == UINT64_MAX); REQUIRE(storage.Get(200) == 0x123456789abcdef0ULL);
    }
}

TEST_CASE("CFG forwarding allocation failure restores every plain candidate without losing extra arguments", "[x64][cfg][uniform][allocation-policy]") {
    for (bool policy : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto source = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, source);
        auto decorated = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        decorated.Def()->SetArg(4, source); const auto extra = decorated.Def()->ArgAt(4).Get<ir::Value>();
        auto ordinary = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{64, T::U64}, ordinary);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{}); plan.body->ReIdInstr();
        const auto decorated_id = decorated.Def()->Id(), ordinary_id = ordinary.Def()->Id();
        const auto graph = Graph(plan); REQUIRE(graph.Enabled()); const auto stores = Stores(plan);
        const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph,
            {.require_scalar_source_register = policy, .allocation_budget = {.physical_registers = {3, 3}}});
        REQUIRE(stats.accepted == !policy); REQUIRE(stats.candidate_loads == 1); REQUIRE(stats.scalar_candidates == 1);
        REQUIRE(decorated.Def()->GetOp() == O::LoadUniform); REQUIRE(decorated.Def()->Id() == decorated_id);
        REQUIRE(decorated.Def()->ArgAt(4).Get<ir::Value>() == extra); REQUIRE(decorated.Def()->GetArg<ir::Uniform>(0).GetOffset() == 16);
        REQUIRE(ordinary.Def()->Id() == ordinary_id); REQUIRE(ordinary.Def()->ReturnType() == T::U64);
        REQUIRE(ordinary.Def()->GetOp() == (policy ? O::LoadUniform : O::BitExtract));
        if (policy) {
            REQUIRE(ordinary.Def()->GetArg<ir::Uniform>(0).GetOffset() == 16);
            REQUIRE(stats.loads_forwarded == 0); REQUIRE_FALSE(stats.final_scalar_sources_allocated);
            for (u32 slot = 1; slot < ir::Inst::max_args; ++slot) REQUIRE(ordinary.Def()->ArgAt(slot).IsVoid());
        }
        REQUIRE(Stores(plan) == stores); REQUIRE(Graph(plan).Enabled());
    }
}

TEST_CASE("CFG forwarding and emitter share the actual packed flags allocation budget", "[x64][cfg][uniform][allocation-policy]") {
    for (bool requested : {false, true}) for (bool real_flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto value = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, value);
        auto target = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{64, T::U64}, target);
        if (real_flags) as.SetCarry(as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8));
        else as.SaveFlags(value, ir::Flags::None);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto budget = x64::Translator::AllocationBudget(plan.body.get(), &plan, requested);
        REQUIRE(budget.physical_registers == (requested && real_flags ? std::vector<unsigned>{5, 15} : std::vector<unsigned>{3, 5, 15}));
        const auto graph = Graph(plan); REQUIRE(graph.Enabled());
        const auto stats = x64::ForwardCfgUniformLoads(plan, 4096, graph,
            {.require_scalar_source_register = true, .allocation_budget = budget});
        REQUIRE(stats.accepted); REQUIRE(stats.scalar_loads == 1); REQUIRE(stats.rejected_scalar_sources == 0);
        REQUIRE(stats.final_scalar_sources_allocated); REQUIRE(stats.allocation_rounds == 1);
        x64::Translator code{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, requested}; code.Translate();
        REQUIRE(code.CfgPlan().PhysicalRegisters() == budget.physical_registers); REQUIRE(code.CfgPlan().Register(value.Def()));
        Storage storage; storage.Put(8, 0x8877665544332211ULL);
        REQUIRE(code.getCode<HaltReason (*)(backend::State*)>()(storage.state) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == 0x8877665544332211ULL);
    }
}
