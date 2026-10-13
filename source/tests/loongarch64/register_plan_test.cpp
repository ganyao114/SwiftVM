#include "test_support.h"
#include "runtime/backend/loongarch64/jit/register_plan.h"

#include <queue>
#include <random>

namespace swift::tests::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using I = ir::Inst;
using V = ir::Value;
void Equal(const la::LiveSet& actual, la::LiveSet expected, const char* detail) {
    if (actual != expected) {
        std::cerr << detail << " actual=";
        for (auto* value : actual) std::cerr << value->Id() << ',';
        std::cerr << " expected=";
        for (auto* value : expected) std::cerr << value->Id() << ',';
        std::cerr << '\n';
    }
    Check(actual == expected, detail);
}
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

void StraightAndTerminals() {
    Builder b;
    auto* a = b.Constant(); auto* r = b.Constant();
    auto* sum = b.Add(O::Add, T::U64, V{a}, ir::Operand{V{r}});
    auto* unused = b.Constant(); auto* store = b.Store(sum); b.Finish();
    auto p = la::RegisterPlan::Build(&b.block);
    Equal(p.At(sum), {a, r}, "straight operands");
    Equal(p.After(sum), {sum}, "straight result");
    Equal(p.After(unused), {sum}, "dead result removed");
    Equal(p.After(store), {}, "end has no live values");
    Equal(p.ForFallthrough(sum), p.After(sum), "straight fallthrough");
    Check(p.NextUse(a, r) == p.Position(sum), "next use position");
    Check(p.Stats().nodes == 1, "straight one CFG node");
    auto* predicate = b.Constant(1, T::U8);
    auto* marker = b.Add(O::Nop, T::VOID); b.Finish();
    b.block.SetTerminal(ir::terminal::If{ir::BOOL{predicate}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
    p = la::RegisterPlan::Build(&b.block);
    Equal(p.After(marker), {predicate}, "terminal condition remains live after final instruction");
    Equal(p.ForFallthrough(marker), {predicate}, "terminal fallthrough condition");

    la::RegisterPlan::Exits exits;
    exits.emplace(store, ir::terminal::If{ir::BOOL{a}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
    p = la::RegisterPlan::Build(&b.block, {}, {}, {}, exits);
    Equal(p.After(store), {}, "function exit has no implicit CFG fallthrough");
    Equal(p.ForFallthrough(store), {}, "stopped exit edge is empty");
    Check(p.At(store).contains(a), "exit terminal condition read before its marker");
}

void DiamondAndDuplicateEdges() {
    Builder b;
    auto* a = b.Constant(); auto* cond = b.Constant(1, T::U8);
    auto* split = b.Add(O::NotGoto, T::VOID, V{cond});
    auto* left = b.Add(O::Add, T::U64, V{a}, ir::Operand{ir::Imm{u64{3}}});
    auto* left_edge = b.Add(O::Goto, T::VOID, V{cond});
    auto* other = b.Label();
    auto* right = b.Add(O::Sub, T::U64, V{a}, ir::Operand{ir::Imm{u64{7}}});
    auto* right_edge = b.Add(O::Goto, T::VOID, V{cond});
    auto* join = b.Label();
    ir::Params inputs; inputs.Push(V{left}); inputs.Push(V{right});
    auto* phi = b.Add(O::AddPhi, T::U64, inputs); b.Store(phi); b.Finish();
    la::RegisterPlan::Targets targets{{split, other}, {left_edge, join}, {right_edge, join}};
    la::RegisterPlan::Stops stops{left_edge, right_edge};
    auto p = la::RegisterPlan::Build(&b.block, targets, stops);
    Equal(p.ForBranch(left_edge), {left}, "left edge only reads its phi source");
    Equal(p.ForBranch(right_edge), {right}, "right edge only reads its phi source");
    Equal(p.At(join), {}, "phi destinations killed at target entry");
    Equal(p.After(join), {phi}, "phi destination is defined by incoming parallel copy before markers");
    Equal(p.After(phi), {phi}, "phi result available after marker");
    Check(p.ForBranch(split).contains(a) && p.ForFallthrough(split).contains(a), "diamond retains shared operand on each arm");

    Builder d;
    auto* x = d.Constant(11); auto* y = d.Constant(22); auto* c = d.Constant(1, T::U8);
    auto* edge = d.Add(O::Goto, T::VOID, V{c});
    auto* label = d.Add(O::BindLabel, T::VOID, V{edge});
    ir::Params duplicate; duplicate.Push(V{x}); duplicate.Push(V{y});
    auto* selected = d.Add(O::AddPhi, T::U64, duplicate); d.Store(selected); d.Finish();
    p = la::RegisterPlan::Build(&d.block);
    Equal(p.ForFallthrough(edge), {x}, "duplicate target retains distinct fallthrough source");
    Equal(p.ForBranch(edge), {y}, "duplicate target retains distinct taken source");
    Equal(p.After(edge), {x, y}, "branch union contains both incoming sources");
    const la::PhiEdge explicit_edge{edge, edge, V{x}};
    p = la::RegisterPlan::Build(&d.block, {}, {}, std::span{&explicit_edge, 1});
    Check(p.ForBranch(edge).contains(x) && p.ForBranch(edge).contains(y), "ambiguous explicit edge conservatively unions sources");
    (void)label;
}

void BackedgesAndParallelPhi() {
    Builder b;
    auto* invariant = b.Constant(); auto* yes = b.Constant(1, T::U8);
    auto* label = b.Label();
    auto* sum = b.Add(O::Add, T::U64, V{invariant}, ir::Operand{ir::Imm{u64{2}}}); b.Store(sum);
    auto* branch = b.Add(O::Goto, T::VOID, V{yes}); b.Finish();
    auto p = la::RegisterPlan::Build(&b.block, {{branch, label}});
    Check(p.After(branch).contains(invariant), "backedge preserves invariant whose lexical use already passed");
    Check(p.NextUse(invariant, branch) == la::RegisterPlan::kNoNextUse, "lexical next-use sentinel cannot imply deadness");
    Check(p.Stats().iterations >= 2, "cycle solved to stable fixed point");

    Builder loop;
    auto* condition = loop.Constant(1, T::U8); auto* header = loop.Label();
    auto* x = new I(O::AddPhi); auto* y = new I(O::AddPhi);
    x->SetReturn(T::U64); y->SetReturn(T::U64);
    ir::Params xp; xp.Push(ir::Imm{u64{31}}); xp.Push(V{y}); x->SetArgs(xp);
    ir::Params yp; yp.Push(ir::Imm{u64{47}}); yp.Push(V{x}); y->SetArgs(yp);
    loop.block.AppendInstUnchecked(x); loop.block.AppendInstUnchecked(y); loop.Store(x);
    auto* back = loop.Add(O::Goto, T::VOID, V{condition}); loop.Finish();
    p = la::RegisterPlan::Build(&loop.block, {{back, header}}, {back});
    Equal(p.ForBranch(back), {x, y, condition}, "parallel cyclic phi copies retain both old sources");
    Equal(p.At(header), {condition}, "parallel phi destinations do not escape through entry live-in");
}

void EarlyPseudosAndFlags() {
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
        auto* consumer = b.Store(pseudo); b.Add(O::Nop, T::VOID);
        b.block.AppendInstUnchecked(pseudo); b.Finish();
        auto p = la::RegisterPlan::Build(&b.block, {}, {edge});
        Check(p.After(producer).contains(pseudo), "producer defines delayed pseudo result");
        Check(p.ForBranch(edge).contains(pseudo) && p.At(label).contains(pseudo), "branch publishes early pseudo before delayed marker");
        Check(p.At(consumer).contains(pseudo), "consumer sees pseudo before marker");
        Check(!p.At(producer).contains(pseudo), "producer kills early pseudo live-in");
        Check(!p.At(pseudo).contains(producer), "no-op pseudo marker has no spurious producer use");
    }
    Builder flags;
    auto* input = flags.Constant();
    auto* result = flags.Add(O::Add, T::U64, V{input}, ir::Operand{ir::Imm{u64{1}}});
    flags.Add(O::Nop, T::VOID);
    auto* save = flags.Add(O::SaveFlags, T::VOID, V{result}, ir::Flags::All); flags.Finish();
    auto p = la::RegisterPlan::Build(&flags.block);
    Check(p.After(result).contains(input) && p.At(save).contains(input), "flags hidden producer operand read remains live");
}

// Independent reachability oracle: a value is live iff a path reaches a use
// before encountering its definition again. This uses no fixed-point sets.
bool ReachUse(u32 first, I* value, const std::vector<la::LiveSet>& uses,
              const std::vector<la::LiveSet>& defs, const std::vector<std::vector<u32>>& successors) {
    std::vector<bool> visited(uses.size());
    std::queue<u32> queue; queue.push(first);
    while (!queue.empty()) {
        const u32 at = queue.front(); queue.pop();
        if (visited[at]) continue; visited[at] = true;
        if (uses[at].contains(value)) return true;
        if (defs[at].contains(value)) continue;
        for (const auto next : successors[at]) queue.push(next);
    }
    return false;
}
void RandomReachability() {
    std::mt19937 rng{0x5ca1ab1e};
    for (u32 trial = 0; trial < 100; ++trial) {
        Builder b;
        std::vector<I*> constants, labels, branches;
        for (u32 i = 0; i < 6; ++i) constants.push_back(b.Constant(i + 1, T::U8));
        for (u32 n = 0; n < 8; ++n) {
            labels.push_back(b.Label());
            auto* sum = b.Add(O::Add, T::U8, V{constants[rng() % constants.size()]},
                             ir::Operand{V{constants[rng() % constants.size()]}});
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
        auto plan = la::RegisterPlan::Build(&b.block, targets, stops);
        std::vector<I*> insts;
        std::unordered_map<I*, u32> positions;
        for (auto& inst : b.block.GetInstList()) { positions[&inst] = insts.size(); insts.push_back(&inst); }
        const u32 count = insts.size();
        std::vector<la::LiveSet> uses(count + 1), defs(count + 1);
        std::vector<std::vector<u32>> successors(count + 1);
        uses[count].insert(terminal_condition);
        for (u32 i = 0; i < count; ++i) {
            auto* inst = insts[i];
            if (inst->GetOp() != O::BindLabel) for (u32 slot = 0; slot < I::max_args; ++slot) {
                auto& arg = inst->ArgAt(slot);
                if (arg.IsValue()) uses[i].insert(arg.Get<V>().Def());
            }
            if (inst->ReturnType() != T::VOID) defs[i].insert(inst);
            if (!stops.contains(inst)) successors[i].push_back(i + 1);
            if (targets.contains(inst)) successors[i].push_back(positions.at(targets.at(inst)));
        }
        for (u32 i = 0; i < count; ++i) {
            la::LiveSet expected_before, expected_after, expected_taken, expected_fallthrough;
            for (auto* value : insts) {
                if (value->ReturnType() == T::VOID) continue;
                if (ReachUse(i, value, uses, defs, successors)) expected_before.insert(value);
                for (const auto next : successors[i]) if (ReachUse(next, value, uses, defs, successors)) expected_after.insert(value);
                if (!stops.contains(insts[i]) && ReachUse(i + 1, value, uses, defs, successors)) expected_fallthrough.insert(value);
                if (targets.contains(insts[i]) && ReachUse(positions.at(targets.at(insts[i])), value, uses, defs, successors)) expected_taken.insert(value);
            }
            Equal(plan.At(insts[i]), expected_before, "random CFG live-before matches reachability");
            Equal(plan.After(insts[i]), expected_after, "random CFG live-after matches reachability");
            Equal(plan.ForBranch(insts[i]), expected_taken, "random CFG taken edge matches reachability");
            Equal(plan.ForFallthrough(insts[i]), expected_fallthrough, "random CFG fallthrough edge matches reachability");
        }
    }
}

}  // namespace

void RegisterAllocationPlan() {
    StraightAndTerminals();
    DiamondAndDuplicateEdges();
    BackedgesAndParallelPhi();
    EarlyPseudosAndFlags();
    RandomReachability();
    std::cout << "PASS full CFG register plan, Phi edges, pseudo lifetimes and reachability oracle\n";
}

}  // namespace swift::tests::loongarch64
