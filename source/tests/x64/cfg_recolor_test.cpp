#include <algorithm>
#include <array>
#include <unordered_set>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/cfg_register_plan.h"
#include "runtime/backend/x64/sparse_frame_plan.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    std::unordered_set<ir::Inst*> flags;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Inst* Label() {
        auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label;
    }
    ir::Value Load(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    ir::Value Add(ir::Value value, u64 imm) { return as.Add(value, ir::Operand{ir::Imm{imm}}).SetType(T::U64); }
    ir::Value Jump() { return as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); }
    ir::Value Branch(ir::Value condition) { return as.Goto(ir::BOOL{condition}); }
    void Target(ir::Value branch, ir::Inst* label, bool stop = false) {
        function.targets[branch.Def()] = label; if (stop) function.stops.insert(branch.Def());
    }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
    void Save(ir::Value value, ir::Flags requested = ir::Flags::All) { as.SaveFlags(value, requested); flags.insert(value.Def()); }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    x64::CfgRegisterPlan Plan(std::vector<unsigned> regs, bool weighted, bool unary, u64 storage) {
        return x64::CfgRegisterPlan::Build(function.body.get(), function,
            {.physical_registers = std::move(regs), .interference_storage_budget_bytes = storage,
             .loop_weighted_costs = weighted, .scalar_unary_phases = unary});
    }
};
void Inputs(ir::Value phi, ir::Value first, ir::Value second) {
    ir::Params inputs; inputs.Push(first); inputs.Push(second); phi.Def()->SetArg(0, inputs);
}
void Same(const x64::CfgRegisterPlan& actual, const x64::CfgRegisterPlan& expected) {
    REQUIRE(actual.Enabled() == expected.Enabled()); REQUIRE(actual.Rejection() == expected.Rejection());
    REQUIRE(actual.HasLiveness() == expected.HasLiveness()); REQUIRE(actual.HasInterference() == expected.HasInterference());
    REQUIRE(actual.Validate() == expected.Validate()); REQUIRE(actual.Body() == expected.Body());
    REQUIRE(actual.PhysicalRegisters() == expected.PhysicalRegisters()); REQUIRE(actual.Instructions() == expected.Instructions());
    REQUIRE(actual.LoopWeightedCostsEnabled() == expected.LoopWeightedCostsEnabled());
    REQUIRE(actual.ScalarUnaryPhasesEnabled() == expected.ScalarUnaryPhasesEnabled());
    REQUIRE(actual.Blocks().size() == expected.Blocks().size()); REQUIRE(actual.Edges().size() == expected.Edges().size());
    REQUIRE(actual.Allocations().size() == expected.Allocations().size());
    const auto& x = actual.ConstructionStats(); const auto& y = expected.ConstructionStats();
    REQUIRE(x.dense_used == y.dense_used); REQUIRE(x.sparse_fallback == y.sparse_fallback);
    REQUIRE(x.dense_bytes == y.dense_bytes); REQUIRE(x.clique_calls == y.clique_calls);
    REQUIRE(x.natural_loops == y.natural_loops); REQUIRE(x.max_loop_depth == y.max_loop_depth);
    for (u32 i = 0; i < actual.Blocks().size(); ++i) {
        const auto& a = actual.Blocks()[i]; const auto& b = expected.Blocks()[i];
        REQUIRE(a.begin == b.begin); REQUIRE(a.end == b.end); REQUIRE(a.reachable == b.reachable);
        REQUIRE(a.incoming_edges == b.incoming_edges); REQUIRE(a.outgoing_edges == b.outgoing_edges);
        REQUIRE(a.live_in == b.live_in); REQUIRE(a.live_out == b.live_out); REQUIRE(a.loop_depth == b.loop_depth); REQUIRE(a.loop_weight == b.loop_weight);
        for (u32 j = 0; j < actual.Blocks().size(); ++j) REQUIRE(actual.DominatesBlock(i, j) == expected.DominatesBlock(i, j));
    }
    for (u32 i = 0; i < actual.Edges().size(); ++i) {
        const auto& a = actual.Edges()[i]; const auto& b = expected.Edges()[i];
        REQUIRE(a.from == b.from); REQUIRE(a.to == b.to); REQUIRE(a.source == b.source);
        REQUIRE(a.taken == b.taken); REQUIRE(a.host_entry == b.host_entry); REQUIRE(a.live == b.live);
        REQUIRE(a.copies.size() == b.copies.size());
        for (u32 j = 0; j < a.copies.size(); ++j) {
            REQUIRE(a.copies[j].destination == b.copies[j].destination);
            const auto& left = a.copies[j].source; const auto& right = b.copies[j].source;
            REQUIRE(left.type == right.type);
            if (left.IsValue()) { REQUIRE(left.value.Def() == right.value.Def()); REQUIRE(left.value.Type() == right.value.Type()); }
            else if (left.IsImm()) { REQUIRE(left.imm.Get() == right.imm.Get()); REQUIRE(left.imm.GetType() == right.imm.GetType()); }
        }
    }
    for (u32 i = 0; i < actual.Allocations().size(); ++i) {
        const auto& a = actual.Allocations()[i]; const auto& b = expected.Allocations()[i];
        REQUIRE(a.producer == b.producer); REQUIRE(a.slot == b.slot); REQUIRE(a.uses == b.uses);
        REQUIRE(a.requires_canonical_home == b.requires_canonical_home); REQUIRE(a.requires_full_container == b.requires_full_container);
        REQUIRE(a.scalar_reads == b.scalar_reads); REQUIRE(a.estimated_benefit == b.estimated_benefit);
        REQUIRE(a.weighted_reads == b.weighted_reads); REQUIRE(a.weighted_benefit == b.weighted_benefit);
    }
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
void SameFrame(Program& p, const x64::CfgRegisterPlan& actual, const x64::CfgRegisterPlan& expected) {
    for (bool rematerialize : {false, true}) {
        const auto a = x64::SparseFramePlan::Build(p.function.body.get(), &p.function, actual, p.flags, rematerialize);
        const auto b = x64::SparseFramePlan::Build(p.function.body.get(), &p.function, expected, p.flags, rematerialize);
        REQUIRE(a.Enabled()); REQUIRE(b.Enabled());
        REQUIRE(a.Validate(p.function.body.get(), &p.function, actual, p.flags, rematerialize));
        REQUIRE(b.Validate(p.function.body.get(), &p.function, expected, p.flags, rematerialize));
        const auto& x = a.Statistics(); const auto& y = b.Statistics();
        REQUIRE(x.home_count == y.home_count); REQUIRE(x.retained_homes == y.retained_homes);
        REQUIRE(x.omitted_control == y.omitted_control); REQUIRE(x.omitted_register == y.omitted_register);
        REQUIRE(x.omitted_unused == y.omitted_unused); REQUIRE(x.home_bytes == y.home_bytes);
        REQUIRE(x.reused_scalar_homes == y.reused_scalar_homes); REQUIRE(x.shared_scalar_home_slots == y.shared_scalar_home_slots);
        REQUIRE(x.omitted_rematerialized == y.omitted_rematerialized);
        for (auto* inst : actual.Instructions()) {
            REQUIRE(a.Home(inst) == b.Home(inst)); REQUIRE(a.Decision(inst) == b.Decision(inst));
            REQUIRE(a.RematerializedValue(inst) == b.RematerializedValue(inst));
        }
    }
}
void Exercise(Program& p) {
    const std::array<std::vector<unsigned>, 8> budgets{{{}, {15}, {3}, {5, 15}, {15, 5}, {3, 5, 15}, {15, 3, 5}, {}}};
    const auto text = p.function.body->ToString();
    for (bool weighted : {false, true}) for (bool unary : {false, true}) for (u64 storage : {u64{0}, u64{8 * 1024 * 1024}}) {
        const auto source = p.Plan({3, 5, 15}, weighted, unary, storage), snapshot = source;
        REQUIRE(source.Validate()); auto chained = source;
        for (const auto& budget : budgets) {
            const auto fresh = p.Plan(budget, weighted, unary, storage);
            const auto recolored = source.Recolor(budget); chained = chained.Recolor(budget);
            Same(recolored, fresh); Same(chained, fresh); SameFrame(p, recolored, fresh);
            Same(source, snapshot); REQUIRE(p.function.body->ToString() == text);
        }
        Same(chained.Recolor({3, 5, 15}), source);
    }
}
} // namespace

TEST_CASE("CFG recoloring matches fresh construction for pressure binary unary and raw containers", "[x64][cfg-recolor][equivalence]") {
    Program p; std::array<ir::Value, 4> invariant{p.Load(0), p.Load(8), p.Load(16), p.Load(24)};
    auto raw = p.as.BitCast(ir::Value{invariant[0].Def()}.SetCastType(T::V128)).SetType(T::V128); p.Store(256, raw);
    auto first = p.Add(invariant[1], 7), neg = p.as.Neg(first).SetType(T::U64);
    auto rotate = p.as.RorImm(neg, ir::Imm{u32{13}}).SetType(T::U64);
    auto next = p.as.BitExtract(rotate, ir::Imm{u32{7}}, ir::Imm{u32{32}}).SetType(T::U64);
    p.Save(neg); p.Store(128, next);
    for (u32 repeat = 0; repeat < 3; ++repeat) for (u32 i = 0; i < invariant.size(); ++i) p.Store(512 + i * 8, invariant[i]);
    p.Finish(); Exercise(p);
}

TEST_CASE("CFG recoloring matches complete loop Phi helper flags and unreachable topology", "[x64][cfg-recolor][loop][phi]") {
    Program p; auto seed = p.Load(0), invariant = p.Load(8), trips = p.Load(16), entry = p.Jump(); auto* header = p.Label();
    auto phi = p.as.AddPhi(ir::Params{}).SetType(T::U64), count = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    auto neg = p.as.Neg(phi).SetType(T::U64), next = p.Add(neg, 17); p.Save(next);
    auto returned = p.as.CallLambda(ir::Lambda{ir::Imm{u64{0x12345678}}}, next, invariant, next).SetType(T::U64); p.Store(128, returned);
    auto remaining = p.as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    auto back = p.Branch(p.as.TestNotZero(remaining).SetType(T::U8)); p.Target(entry, header, true); p.Target(back, header);
    Inputs(phi, seed, next); Inputs(count, trips, remaining); p.Store(136, next); p.Store(144, invariant);
    auto skip = p.Jump(); auto ignored = p.Load(24); p.Store(152, ignored); auto* exit = p.Label(); p.Target(skip, exit, true); p.Finish();
    Exercise(p);
}

TEST_CASE("CFG recoloring preserves duplicate Phi edge occurrence order and terminal selectors", "[x64][cfg-recolor][edges]") {
    Program p; auto a = p.Load(0), b = p.Load(8), condition = p.Load(16); auto branch = p.Branch(condition); auto* join = p.Label();
    auto phi = p.as.AddPhi(ir::Params{}).SetType(T::U64); Inputs(phi, a, b); p.Target(branch, join);
    auto next = p.as.Neg(phi).SetType(T::U64); p.Store(128, next);
    p.function.body->SetTerminal(ir::terminal::If{ir::BOOL{phi}, ir::terminal::ReturnToHost{},
        ir::terminal::Switch{condition, {{ir::Imm{u64{0}}, ir::terminal::ReturnToHost{}}}}}); Exercise(p);
}

TEST_CASE("CFG recoloring restores base home obligations after zero-register spills", "[x64][cfg-recolor][homes]") {
    Program p; auto source = p.Load(0), next = p.Add(source, 7); p.Store(128, next); p.Store(136, next); p.Finish();
    auto full = p.Plan({15}, true, true, 8 * 1024 * 1024); REQUIRE(full.Validate());
    REQUIRE(full.Register(source.Def()) == 15); REQUIRE(full.Register(next.Def()) == 15);
    REQUIRE_FALSE(full.RequiresCanonicalHome(source.Def())); REQUIRE_FALSE(full.RequiresCanonicalHome(next.Def()));
    const auto empty = full.Recolor({}); REQUIRE(empty.Validate());
    REQUIRE_FALSE(empty.Register(source.Def())); REQUIRE(empty.RequiresCanonicalHome(source.Def())); REQUIRE(empty.RequiresCanonicalHome(next.Def()));
    const auto restored = empty.Recolor({15}); Same(restored, full); SameFrame(p, restored, full);
    REQUIRE_FALSE(restored.RequiresCanonicalHome(source.Def())); REQUIRE_FALSE(restored.RequiresCanonicalHome(next.Def()));
    // Raw container obligations remain true even when the source gets a slot.
    Program raw; auto value = raw.Load(0); auto vector = raw.as.BitCast(ir::Value{value.Def()}.SetCastType(T::V128)).SetType(T::V128);
    raw.Store(256, vector); raw.Store(128, value); raw.Store(136, value); raw.Finish();
    const auto assigned = raw.Plan({15}, false, true, 0); REQUIRE(assigned.Register(value.Def()) == 15);
    REQUIRE(assigned.RequiresCanonicalHome(value.Def())); REQUIRE(assigned.RequiresFullContainer(value.Def()));
    Same(assigned.Recolor({}).Recolor({15}), assigned); SameFrame(raw, assigned.Recolor({}).Recolor({15}), assigned);
}

TEST_CASE("CFG recoloring rejects illegal budgets and allocation authority boundaries", "[x64][cfg-recolor][validation]") {
    Program p; auto value = p.Load(0); p.Store(128, value); p.Finish(); auto source = p.Plan({3, 5, 15}, true, true, 0), snapshot = source;
    for (const auto& budget : std::array<std::vector<unsigned>, 5>{{{3, 3}, {0}, {12}, {3, 5, 15, 3}, {16}}}) {
        const auto rejected = source.Recolor(budget); REQUIRE_FALSE(rejected.Enabled()); REQUIRE_FALSE(rejected.Validate());
        REQUIRE(rejected.Rejection() == x64::CfgPlanRejection::InvalidBudget); Same(source, snapshot);
    }
    const auto graph = x64::CfgRegisterPlan::ValidateGraph(p.function.body.get(), p.function); REQUIRE(graph.Enabled()); REQUIRE_FALSE(graph.Validate());
    const auto recolored_graph = graph.Recolor({15}); REQUIRE_FALSE(recolored_graph.Enabled()); REQUIRE_FALSE(recolored_graph.Validate());
    REQUIRE(recolored_graph.Rejection() == x64::CfgPlanRejection::InvalidSSA);
    REQUIRE_FALSE(recolored_graph.Register(value.Def())); REQUIRE(recolored_graph.RequiresCanonicalHome(value.Def())); REQUIRE(recolored_graph.RequiresFullContainer(value.Def()));
    x64::CfgRegisterPlan missing; const auto recolored_missing = missing.Recolor({15}); REQUIRE_FALSE(recolored_missing.Enabled()); REQUIRE_FALSE(recolored_missing.Validate());
    REQUIRE(recolored_missing.Rejection() == x64::CfgPlanRejection::MissingBody);
    Program broken; broken.Jump(); broken.Finish(); const auto invalid = broken.Plan({15}, false, false, 0);
    REQUIRE_FALSE(invalid.Enabled()); const auto recolored_invalid = invalid.Recolor({3, 5, 15}); REQUIRE_FALSE(recolored_invalid.Enabled()); REQUIRE_FALSE(recolored_invalid.Validate());
    REQUIRE(recolored_invalid.Rejection() == invalid.Rejection());
}

TEST_CASE("CFG recoloring handles empty bodies with every valid physical budget", "[x64][cfg-recolor][empty]") {
    Program p; p.Finish(); Exercise(p);
}
