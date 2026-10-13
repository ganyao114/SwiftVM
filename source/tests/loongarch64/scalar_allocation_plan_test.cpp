#include "test_support.h"
#include "runtime/backend/loongarch64/jit/scalar_allocation_plan.h"

#include <algorithm>
#include <queue>
#include <random>

namespace swift::tests::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using I = ir::Inst;
using V = ir::Value;

struct Builder {
    ir::Block block{ir::Location{0x1000}};
    Builder() { block.SetTerminal(ir::terminal::ReturnToHost{}); }
    template<class... Args> I* Add(O op, T type, const Args&... args) {
        auto* inst = I::Create(op, args...);
        inst->SetReturn(type); block.AppendInstUnchecked(inst); return inst;
    }
    I* Constant(u64 value = 17, T type = T::U64) { return Add(O::LoadImm, type, ir::Imm{value}); }
    I* Label() {
        auto* inst = new I(O::BindLabel);
        inst->SetArgs(V{inst}); inst->SetReturn(T::VOID); block.AppendInstUnchecked(inst); return inst;
    }
    I* Store(I* value) { return Add(O::StoreUniform, T::VOID, ir::Uniform{0, value->ReturnType()}, V{value}); }
    void Finish() { block.ReIdInstr(); }
};

void Distinct(const la::ScalarAllocationPlan& allocation, const std::vector<I*>& values,
              const char* detail) {
    for (u32 left = 0; left < values.size(); ++left) for (u32 right = left + 1; right < values.size(); ++right) {
        const auto a = allocation.Register(values[left]), b = allocation.Register(values[right]);
        Check(!a || !b || a != b, detail);
        Check(allocation.Interferes(values[left], values[right]), "independent simultaneous values have interference edge");
    }
}

void PressureAndDefinitionOrder() {
    Builder b;
    std::vector<I*> constants;
    for (u32 i = 0; i < 9; ++i) constants.push_back(b.Constant(i + 1));
    for (auto* value : constants) b.Store(value);
    auto* vector = b.Add(O::LoadUniform, T::V128, ir::Uniform{0, T::V128}); b.Store(vector);
    b.Finish();
    const auto live = la::RegisterPlan::Build(&b.block);
    const auto allocation = la::ScalarAllocationPlan::Build(&b.block, live);
    Distinct(allocation, constants, "nine live constants never share a register");
    Check(allocation.Stats().spilled_values == 3 && allocation.Stats().register_values == 6,
          "nine-value clique uses six registers and three homes");
    Check(!allocation.Register(vector) && !allocation.Spilled(vector), "vector is outside scalar allocator");
    const auto fallback = la::ScalarAllocationPlan::Build(&b.block, live, {}, 0);
    Check(fallback.Stats().spilled_values == 9 && fallback.Stats().register_values == 0, "zero registers is canonical-home fallback");
    for (auto* value : constants) Check(allocation.Register(value) == la::ScalarAllocationPlan::Build(&b.block, live).Register(value),
                                       "coloring is deterministic");

    Builder reread;
    auto* left = reread.Constant(), *right = reread.Constant(29);
    auto* result = reread.Add(O::Add, T::U64, V{left}, ir::Operand{V{right}});
    auto* save = reread.Add(O::SaveFlags, T::VOID, V{result}, ir::Flags::All);
    reread.Store(result); reread.Finish();
    const auto reread_live = la::RegisterPlan::Build(&reread.block);
    const auto reread_allocation = la::ScalarAllocationPlan::Build(&reread.block, reread_live);
    Distinct(reread_allocation, {left, right, result}, "result cannot overwrite implicit flags input");
    Check(reread_allocation.Inputs(save).contains(left) && reread_allocation.Inputs(save).contains(right),
          "helper publication includes hidden flags producer inputs");
}

void PhiCycleAndDiamond() {
    Builder loop;
    auto* condition = loop.Constant(1, T::U8); auto* header = loop.Label();
    auto* x = new I(O::AddPhi); auto* y = new I(O::AddPhi);
    x->SetReturn(T::U64); y->SetReturn(T::U64);
    ir::Params xp; xp.Push(ir::Imm{u64{31}}); xp.Push(V{y}); x->SetArgs(xp);
    ir::Params yp; yp.Push(ir::Imm{u64{47}}); yp.Push(V{x}); y->SetArgs(yp);
    loop.block.AppendInstUnchecked(x); loop.block.AppendInstUnchecked(y); loop.Store(x); loop.Store(y);
    auto* back = loop.Add(O::Goto, T::VOID, V{condition}); loop.Finish();
    const la::RegisterPlan::Targets targets{{back, header}};
    const auto live = la::RegisterPlan::Build(&loop.block, targets, {back});
    const auto allocation = la::ScalarAllocationPlan::Build(&loop.block, live, targets);
    Check(allocation.Definitions(header).contains(x) && allocation.Definitions(header).contains(y),
          "phi destinations assigned together at entry");
    Check(allocation.Definitions(x).empty() && allocation.Definitions(y).empty(), "phi markers do not redefine allocated value");
    Distinct(allocation, {condition, x, y}, "loop phi cycle and condition use distinct registers");
    std::array<u64, la::ScalarAllocationPlan::kRegisterCount> physical{};
    physical[*allocation.Register(x)] = 31; physical[*allocation.Register(y)] = 47;
    for (u32 iteration = 0; iteration < 20; ++iteration) {
        const auto old_x = physical[*allocation.Register(x)], old_y = physical[*allocation.Register(y)];
        physical[*allocation.Register(x)] = old_y; physical[*allocation.Register(y)] = old_x;
        Check(physical[*allocation.Register(x)] == (iteration & 1 ? 31 : 47), "parallel backedge x value");
        Check(physical[*allocation.Register(y)] == (iteration & 1 ? 47 : 31), "parallel backedge y value");
    }

    Builder diamond;
    auto* input = diamond.Constant(), *cond = diamond.Constant(1, T::U8);
    auto* split = diamond.Add(O::NotGoto, T::VOID, V{cond});
    auto* left = diamond.Add(O::Add, T::U64, V{input}, ir::Operand{ir::Imm{u64{3}}});
    auto* left_edge = diamond.Add(O::Goto, T::VOID, V{cond});
    auto* other = diamond.Label();
    auto* right = diamond.Add(O::Sub, T::U64, V{input}, ir::Operand{ir::Imm{u64{7}}});
    auto* right_edge = diamond.Add(O::Goto, T::VOID, V{cond});
    auto* join = diamond.Label();
    ir::Params inputs; inputs.Push(V{left}); inputs.Push(V{right});
    auto* phi = diamond.Add(O::AddPhi, T::U64, inputs); diamond.Store(phi); diamond.Finish();
    const la::RegisterPlan::Targets routes{{split, other}, {left_edge, join}, {right_edge, join}};
    const auto diamond_live = la::RegisterPlan::Build(&diamond.block, routes, {left_edge, right_edge});
    const auto colored = la::ScalarAllocationPlan::Build(&diamond.block, diamond_live, routes);
    Check(colored.Stats().spilled_values == 0, "diamond entirely register assigned");
    Check(!colored.Interferes(left, right) && !colored.Interferes(left, phi) && !colored.Interferes(right, phi),
          "edge-specific phi sources can coalesce with destination");
    Check(colored.Stats().phi_coalesced >= 1, "color preference coalesces a noninterfering phi source");
}

void HelperEarlyResultsAndTerminal() {
    for (O op : {O::VecSatMulQC, O::VecSatShiftQC, O::Div128Remainder, O::CpuidUpper}) {
        Builder b;
        const bool qc = op == O::VecSatMulQC || op == O::VecSatShiftQC;
        auto* condition = b.Constant(1, T::U8);
        auto* input = b.Add(O::LoadUniform, qc ? T::V128 : T::U64, ir::Uniform{0, qc ? T::V128 : T::U64});
        I* producer;
        if (op == O::VecSatMulQC)
            producer = b.Add(O::VecSatMulHigh, T::V128, V{input}, V{input}, V{input}, ir::Imm{u32{32}}, ir::Imm{u32{0}});
        else if (op == O::VecSatShiftQC)
            producer = b.Add(O::VecSatShiftImm, T::V128, V{input}, ir::Imm{u32{1}}, ir::Imm{u32{32}}, ir::Imm{u32{0}});
        else if (op == O::Div128Remainder)
            producer = b.Add(O::Div128, T::U64, V{input}, V{input}, V{input}, ir::Imm{u32{0}});
        else producer = b.Add(O::Cpuid, T::U64, V{input}, V{input}, ir::Imm{u32{0}}, ir::Imm{u32{0}});
        auto* pseudo = new I(op); pseudo->SetArgs(V{producer}); pseudo->SetReturn(qc ? T::U8 : T::U64);
        auto* edge = b.Add(O::Goto, T::VOID, V{condition});
        auto* label = b.Add(O::BindLabel, T::VOID, V{edge});
        b.Store(pseudo); b.Store(condition); b.block.AppendInstUnchecked(pseudo); b.Finish();
        const auto live = la::RegisterPlan::Build(&b.block, {}, {edge});
        const auto allocation = la::ScalarAllocationPlan::Build(&b.block, live);
        Check(allocation.Definitions(producer).contains(pseudo), "helper reloads early pseudo at producer");
        Check(allocation.Definitions(pseudo).empty(), "delayed pseudo marker cannot overwrite allocated result");
        Distinct(allocation, {condition, pseudo}, "early pseudo crosses branch beside invariant");
        if (!qc) {
            Distinct(allocation, {input, producer, pseudo}, "helper input/result/pair result distinct");
            Check(allocation.Inputs(producer).contains(input), "helper publishes scalar typed input");
        }
        Check(live.At(label).contains(pseudo), "pseudo is live at branch target");
    }
    Builder terminal;
    auto* predicate = terminal.Constant(1, T::U8), *other = terminal.Constant(4);
    auto* marker = terminal.Add(O::Nop, T::VOID); terminal.Finish();
    terminal.block.SetTerminal(ir::terminal::If{ir::BOOL{predicate}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
    const la::RegisterPlan::Exits exits{{marker, ir::terminal::If{ir::BOOL{predicate},
            ir::terminal::Switch{V{other}, {}}, ir::terminal::ReturnToHost{}}}};
    const auto live = la::RegisterPlan::Build(&terminal.block, {}, {}, {}, exits);
    const auto allocation = la::ScalarAllocationPlan::Build(&terminal.block, live);
    Check(allocation.Register(other).has_value(), "function terminal scalar gets a global register");
    Distinct(allocation, {predicate, other}, "final terminal value survives later definition");
}

// Independent instruction reachability oracle. It does not inspect the
// allocator's graph or RegisterPlan sets: find a use along any path before the
// next definition. Before/after simultaneous values must have distinct colors.
bool ReachUse(u32 first, I* value, const std::vector<la::LiveSet>& uses,
              const std::vector<la::LiveSet>& defs, const std::vector<std::vector<u32>>& successors) {
    std::vector<bool> visited(uses.size());
    std::queue<u32> queue; queue.push(first);
    while (!queue.empty()) {
        const u32 at = queue.front(); queue.pop();
        if (visited[at]) continue;
        visited[at] = true;
        if (uses[at].contains(value)) return true;
        if (defs[at].contains(value)) continue;
        for (const auto next : successors[at]) queue.push(next);
    }
    return false;
}

void RandomIndependentInterference() {
    std::mt19937 rng{0x10a64c01};
    for (u32 trial = 0; trial < 120; ++trial) {
        Builder b;
        std::vector<I*> constants, labels, branches;
        for (u32 i = 0; i < 3 + rng() % 10; ++i) constants.push_back(b.Constant(i + 1, T::U64));
        for (u32 n = 0; n < 7; ++n) {
            labels.push_back(b.Label());
            auto* sum = b.Add(O::Add, T::U64, V{constants[rng() % constants.size()]},
                             ir::Operand{V{constants[rng() % constants.size()]}});
            if (rng() & 1) b.Add(O::SaveFlags, T::VOID, V{sum}, ir::Flags::All);
            b.Store(sum);
            branches.push_back(b.Add(O::Goto, T::VOID, V{constants[rng() % constants.size()]}));
        }
        auto* terminal_condition = constants[rng() % constants.size()];
        b.block.SetTerminal(ir::terminal::If{ir::BOOL{terminal_condition}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
        b.Finish();
        la::RegisterPlan::Targets targets;
        la::RegisterPlan::Stops stops;
        for (auto* branch : branches) {
            targets.emplace(branch, labels[rng() % labels.size()]);
            if (rng() & 1) stops.insert(branch);
        }
        const auto live = la::RegisterPlan::Build(&b.block, targets, stops);
        const auto plan = la::ScalarAllocationPlan::Build(&b.block, live, targets, trial % 7);
        std::vector<I*> insts, values;
        std::unordered_map<I*, u32> positions;
        for (auto& inst : b.block.GetInstList()) {
            positions[&inst] = insts.size(); insts.push_back(&inst);
            if (inst.ReturnType() != T::VOID) values.push_back(&inst);
        }
        const u32 count = insts.size();
        std::vector<la::LiveSet> uses(count + 1), defs(count + 1);
        std::vector<std::vector<u32>> successors(count + 1);
        uses[count].insert(terminal_condition);
        const auto operands = [&](I* inst, la::LiveSet& set) {
            for (u32 slot = 0; slot < I::max_args; ++slot) {
                auto& arg = inst->ArgAt(slot);
                if (arg.IsValue()) set.insert(arg.Get<V>().Def());
            }
        };
        for (u32 i = 0; i < count; ++i) {
            auto* inst = insts[i];
            if (inst->GetOp() != O::BindLabel) operands(inst, uses[i]);
            if (inst->GetOp() == O::SaveFlags) operands(inst->GetArg<V>(0).Def(), uses[i]);
            if (inst->ReturnType() != T::VOID) defs[i].insert(inst);
            if (!stops.contains(inst)) successors[i].push_back(i + 1);
            if (targets.contains(inst)) successors[i].push_back(positions.at(targets.at(inst)));
        }
        for (u32 i = 0; i < count; ++i) {
            std::vector<I*> simultaneous;
            for (auto* value : values) {
                bool present = defs[i].contains(value) || ReachUse(i, value, uses, defs, successors);
                for (const auto next : successors[i]) present |= ReachUse(next, value, uses, defs, successors);
                if (present) simultaneous.push_back(value);
                const auto assigned = plan.Register(value);
                Check(!assigned || *assigned < trial % 7, "all physical colors fit configured register budget");
            }
            Distinct(plan, simultaneous, "random independent reachability never aliases simultaneous values");
        }
    }
}

void EstimatedMemoryTraffic() {
    Builder b;
    auto* value = b.Constant(1);
    for (u32 iteration = 0; iteration < 64; ++iteration)
        value = b.Add(O::Add, T::U64, V{value}, ir::Operand{ir::Imm{u64{3}}});
    b.Store(value); b.Finish();
    const auto live = la::RegisterPlan::Build(&b.block);
    const auto plan = la::ScalarAllocationPlan::Build(&b.block, live);
    const auto stats = plan.Stats();
    Check(stats.spilled_values == 0 && stats.spill_input_sites == 0 && stats.spill_definition_sites == 0,
          "whole chain needs no scalar home traffic");
    Check(stats.input_sites == 65 && stats.definition_sites == 65, "chain traffic estimate counts unique reads and definitions");
    std::cout << "static scalar chain estimated home loads/stores: baseline=" << stats.input_sites << '/'
              << stats.definition_sites << " allocated=" << stats.spill_input_sites << '/'
              << stats.spill_definition_sites << " spilled_values=" << stats.spilled_values << '\n';
}

}  // namespace

void ScalarAllocationPlan() {
    PressureAndDefinitionOrder(); PhiCycleAndDiamond(); HelperEarlyResultsAndTerminal();
    RandomIndependentInterference(); EstimatedMemoryTraffic();
    std::cout << "PASS whole-CFG static scalar coloring, phi cycles, helper pseudos and independent interference oracle\n";
}

}  // namespace swift::tests::loongarch64
