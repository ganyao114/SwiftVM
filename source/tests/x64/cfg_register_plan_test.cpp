#include <algorithm>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/cfg_register_plan.h"
#include "runtime/backend/x64/flags_usage.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
void CompareConstruction(const x64::CfgRegisterPlan& actual, const x64::CfgRegisterPlan& reference) {
    REQUIRE(actual.Rejection() == reference.Rejection());
    REQUIRE(actual.Enabled() == reference.Enabled()); REQUIRE(actual.Validate() == reference.Validate());
    REQUIRE(actual.HasLiveness() == reference.HasLiveness()); REQUIRE(actual.HasInterference() == reference.HasInterference());
    REQUIRE(actual.Body() == reference.Body()); REQUIRE(actual.Instructions() == reference.Instructions());
    REQUIRE(actual.PhysicalRegisters() == reference.PhysicalRegisters());
    REQUIRE(actual.ConstructionStats().clique_calls == reference.ConstructionStats().clique_calls);
    REQUIRE_FALSE(reference.ConstructionStats().dense_used); REQUIRE(reference.ConstructionStats().dense_bytes == 0);
    if (actual.HasInterference() && !actual.Instructions().empty()) {
        REQUIRE(actual.ConstructionStats().dense_used != actual.ConstructionStats().sparse_fallback);
        REQUIRE(reference.ConstructionStats().sparse_fallback);
    }
    REQUIRE(actual.Allocations().size() == reference.Allocations().size());
    for (size_t i = 0; i < actual.Allocations().size(); ++i) {
        const auto& a = actual.Allocations()[i]; const auto& b = reference.Allocations()[i];
        REQUIRE(a.producer == b.producer); REQUIRE(a.slot == b.slot);
        REQUIRE(a.requires_canonical_home == b.requires_canonical_home);
        REQUIRE(a.requires_full_container == b.requires_full_container);
        REQUIRE(a.uses == b.uses); REQUIRE(a.scalar_reads == b.scalar_reads); REQUIRE(a.estimated_benefit == b.estimated_benefit);
    }
    for (auto* left : actual.Instructions()) {
        REQUIRE(actual.Slot(left) == reference.Slot(left)); REQUIRE(actual.Register(left) == reference.Register(left));
        REQUIRE(actual.RequiresCanonicalHome(left) == reference.RequiresCanonicalHome(left));
        REQUIRE(actual.RequiresFullContainer(left) == reference.RequiresFullContainer(left));
        REQUIRE_FALSE(actual.Interferes(left, left));
        for (auto* right : actual.Instructions()) {
            REQUIRE(actual.Interferes(left, right) == reference.Interferes(left, right));
            REQUIRE(actual.CanReuseBinaryInput(left, right) == reference.CanReuseBinaryInput(left, right));
        }
    }
    REQUIRE(actual.Blocks().size() == reference.Blocks().size());
    for (size_t i = 0; i < actual.Blocks().size(); ++i) {
        const auto& a = actual.Blocks()[i]; const auto& b = reference.Blocks()[i];
        REQUIRE(a.begin == b.begin); REQUIRE(a.end == b.end); REQUIRE(a.reachable == b.reachable);
        REQUIRE(a.incoming_edges == b.incoming_edges); REQUIRE(a.outgoing_edges == b.outgoing_edges);
        REQUIRE(a.live_in == b.live_in); REQUIRE(a.live_out == b.live_out);
        for (u32 j = 0; j < actual.Blocks().size(); ++j)
            REQUIRE(actual.DominatesBlock(i, j) == reference.DominatesBlock(i, j));
    }
    REQUIRE(actual.Edges().size() == reference.Edges().size());
    for (size_t i = 0; i < actual.Edges().size(); ++i) {
        const auto& a = actual.Edges()[i]; const auto& b = reference.Edges()[i];
        REQUIRE(a.from == b.from); REQUIRE(a.to == b.to); REQUIRE(a.source == b.source);
        REQUIRE(a.taken == b.taken); REQUIRE(a.host_entry == b.host_entry); REQUIRE(a.live == b.live);
        REQUIRE(a.copies.size() == b.copies.size());
        for (size_t j = 0; j < a.copies.size(); ++j) {
            REQUIRE(a.copies[j].destination == b.copies[j].destination);
            const auto& x = a.copies[j].source; const auto& y = b.copies[j].source;
            REQUIRE(x.type == y.type);
            if (x.IsValue()) { REQUIRE(x.value.Def() == y.value.Def()); REQUIRE(x.value.Type() == y.value.Type()); }
            else if (x.IsImm()) { REQUIRE(x.imm.Get() == y.imm.Get()); REQUIRE(x.imm.GetType() == y.imm.GetType()); }
        }
    }
}
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Inst* Label() {
        auto* label = new ir::Inst(O::BindLabel);
        label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label;
    }
    ir::Value Imm(u64 value) { return as.LoadImm(ir::Imm{value}).SetType(T::U64); }
    ir::Value Load(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    ir::Value Add(ir::Value a, u64 b) { return as.Add(a, ir::Operand{ir::Imm{b}}).SetType(T::U64); }
    ir::Value Branch(ir::Value condition) { return as.Goto(ir::BOOL{condition}); }
    ir::Value Jump() { return Branch(as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)); }
    void Target(ir::Value branch, ir::Inst* label, bool stop = false) {
        function.targets[branch.Def()] = label;
        if (stop) function.stops.insert(branch.Def());
    }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    x64::CfgRegisterPlan Plan(std::vector<unsigned> regs = {3, 5, 15}) {
        x64::CfgRegisterOptions options{.physical_registers = std::move(regs)};
        auto plan = x64::CfgRegisterPlan::Build(function.body.get(), function, options);
        options.interference_storage_budget_bytes = 0;
        const auto reference = x64::CfgRegisterPlan::Build(function.body.get(), function, options);
        CompareConstruction(plan, reference);
        // Every existing valid/malformed fixture also checks the standalone
        // graph verifier; invalid register budgets are allocation-only input.
        auto graph = x64::CfgRegisterPlan::ValidateGraph(function.body.get(), function);
        if (plan.Rejection() != x64::CfgPlanRejection::InvalidBudget) REQUIRE(graph.Rejection() == plan.Rejection());
        REQUIRE_FALSE(graph.HasLiveness()); REQUIRE_FALSE(graph.HasInterference()); REQUIRE_FALSE(graph.Validate());
        REQUIRE(graph.Allocations().empty()); REQUIRE(graph.PhysicalRegisters().empty());
        for (const auto& inst : function.body->GetInstList()) REQUIRE(graph.RequiresFullContainer(&inst));
        return plan;
    }
};
bool Contains(const std::vector<ir::Inst*>& list, ir::Inst* value) { return std::find(list.begin(), list.end(), value) != list.end(); }
u64 Helper(u64 a, u64 b, u64 c) { return a + b + c; }
void Check(const x64::CfgRegisterPlan& plan, ir::Block* body) {
    REQUIRE(plan.Enabled()); REQUIRE(plan.Validate()); REQUIRE(plan.MatchesBody(body));
    REQUIRE(plan.HasLiveness()); REQUIRE(plan.HasInterference());
    for (const auto& allocation : plan.Allocations()) {
        REQUIRE(plan.RequiresFullContainer(allocation.producer) == allocation.requires_full_container);
        if (allocation.requires_full_container) REQUIRE(plan.RequiresCanonicalHome(allocation.producer));
        if (!allocation.slot && allocation.uses) REQUIRE(plan.RequiresCanonicalHome(allocation.producer));
        if (!allocation.uses) { REQUIRE_FALSE(allocation.slot); REQUIRE_FALSE(plan.RequiresCanonicalHome(allocation.producer)); }
        for (const auto& other : plan.Allocations()) {
            if (plan.Interferes(allocation.producer, other.producer) && allocation.slot && other.slot)
                REQUIRE(allocation.slot != other.slot);
        }
    }
}
}

TEST_CASE("x64 CFG graph validation supplies topology dominance and Phi inputs without allocation analysis", "[x64][cfg-plan][graph-only]") {
    Program p; auto seed = p.Load(0), entry = p.Jump(); auto* header = p.Label();
    auto phi = p.as.AddPhi(ir::Params{}).SetType(T::U64), next = p.Add(phi, 1);
    auto back = p.Branch(p.Load(8)); p.Store(16, next);
    ir::Params inputs; inputs.Push(seed); inputs.Push(next); phi.Def()->SetArg(0, inputs);
    p.Target(entry, header, true); p.Target(back, header); p.Finish();
    auto graph = x64::CfgRegisterPlan::ValidateGraph(p.function.body.get(), p.function);
    REQUIRE_FALSE(graph.ConstructionStats().dense_used); REQUIRE_FALSE(graph.ConstructionStats().sparse_fallback);
    REQUIRE(graph.ConstructionStats().dense_bytes == 0); REQUIRE(graph.ConstructionStats().clique_calls == 0);
    auto full = p.Plan({}); Check(full, p.function.body.get());
    REQUIRE(graph.Enabled()); REQUIRE(graph.MatchesBody(p.function.body.get()));
    REQUIRE_FALSE(graph.Validate()); REQUIRE_FALSE(graph.HasLiveness()); REQUIRE_FALSE(graph.HasInterference());
    REQUIRE(graph.Instructions() == full.Instructions()); REQUIRE(graph.Blocks().size() == full.Blocks().size());
    REQUIRE(graph.Edges().size() == full.Edges().size());
    REQUIRE(graph.Allocations().empty()); REQUIRE_FALSE(graph.Slot(seed.Def()));
    REQUIRE(graph.RequiresCanonicalHome(seed.Def())); REQUIRE_FALSE(graph.CanReuseBinaryInput(next.Def(), phi.Def()));
    for (u32 i = 0; i < graph.Blocks().size(); ++i) {
        const auto& a = graph.Blocks()[i]; const auto& b = full.Blocks()[i];
        REQUIRE(a.begin == b.begin); REQUIRE(a.end == b.end); REQUIRE(a.reachable == b.reachable);
        REQUIRE(a.incoming_edges == b.incoming_edges); REQUIRE(a.outgoing_edges == b.outgoing_edges);
        REQUIRE(a.live_in.empty()); REQUIRE(a.live_out.empty());
        for (u32 j = 0; j < graph.Blocks().size(); ++j) REQUIRE(graph.DominatesBlock(i, j) == full.DominatesBlock(i, j));
    }
    for (u32 i = 0; i < graph.Edges().size(); ++i) {
        const auto& a = graph.Edges()[i]; const auto& b = full.Edges()[i];
        REQUIRE(a.from == b.from); REQUIRE(a.to == b.to); REQUIRE(a.source == b.source);
        REQUIRE(a.taken == b.taken); REQUIRE(a.host_entry == b.host_entry); REQUIRE(a.live.empty());
        REQUIRE(a.copies.size() == b.copies.size());
        for (u32 j = 0; j < a.copies.size(); ++j) {
            REQUIRE(a.copies[j].destination == b.copies[j].destination);
            REQUIRE(a.copies[j].source.value.Def() == b.copies[j].source.value.Def());
        }
    }
    // Graph-only still validates edge dominance before its early return.
    inputs = {}; inputs.Push(next); inputs.Push(next); phi.Def()->SetArg(0, inputs);
    REQUIRE(x64::CfgRegisterPlan::ValidateGraph(p.function.body.get(), p.function).Rejection() == x64::CfgPlanRejection::InvalidSSA);
}

TEST_CASE("x64 dense interference matches sparse construction across word and budget boundaries", "[x64][cfg-plan][interference]") {
    for (u32 producers : {32u, 33u, 65u, 129u}) {
        Program p; std::vector<ir::Value> values;
        for (u32 i = 0; i < producers; ++i) values.push_back(p.Load(i * 8));
        for (u32 i = 0; i < producers; ++i) p.Store(1024 + i * 8, values[i]);
        p.Finish();
        const auto dense = p.Plan(); Check(dense, p.function.body.get());
        REQUIRE(dense.ConstructionStats().dense_used);
        const u64 count = dense.Instructions().size(), words = count / 64 + (count % 64 != 0);
        const u64 required = (count + 1) * words * sizeof(u64);
        REQUIRE(dense.ConstructionStats().dense_bytes == required);
        for (auto left : values) for (auto right : values)
            REQUIRE(dense.Interferes(left.Def(), right.Def()) == (left.Def() != right.Def()));
        for (u64 budget : {u64{0}, u64{1}, required - 1, required, std::numeric_limits<u64>::max()}) {
            const auto alternative = x64::CfgRegisterPlan::Build(p.function.body.get(), p.function,
                {.interference_storage_budget_bytes = budget});
            const auto reference = x64::CfgRegisterPlan::Build(p.function.body.get(), p.function,
                {.interference_storage_budget_bytes = 0});
            CompareConstruction(alternative, reference);
            REQUIRE(alternative.ConstructionStats().dense_used == (budget >= required));
            REQUIRE(alternative.ConstructionStats().dense_bytes == (budget >= required ? required : 0));
        }
    }
}

TEST_CASE("x64 interference preserves duplicate Phi edges late inputs and nested terminal selectors", "[x64][cfg-plan][interference]") {
    for (bool params : {false, true}) {
        Program p; auto left = p.Load(0), right = p.Load(8), condition = p.Load(16);
        auto branch = p.Branch(condition); auto* join = p.Label();
        ir::Params inputs; inputs.Push(left); inputs.Push(right);
        auto phi = p.as.AddPhi(inputs).SetType(T::U64), output = p.Add(phi, 7);
        if (params) {
            ir::Params extra; extra.Push(left); extra.Push(ir::Value{right.Def()}.SetCastType(T::V128));
            output.Def()->SetArg(4, extra);
        } else output.Def()->SetArg(4, ir::Lambda{left});
        p.Store(24, output); p.Target(branch, join);
        p.function.body->SetTerminal(ir::terminal::CheckHalt{ir::terminal::Condition{ir::Cond::EQ,
            ir::terminal::If{ir::BOOL{phi}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}},
            ir::terminal::ReturnToHost{}}});
        const auto plan = p.Plan({5, 15}); Check(plan, p.function.body.get());
        REQUIRE(plan.Blocks().size() == 2); REQUIRE(plan.Blocks()[1].incoming_edges.size() == 2);
        const auto& fallthrough = plan.Edges()[plan.Blocks()[1].incoming_edges[0]];
        const auto& taken = plan.Edges()[plan.Blocks()[1].incoming_edges[1]];
        REQUIRE(fallthrough.source == branch.Def()); REQUIRE(taken.source == branch.Def());
        REQUIRE_FALSE(fallthrough.taken); REQUIRE(taken.taken);
        REQUIRE(fallthrough.copies[0].source.value.Def() == left.Def());
        REQUIRE(taken.copies[0].source.value.Def() == right.Def());
        REQUIRE(plan.Interferes(output.Def(), left.Def())); REQUIRE(plan.RequiresFullContainer(left.Def()));
        REQUIRE(plan.Interferes(output.Def(), phi.Def())); // The nested terminal reads Phi after publication.
    }
}

TEST_CASE("x64 whole CFG allocation carries Phi and ordinary definitions through critical edges", "[x64][cfg-plan]") {
    Program p;
    auto seed = p.Load(0), condition = p.Load(8);
    auto first = p.Add(seed, 10);
    auto conditional = p.Branch(condition);
    auto other = p.Load(16), alternate = p.Add(other, 20);
    auto jump = p.Jump();
    auto* join = p.Label();
    ir::Params inputs; inputs.Push(first); inputs.Push(alternate);
    auto phi = p.as.AddPhi(inputs).SetType(T::U64);
    auto result = p.as.Add(phi, ir::Operand{seed}).SetType(T::U64);
    p.Store(24, result);
    p.Target(conditional, join); p.Target(jump, join, true); p.Finish();
    auto plan = p.Plan(); Check(plan, p.function.body.get());
    REQUIRE(plan.Blocks().size() == 3);
    REQUIRE(plan.Interferes(phi.Def(), seed.Def())); // Seed remains live after Phi publication.
    REQUIRE(plan.Slot(phi.Def()));
    REQUIRE_FALSE(plan.RequiresCanonicalHome(phi.Def()));
    u32 copies{};
    for (const auto& edge : plan.Edges()) {
        if (!edge.copies.empty()) {
            REQUIRE(edge.copies.size() == 1);
            REQUIRE(edge.copies[0].destination == phi.Def());
            REQUIRE(edge.copies[0].source.IsValue());
            const auto source = edge.copies[0].source.value.Def();
            REQUIRE(source == (edge.source == conditional.Def() ? first.Def() : alternate.Def()));
            REQUIRE(Contains(edge.live, source)); ++copies;
        }
    }
    REQUIRE(copies == 2);
    const auto again = p.Plan();
    for (const auto& allocation : plan.Allocations()) REQUIRE(again.Slot(allocation.producer) == allocation.slot);
}

TEST_CASE("x64 whole CFG allocation models loop-carried Phi swaps as parallel copies", "[x64][cfg-plan][phi]") {
    Program p;
    auto left = p.Imm(11), right = p.Imm(22), enter = p.Jump();
    auto* header = p.Label();
    ir::Params none;
    auto a = p.as.AddPhi(none).SetType(T::U64), b = p.as.AddPhi(none).SetType(T::U64);
    p.Store(0, a); p.Store(8, b);
    auto condition = p.Load(16), back = p.Branch(condition);
    p.Store(24, a); p.Store(32, b);
    p.Target(enter, header, true); p.Target(back, header);
    ir::Params pa, pb; pa.Push(left); pa.Push(b); pb.Push(right); pb.Push(a);
    a.Def()->SetArg(0, pa); b.Def()->SetArg(0, pb); p.Finish();
    auto plan = p.Plan(); Check(plan, p.function.body.get());
    REQUIRE(plan.Interferes(a.Def(), b.Def()));
    REQUIRE(plan.Slot(a.Def())); REQUIRE(plan.Slot(b.Def()));
    const auto found = std::find_if(plan.Edges().begin(), plan.Edges().end(), [&](const auto& e) { return e.source == back.Def() && e.taken; });
    REQUIRE(found != plan.Edges().end()); REQUIRE(found->copies.size() == 2);
    REQUIRE(found->copies[0].destination == a.Def()); REQUIRE(found->copies[0].source.value.Def() == b.Def());
    REQUIRE(found->copies[1].destination == b.Def()); REQUIRE(found->copies[1].source.value.Def() == a.Def());
    REQUIRE(Contains(found->live, a.Def())); REQUIRE(Contains(found->live, b.Def()));
}

TEST_CASE("x64 whole CFG allocation supports lexical forward dominating definitions and terminal inputs", "[x64][cfg-plan]") {
    Program p;
    auto enter = p.Jump();
    auto* use_label = p.Label();
    auto placeholder = p.Imm(0), result = p.Add(placeholder, 5);
    auto leave = p.Jump();
    auto* define_label = p.Label();
    auto forward = p.Load(0), go_use = p.Jump();
    auto* exit_label = p.Label();
    p.Store(8, result);
    auto* marker = p.as.AppendInst(O::Nop);
    p.function.stops.insert(marker);
    p.function.exits.emplace(marker, ir::terminal::If{ir::BOOL{forward}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
    result.Def()->SetArg(0, forward);
    p.Target(enter, define_label, true); p.Target(go_use, use_label, true); p.Target(leave, exit_label, true); p.Finish();
    auto plan = p.Plan(); Check(plan, p.function.body.get());
    REQUIRE(plan.Slot(forward.Def()));
    REQUIRE_FALSE(plan.RequiresCanonicalHome(forward.Def()));
    REQUIRE(plan.Blocks().size() >= 4);
    bool cross{};
    for (const auto& block : plan.Blocks()) cross |= Contains(block.live_in, forward.Def());
    REQUIRE(cross);
}

TEST_CASE("x64 whole CFG allocation handles budgets returning calls raw homes and flat local CFG views", "[x64][cfg-plan]") {
    Program p;
    auto a = p.Load(0), b = p.Load(8), c = p.Load(16), d = p.Load(24);
    auto returned = p.as.CallHost(&Helper, a, b, c).SetType(T::U64);
    p.Store(32, a); p.Store(40, b); p.Store(48, c); p.Store(56, d); p.Store(64, returned);
    auto raw = p.as.BitCast(ir::Value{b.Def()}.SetCastType(T::V128)).SetType(T::V128);
    p.Store(80, raw); p.Finish();
    for (auto regs : {std::vector<unsigned>{}, std::vector<unsigned>{15}, std::vector<unsigned>{5, 15}, std::vector<unsigned>{3, 5, 15}}) {
        auto plan = p.Plan(regs); Check(plan, p.function.body.get());
        REQUIRE(plan.RequiresCanonicalHome(b.Def()));
        REQUIRE(plan.RequiresFullContainer(b.Def()));
        REQUIRE_FALSE(plan.RequiresFullContainer(a.Def()));
        unsigned assigned{};
        for (auto value : {a, b, c, d, returned}) {
            if (auto reg = plan.Register(value.Def())) {
                REQUIRE(std::find(regs.begin(), regs.end(), *reg) != regs.end()); ++assigned;
            }
        }
        REQUIRE(assigned <= regs.size());
        REQUIRE(plan.Interferes(a.Def(), returned.Def()));
    }
    REQUIRE(p.Plan({3, 3}).Rejection() == x64::CfgPlanRejection::InvalidBudget);
    REQUIRE(p.Plan({0}).Rejection() == x64::CfgPlanRejection::InvalidBudget);
    backend::FunctionPlan view; // Flat-body authority needs no new owner.
    auto flat = x64::CfgRegisterPlan::Build(p.function.body.get(), view);
    Check(flat, p.function.body.get());
    REQUIRE_FALSE(flat.MatchesBody(nullptr));
}

TEST_CASE("x64 full container obligations remain separate from scalar spills and allocation benefit", "[x64][cfg-plan][container]") {
    SECTION("empty budget scalar spills retain only their low qword") {
        Program p; auto input = p.Load(0), output = p.Add(input, 1); p.Store(8, output); p.Finish();
        auto plan = p.Plan({}); Check(plan, p.function.body.get());
        for (auto value : {input, output}) {
            REQUIRE_FALSE(plan.Register(value.Def())); REQUIRE(plan.RequiresCanonicalHome(value.Def()));
            REQUIRE_FALSE(plan.RequiresFullContainer(value.Def()));
            const auto allocation = std::find_if(plan.Allocations().begin(), plan.Allocations().end(),
                [&](const auto& entry) { return entry.producer == value.Def(); });
            REQUIRE(allocation != plan.Allocations().end()); REQUIRE(allocation->scalar_reads == 1);
            REQUIRE(allocation->estimated_benefit == 1);
        }
        Program foreign; auto unknown = foreign.Load(0);
        REQUIRE(plan.RequiresFullContainer(unknown.Def())); REQUIRE(plan.RequiresFullContainer(nullptr));
        x64::CfgRegisterPlan empty; REQUIRE(empty.RequiresFullContainer(input.Def()));
        auto invalid = p.Plan({3, 3}); REQUIRE(invalid.RequiresFullContainer(input.Def()));
    }
    SECTION("raw aliases and every opaque physical argument retain full containers") {
        for (u32 decoration = 0; decoration < 5; ++decoration) {
            Program p; auto input = p.Load(0), output = p.Add(input, 1);
            if (decoration == 0) {
                auto raw = p.as.BitCast(ir::Value{input.Def()}.SetCastType(T::V128)).SetType(T::V128);
                p.Store(16, raw);
            } else if (decoration == 1) {
                const auto view = ir::Value{input.Def()}.SetCastType(T::U8);
                auto raw = p.as.GetResult(view, view).SetType(T::U8);
                p.Store(16, raw);
            } else if (decoration == 2) output.Def()->SetArg(4, ir::Value{input.Def()}.SetCastType(T::V128));
            else if (decoration == 3) output.Def()->SetArg(4, ir::Lambda{input});
            else { ir::Params params; params.Push(input); output.Def()->SetArg(4, params); }
            p.Store(8, output); p.Finish(); auto plan = p.Plan(); Check(plan, p.function.body.get());
            REQUIRE(plan.RequiresFullContainer(input.Def())); REQUIRE(plan.RequiresCanonicalHome(input.Def()));
        }
    }
    SECTION("raw marker primary uses the actual scalar result path") {
        Program p; auto producer = p.Add(p.Load(0), 1);
        p.as.SaveFlags(ir::Value{producer.Def()}.SetCastType(T::V128), ir::Flags::NZ); p.Finish();
        auto plan = p.Plan({}); Check(plan, p.function.body.get());
        REQUIRE(plan.RequiresCanonicalHome(producer.Def())); REQUIRE_FALSE(plan.RequiresFullContainer(producer.Def()));
    }
    SECTION("scalar input to vector Phi and raw Phi output retain high qwords") {
        Program p; auto source = p.Load(0), branch = p.Branch(p.Load(8)); auto* label = p.Label();
        ir::Params inputs; inputs.Push(ir::Value{source.Def()}.SetCastType(T::V128));
        inputs.Push(ir::Value{source.Def()}.SetCastType(T::V128));
        auto vector_phi = p.as.AddPhi(inputs).SetType(T::V128); p.Store(16, vector_phi);
        p.Target(branch, label); p.Finish(); auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE(plan.RequiresFullContainer(source.Def())); REQUIRE(plan.RequiresFullContainer(vector_phi.Def()));
    }
}

TEST_CASE("x64 CFG binary phase reuse proves input death and preserves opaque lifetimes", "[x64][cfg-plan][coalescing]") {
    SECTION("two slots carry an invariant and a destructive scalar chain") {
        Program p; auto invariant = p.Load(0), input = p.Load(8);
        auto first = p.Add(input, 1), second = p.Add(first, 2);
        p.Store(16, second); p.Store(24, invariant); p.Finish();
        auto plan = p.Plan({5, 15}); Check(plan, p.function.body.get());
        REQUIRE(plan.CanReuseBinaryInput(first.Def(), input.Def()));
        REQUIRE(plan.CanReuseBinaryInput(second.Def(), first.Def()));
        REQUIRE_FALSE(plan.Interferes(first.Def(), input.Def()));
        REQUIRE(plan.Slot(input.Def())); REQUIRE(plan.Slot(first.Def())); REQUIRE(plan.Slot(second.Def()));
        REQUIRE(plan.Slot(input.Def()) == plan.Slot(first.Def()));
        REQUIRE(plan.Slot(first.Def()) == plan.Slot(second.Def()));
        REQUIRE(plan.Interferes(invariant.Def(), second.Def()));
        auto repeat = p.Plan({5, 15});
        for (const auto& allocation : plan.Allocations()) REQUIRE(repeat.Slot(allocation.producer) == allocation.slot);
    }
    SECTION("all eight audited binary operations capture repeated wider input views") {
        for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::And, O::Or, O::Xor, O::AndNot}) {
            Program p; auto input = p.Load(0);
            auto output = ir::Value{p.as.AppendInst(op, ir::Value{input.Def()}.SetCastType(T::U8),
                            ir::Operand{ir::Value{input.Def()}.SetCastType(T::U64)})}.SetType(T::U64);
            p.Store(8, output); p.Finish();
            auto plan = p.Plan({15}); Check(plan, p.function.body.get());
            REQUIRE(plan.CanReuseBinaryInput(output.Def(), input.Def()));
            REQUIRE(plan.Slot(input.Def())); REQUIRE(plan.Slot(input.Def()) == plan.Slot(output.Def()));
        }
    }
    SECTION("a later ordinary use prevents destructive reuse") {
        Program p; auto input = p.Load(0), output = p.Add(input, 1);
        p.Store(8, output); p.Store(16, input); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE_FALSE(plan.CanReuseBinaryInput(output.Def(), input.Def()));
        REQUIRE(plan.Interferes(output.Def(), input.Def()));
    }
    SECTION("a later flags consumer retains the producer") {
        Program p; auto input = p.Load(0), arithmetic = p.Add(input, 1), output = p.Add(arithmetic, 2);
        p.as.SaveFlags(arithmetic, ir::Flags::All); p.Store(8, output); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE_FALSE(plan.CanReuseBinaryInput(output.Def(), arithmetic.Def()));
        REQUIRE(plan.Interferes(output.Def(), arithmetic.Def()));
    }
    SECTION("a backedge Phi still reads the original input") {
        Program p; auto initial = p.Imm(4), entry = p.Jump(); auto* label = p.Label();
        auto carried = p.as.AddPhi(ir::Params{}).SetType(T::U64), output = p.Add(carried, 1);
        p.Store(8, output); auto back = p.Branch(p.Load(0));
        ir::Params inputs; inputs.Push(initial); inputs.Push(carried); carried.Def()->SetArg(0, inputs);
        p.Target(entry, label, true); p.Target(back, label); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE_FALSE(plan.CanReuseBinaryInput(output.Def(), carried.Def()));
        REQUIRE(plan.Interferes(output.Def(), carried.Def()));
    }
    SECTION("an extra raw occurrence remains live through Write") {
        Program p; auto input = p.Load(0), output = p.Add(input, 1);
        output.Def()->SetArg(4, ir::Value{input.Def()}.SetCastType(T::V128));
        p.Store(8, output); p.Finish(); auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE_FALSE(plan.CanReuseBinaryInput(output.Def(), input.Def()));
        REQUIRE(plan.Interferes(output.Def(), input.Def())); REQUIRE(plan.RequiresCanonicalHome(input.Def()));
    }
    SECTION("a separate raw consumer keeps its home without extending a dead scalar use") {
        Program p; auto input = p.Load(0);
        auto raw = p.as.BitCast(ir::Value{input.Def()}.SetCastType(T::V128)).SetType(T::V128);
        p.Store(16, raw); auto output = p.Add(input, 1); p.Store(8, output); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE(plan.CanReuseBinaryInput(output.Def(), input.Def())); REQUIRE(plan.RequiresCanonicalHome(input.Def()));
    }
    SECTION("non audited output operations retain input through Write") {
        Program p; auto input = p.Load(0), output = p.as.Mul(input, ir::Operand{ir::Imm{u64{3}}}).SetType(T::U64);
        p.Store(8, output); p.Finish(); auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE_FALSE(plan.CanReuseBinaryInput(output.Def(), input.Def())); REQUIRE(plan.Interferes(output.Def(), input.Def()));
    }
}

TEST_CASE("x64 CFG coloring retains profitable chains over long lived raw home values", "[x64][cfg-plan][cost]") {
    for (bool positive_raw_benefit : {false, true}) {
    Program p; auto raw_source = p.Load(0), invariant = p.Load(8), input = p.Load(16);
    auto raw = p.as.BitCast(ir::Value{raw_source.Def()}.SetCastType(T::V128)).SetType(T::V128);
    p.Store(32, raw);
    auto first = p.Add(input, 1), second = p.Add(first, 2), third = p.Add(second, 3);
    p.Store(48, third); p.Store(56, invariant); p.Store(64, raw_source);
    if (positive_raw_benefit) p.Store(72, raw_source);
    p.Finish();
    auto plan = p.Plan({5, 15}); Check(plan, p.function.body.get());
    REQUIRE(plan.RequiresCanonicalHome(raw_source.Def())); REQUIRE_FALSE(plan.Slot(raw_source.Def()));
    const auto raw_allocation = std::find_if(plan.Allocations().begin(), plan.Allocations().end(),
                                           [&](const auto& a) { return a.producer == raw_source.Def(); });
    REQUIRE(raw_allocation != plan.Allocations().end());
    REQUIRE(raw_allocation->scalar_reads == (positive_raw_benefit ? 2 : 1));
    REQUIRE(raw_allocation->estimated_benefit == (positive_raw_benefit ? 1 : 0));
    REQUIRE(raw_allocation->uses > raw_allocation->scalar_reads);
    for (auto value : {invariant, input, first, second, third}) REQUIRE(plan.Slot(value.Def()));
    REQUIRE(plan.Slot(input.Def()) == plan.Slot(first.Def()));
    REQUIRE(plan.Slot(first.Def()) == plan.Slot(second.Def()));
    REQUIRE(plan.Slot(second.Def()) == plan.Slot(third.Def()));
    // The raw source overlaps the chain but cannot inflate the degree of
    // the profitable values' two-register coloring graph.
    REQUIRE(plan.Interferes(raw_source.Def(), second.Def()));
    auto repeat = p.Plan({5, 15});
    for (const auto& allocation : plan.Allocations()) REQUIRE(repeat.Slot(allocation.producer) == allocation.slot);
    }
}

TEST_CASE("x64 delayed flags validate original operands without extending their runtime lifetimes", "[x64][cfg-plan][flags]") {
    for (auto flags : {ir::Flags::CV, ir::Flags::NZCV, ir::Flags::NZ, ir::Flags::Parity, ir::Flags::All}) {
        Program p; auto input = p.Load(0), rhs = p.Load(8);
        auto producer = p.as.Add(input, ir::Operand{rhs}).SetType(T::U64);
        auto successor = p.Add(producer, 2);
        p.as.SaveFlags(producer, flags); p.Store(16, successor); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        // Native CV/NZCV snapshots outlive ALU inputs without retaining the
        // scalar result; NZ-only and parity actually read that result.
        REQUIRE(plan.CanReuseBinaryInput(producer.Def(), input.Def()));
        REQUIRE_FALSE(plan.Interferes(producer.Def(), input.Def()));
        const bool result_read = flags == ir::Flags::NZ || flags == ir::Flags::Parity || flags == ir::Flags::All;
        REQUIRE(plan.CanReuseBinaryInput(successor.Def(), producer.Def()) == !result_read);
        REQUIRE(plan.Interferes(producer.Def(), successor.Def()) == result_read);
        for (auto original : {input, rhs}) {
            REQUIRE(plan.Slot(original.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(original.Def()));
            const auto allocation = std::find_if(plan.Allocations().begin(), plan.Allocations().end(),
                [&](const auto& value) { return value.producer == original.Def(); });
            REQUIRE(allocation != plan.Allocations().end());
            REQUIRE(allocation->uses == 1); REQUIRE(allocation->scalar_reads == 1);
        }
    }
    SECTION("actual raw consumers still require the original operand home") {
        Program p; auto input = p.Load(0);
        auto raw = p.as.BitCast(ir::Value{input.Def()}.SetCastType(T::V128)).SetType(T::V128);
        p.Store(16, raw); auto producer = p.Add(input, 1), successor = p.Add(producer, 2);
        p.as.SaveFlags(producer, ir::Flags::CV); p.Store(8, successor); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE(plan.RequiresCanonicalHome(input.Def()));
        REQUIRE(plan.CanReuseBinaryInput(producer.Def(), input.Def()));
        REQUIRE_FALSE(plan.Interferes(producer.Def(), successor.Def()));
    }
    SECTION("foreign original operands remain invalid in every physical argument form") {
        for (u32 decoration = 0; decoration < 4; ++decoration) {
            Program p, foreign; auto input = p.Load(0), producer = p.Add(input, 1), extra = foreign.Load(0);
            if (decoration == 0) producer.Def()->SetArg(4, extra);
            else if (decoration == 1) producer.Def()->SetArg(4, ir::Value{extra.Def()}.SetCastType(T::V128));
            else if (decoration == 2) producer.Def()->SetArg(4, ir::Lambda{extra});
            else { ir::Params params; params.Push(extra); producer.Def()->SetArg(4, params); }
            p.as.SaveFlags(producer, ir::Flags::CV); p.Finish();
            REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidSSA);
        }
    }
}

TEST_CASE("x64 shared producer flags classifier distinguishes native metadata result and constant publications", "[x64][cfg-plan][flags]") {
    struct Expected { O op; T type; ir::Flags mask; bool metadata, result, native_nz; u64 published; };
    const u64 nz = u64{3} << 30, cv = u64{3} << 28, af = u64{1} << 26;
    for (auto fixture : {
        Expected{O::Add, T::U8, ir::Flags::CV, true, false, true, cv},
        Expected{O::Sub, T::S16, ir::Flags::NZCV, true, false, true, nz | cv},
        Expected{O::Adc, T::U32, ir::Flags::NZ, false, true, false, nz},
        Expected{O::Sbb, T::U64, ir::Flags::All, true, true, true, nz | cv | af | 255},
        Expected{O::Neg, T::S64, ir::Flags::AuxiliaryCarry, true, false, true, af},
        Expected{O::And, T::U64, ir::Flags::CV, false, false, false, cv},
        Expected{O::Or, T::U16, ir::Flags::NZCV, false, true, false, nz | cv},
        Expected{O::Xor, T::U32, ir::Flags::Parity, false, true, false, 255},
        Expected{O::Mul, T::U32, ir::Flags::CV, true, false, false, cv},
        Expected{O::Mul, T::U16, ir::Flags::NZCV, true, true, false, nz | cv},
        Expected{O::Mul, T::U64, ir::Flags::CV, false, false, false, 0},
        Expected{O::LoadUniform, T::U64, ir::Flags::NZCV, false, true, false, nz},
        Expected{O::Add, T::U64, ir::Flags::None, false, false, false, 0},
        Expected{O::Add, T::U64, ir::Flags::Positive, false, false, false, 0},
        Expected{O::Add, T::U64, static_cast<ir::Flags>(u64{1} << 63), false, false, false, 0},
        Expected{O::Add, T::VOID, ir::Flags::All, false, false, false, 0},
        Expected{O::Add, T::V128, ir::Flags::All, false, false, false, 0}}) {
        const auto usage = x64::ClassifyProducerFlags(fixture.op, fixture.type, fixture.mask);
        REQUIRE(usage.needs_metadata == fixture.metadata); REQUIRE(usage.needs_result == fixture.result);
        REQUIRE(usage.nz_from_native == fixture.native_nz); REQUIRE(usage.published_mask == fixture.published);
    }
}

TEST_CASE("x64 metadata-only and GetFlags primary values retain validation without scalar home obligations", "[x64][cfg-plan][flags]") {
    SECTION("dead metadata-only result needs neither scalar slot nor canonical home") {
        Program p; auto input = p.Load(0), producer = p.Add(input, 1), pressure = p.Load(8);
        p.Store(16, p.Add(pressure, 3)); p.as.SaveFlags(producer, ir::Flags::NZCV); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE_FALSE(plan.Slot(producer.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(producer.Def()));
        REQUIRE_FALSE(plan.Interferes(producer.Def(), pressure.Def()));
        const auto allocation = std::find_if(plan.Allocations().begin(), plan.Allocations().end(),
            [&](const auto& value) { return value.producer == producer.Def(); });
        REQUIRE(allocation != plan.Allocations().end()); REQUIRE(allocation->uses == 0);
        REQUIRE(allocation->scalar_reads == 0); REQUIRE(allocation->estimated_benefit == 0);
    }
    SECTION("result publication reads canonical producer despite raw marker view") {
        Program p; auto input = p.Load(0), producer = p.Add(input, 1);
        p.as.SaveFlags(ir::Value{producer.Def()}.SetCastType(T::V128), ir::Flags::NZ); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE(plan.Slot(producer.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(producer.Def()));
        const auto allocation = std::find_if(plan.Allocations().begin(), plan.Allocations().end(),
            [&](const auto& value) { return value.producer == producer.Def(); });
        REQUIRE(allocation->uses == 1); REQUIRE(allocation->scalar_reads == 1);
    }
    SECTION("GetFlags reads packed guest flags and does not read its primary") {
        Program p; auto primary = p.Load(0);
        auto flags = p.as.GetFlags(primary, ir::Flags::NZCV).SetType(T::U64); p.Store(8, flags); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE_FALSE(plan.Slot(primary.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(primary.Def()));
    }
    SECTION("metadata-only primary still rejects foreign and forward definitions") {
        Program p, foreign; auto producer = foreign.Add(foreign.Load(0), 1);
        p.as.SaveFlags(producer, ir::Flags::CV); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidSSA);
        Program forward; auto input = forward.Load(0), local = forward.Add(input, 1);
        auto* marker = forward.as.AppendInst(O::SaveFlags, local, ir::Flags::CV);
        forward.function.body->RemoveInst(marker); forward.function.body->InsertBefore(marker, local.Def()); forward.Finish();
        REQUIRE(forward.Plan().Rejection() == x64::CfgPlanRejection::InvalidSSA);
    }
    SECTION("raw extra occurrences keep the producer canonical home") {
        Program p; auto producer = p.Add(p.Load(0), 1);
        auto* marker = p.as.AppendInst(O::SaveFlags, producer, ir::Flags::CV);
        marker->SetArg(4, ir::Value{producer.Def()}.SetCastType(T::V128)); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get()); REQUIRE(plan.RequiresCanonicalHome(producer.Def()));
    }
}

TEST_CASE("x64 whole CFG allocation rejects invalid metadata Phi counts and nondominating SSA", "[x64][cfg-plan]") {
    SECTION("foreign metadata") {
        Program p, foreign; auto input = p.Load(0); p.Store(8, input); p.Finish();
        auto foreign_branch = foreign.Jump();
        p.function.stops.insert(foreign_branch.Def());
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("halt check cannot replace a uniform store") {
        Program p; auto input = p.Load(0);
        auto* store = p.as.AppendInst(O::StoreUniform, ir::Uniform{8, T::U64}, input);
        p.function.halt_checks.insert(store);
        auto loaded = p.Load(8); p.Store(16, loaded); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("halt check cannot replace a consumed definition") {
        Program p; auto input = p.Load(0); p.function.halt_checks.insert(input.Def());
        p.Store(8, input); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("halt check requires an argument free void Nop") {
        Program p; auto* check = p.as.AppendInst(O::Nop); check->SetReturn(T::U64);
        p.function.halt_checks.insert(check); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
        check->SetReturn(T::VOID); check->SetArg(0, ir::Imm{u64{1}});
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("exit cannot advertise a replaced effect") {
        Program p; auto input = p.Load(0);
        auto* store = p.as.AppendInst(O::StoreUniform, ir::Uniform{8, T::U64}, input);
        p.function.stops.insert(store); p.function.exits.emplace(store, ir::terminal::ReturnToHost{}); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("exit cannot replace a definition") {
        Program p; auto input = p.Load(0); p.Store(8, input);
        p.function.stops.insert(input.Def()); p.function.exits.emplace(input.Def(), ir::terminal::ReturnToHost{}); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("void branch exit marker retains BindLabel identity compatibility") {
        Program p; auto seed = p.Load(0), branch = p.Branch(seed);
        p.as.BindLabel(branch);
        // Goto's argument inference gives it U8 unless a metadata marker
        // explicitly declares that the replaced instruction defines no value.
        branch.Def()->SetReturn(T::VOID);
        p.function.stops.insert(branch.Def());
        p.function.exits.emplace(branch.Def(), ir::terminal::ReturnToHost{}); p.Finish();
        auto plan = p.Plan(); Check(plan, p.function.body.get());
        REQUIRE(plan.Edges().size() == 1); REQUIRE(plan.Edges()[0].host_entry);
        REQUIRE(plan.Blocks().size() == 2); REQUIRE_FALSE(plan.Blocks()[1].reachable);
        REQUIRE_FALSE(plan.Slot(seed.Def())); // The replaced branch never reads its old condition.
        branch.Def()->SetReturn(T::U64);
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("replaced branch cannot authorize a phantom Phi input") {
        Program p; auto seed = p.Load(0), branch = p.Branch(seed), other = p.Load(8);
        p.as.BindLabel(branch);
        branch.Def()->SetReturn(T::VOID);
        ir::Params inputs; inputs.Push(seed); inputs.Push(other);
        auto phi = p.as.AddPhi(inputs).SetType(T::U64); p.Store(16, phi);
        p.function.stops.insert(branch.Def());
        p.function.exits.emplace(branch.Def(), ir::terminal::ReturnToHost{}); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidPhi);
    }
    SECTION("Phi count") {
        Program p; auto seed = p.Load(0), branch = p.Jump(); auto* label = p.Label();
        ir::Params inputs; inputs.Push(seed); inputs.Push(seed);
        auto phi = p.as.AddPhi(inputs).SetType(T::U64); p.Store(8, phi);
        p.Target(branch, label, true); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidPhi);
    }
    SECTION("missing local target") {
        Program p; p.Jump(); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
    SECTION("ordinary non dominating source") {
        Program p; auto condition = p.Load(0), branch = p.Branch(condition);
        auto conditional_value = p.Load(8); auto jump = p.Jump(); auto* join = p.Label();
        p.Store(16, conditional_value); p.Target(branch, join); p.Target(jump, join, true); p.Finish();
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidSSA);
    }
    SECTION("foreign terminal input") {
        Program p, foreign; auto value = foreign.Load(0);
        p.function.body->SetTerminal(ir::terminal::If{ir::BOOL{value}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidSSA);
    }
    SECTION("nested foreign terminal input in empty body") {
        Program p, foreign; auto value = foreign.Load(0);
        p.function.body->SetTerminal(ir::terminal::CheckHalt{ir::terminal::Condition{ir::Cond::EQ,
            ir::terminal::If{ir::BOOL{value}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}},
            ir::terminal::ReturnToHost{}}});
        REQUIRE(p.Plan().Rejection() == x64::CfgPlanRejection::InvalidSSA);
    }
    SECTION("empty body with value free terminal") {
        Program p; p.Finish(); auto plan = p.Plan();
        Check(plan, p.function.body.get());
        REQUIRE(plan.Blocks().empty()); REQUIRE(plan.Edges().empty());
    }
    SECTION("body ownership mismatch") {
        Program p, other; p.Finish(); other.Finish();
        REQUIRE(x64::CfgRegisterPlan::Build(other.function.body.get(), p.function).Rejection() == x64::CfgPlanRejection::InvalidMetadata);
    }
}

