#include "test_support.h"
#include "runtime/backend/function_plan.h"
#include "runtime/backend/loongarch64/jit/flags_plan.h"

#include <queue>
#include <random>

namespace swift::tests::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using F = ir::Flags;
using I = ir::Inst;
using V = ir::Value;

struct Builder {
    ir::Block block{ir::Location{0x9000}};
    Builder() { block.SetTerminal(ir::terminal::ReturnToHost{}); }
    template <class... Args> I* Add(O op, T type, const Args&... args) {
        auto* inst = I::Create(op, args...);
        inst->SetReturn(type); block.AppendInstUnchecked(inst); return inst;
    }
    I* Constant(u64 value = 1) { return Add(O::LoadImm, T::U64, ir::Imm{value}); }
    I* Label() {
        auto* inst = new I{O::BindLabel}; inst->SetArgs(V{inst}); inst->SetReturn(T::VOID);
        block.AppendInstUnchecked(inst); return inst;
    }
    void Finish() { block.ReIdInstr(); }
};

void StraightPublicationAndDemand() {
    Builder b;
    auto* value = b.Constant();
    auto* clean = b.Add(O::CallLambda, T::VOID);
    auto* carry = b.Add(O::SetCarry, T::VOID, V{value});
    auto* first = b.Add(O::CallLambda, T::VOID);
    auto* again = b.Add(O::CallLambda, T::VOID);
    auto* overflow = b.Add(O::SetOverflow, T::VOID, V{value});
    auto* fault = b.Add(O::LoadMemory, T::U64, ir::Operand{V{value}});
    auto* tail = b.Add(O::CallLambda, T::VOID); b.Finish();
    const auto plan = la::FlagsPlan::Build(&b.block);
    Check(plan.At(clean) == F::None && plan.At(first) == F::Carry,
          "flags publication is omitted only at a synchronized helper boundary");
    Check(plan.After(first) == F::None && plan.At(again) == F::None,
          "known helper reload makes a repeated publication redundant");
    Check(plan.At(fault) == F::Overflow && plan.After(fault) == F::None && plan.At(tail) == F::None,
          "native fault access publishes before the fault and leaves the resumed path synchronized");
    Check(plan.Stats().dirty_boundaries == 2 && plan.Stats().clean_boundaries == 3,
          "dirty and clean boundaries count distinct reachable publication sites");
    Check(plan.ExitDirty() == F::None, "a final helper gives a synchronized ordinary and fault exit");
    Check(plan.NeededWrite(carry) == F::Carry && plan.NeededWrite(overflow) == F::Overflow,
          "helper and guest-fault observers require preceding partial writes");

    Builder demand;
    auto* input = demand.Constant();
    auto* result = demand.Add(O::Add, T::U64, V{input}, ir::Operand{ir::Imm{u64{1}}});
    auto* full = demand.Add(O::SaveFlags, T::VOID, V{result}, F::All);
    auto* adc = demand.Add(O::Adc, T::U64, V{input}, ir::Operand{ir::Imm{u64{1}}});
    auto* overwrite = demand.Add(O::SaveFlags, T::VOID, V{adc}, F::All);
    auto* observer = demand.Add(O::CallLambda, T::VOID); demand.Finish();
    const auto chain = la::FlagsPlan::Build(&demand.block);
    Check(chain.NeededWrite(full) == F::Carry && chain.NeededWrite(overwrite) == F::All,
          "carry chain demands only carry until the final full publication");
    Check(chain.EffectAt(overwrite).reads == F::Carry && chain.NeededBefore(overwrite) == F::Carry,
          "SaveFlags of ADC reads the incoming carry at its own IR position");
    Check(chain.At(observer) == F::All, "full final flag write dirties every architectural flag");
    auto* snapshot = demand.Add(O::GetFlags, T::U64, V{result}, F::Carry);
    demand.Finish();
    Check(la::FlagsPlan::Effect(snapshot).reads == F::All,
          "GetFlags observes the complete word even when its annotation requests carry only");

    for (u32 condition = 0; condition < 16; ++condition) {
        const std::array<F, 16> expected{F::Zero, F::Zero, F::Carry, F::Carry,
            F::Negate, F::Negate, F::Overflow, F::Overflow, F::Carry | F::Zero, F::Carry | F::Zero,
            F::Negate | F::Overflow, F::Negate | F::Overflow,
            F::Negate | F::Overflow | F::Zero, F::Negate | F::Overflow | F::Zero, F::None, F::None};
        Check(la::FlagsPlan::ConditionDemand(ir::Cond(condition)) == expected[condition],
              "condition demand is the independent complete NZCV truth-table input mask");
    }
}

void DiamondsPhisAndBackedges() {
    Builder b;
    auto* value = b.Constant();
    auto* split = b.Add(O::NotGoto, T::VOID, V{value});
    b.Add(O::SetCarry, T::VOID, V{value});
    auto* left = b.Add(O::Goto, T::VOID, V{value});
    auto* other = b.Label();
    b.Add(O::SetOverflow, T::VOID, V{value});
    auto* right = b.Add(O::Goto, T::VOID, V{value});
    auto* join = b.Label();
    ir::Params inputs; inputs.Push(V{value}); inputs.Push(V{value});
    auto* phi = b.Add(O::AddPhi, T::U64, inputs);
    auto* helper = b.Add(O::CallLambda, T::VOID); b.Finish();
    auto plan = la::FlagsPlan::Build(&b.block, {{split, other}, {left, join}, {right, join}}, {left, right});
    Check(plan.At(join) == F::CV && plan.At(phi) == F::CV && plan.At(helper) == F::CV,
          "Phi does not clear the union of dirty flags arriving from a diamond");
    Check(plan.After(helper) == F::None && plan.ExitDirty() == F::CV,
          "shared epilogue includes dirty early-poll exits even after a final reload");

    Builder loop;
    auto* condition = loop.Constant(); auto* header = loop.Label();
    auto* top = loop.Add(O::CallLambda, T::VOID);
    loop.Add(O::SetCarry, T::VOID, V{condition});
    auto* back = loop.Add(O::Goto, T::VOID, V{condition}); loop.Finish();
    plan = la::FlagsPlan::Build(&loop.block, {{back, header}});
    Check(plan.At(header) == F::Carry && plan.At(top) == F::Carry,
          "loop header joins the clean host entry with the dirty backedge");
    Check(plan.Stats().dirty_iterations >= 2 && plan.ExitDirty() == F::Carry,
          "may-dirty fixed point retains loop-carried flags and the fallthrough exit");

    Builder duplicate;
    auto* c = duplicate.Constant(); duplicate.Add(O::SetOverflow, T::VOID, V{c});
    auto* edge = duplicate.Add(O::Goto, T::VOID, V{c});
    auto* label = duplicate.Add(O::BindLabel, T::VOID, V{edge});
    auto* call = duplicate.Add(O::CallLambda, T::VOID); duplicate.Finish();
    plan = la::FlagsPlan::Build(&duplicate.block);
    Check(plan.At(label) == F::Overflow && plan.At(call) == F::Overflow,
          "duplicate taken/fallthrough targets preserve dirty state");
}

void BoundariesUnreachableAndOpaque() {
    Builder b;
    auto* value = b.Constant(); b.Add(O::SetCarry, T::VOID, V{value});
    auto* retire = b.Add(O::AdvancePC, T::VOID, ir::Imm{u64{4}});
    auto* alignment = b.Add(O::CheckMemoryAlignment, T::VOID, V{value}, ir::Imm{u64{7}});
    auto* halted = b.Add(O::Nop, T::VOID);
    auto* exit = b.Add(O::Nop, T::VOID);
    auto* dead = b.Add(O::SetOverflow, T::VOID, V{value}); b.Finish();
    la::FlagsPlan::Exits exits{{exit, ir::terminal::ReturnToHost{}}};
    auto plan = la::FlagsPlan::Build(&b.block, {}, {}, exits, {halted});
    Check(plan.EffectAt(retire).exits_before && plan.EffectAt(alignment).exits_after &&
          plan.EffectAt(halted).exits_before, "retirement, alignment and function halt-check paths are observed");
    Check(plan.ExitDirty() == F::Carry && !plan.Reachable(dead) && plan.At(dead) == F::All,
          "unreachable writes cannot poison reachable exits or justify omitted publications");

    Builder unknown;
    auto* opaque = unknown.Add(static_cast<O>(0x7fff), T::VOID);
    auto* boundary = unknown.Add(O::CallLambda, T::VOID); unknown.Finish();
    plan = la::FlagsPlan::Build(&unknown.block);
    Check(plan.EffectAt(opaque).opaque && plan.At(boundary) == F::All && plan.Stats().opaque == 1,
          "unknown lowering dirties all flags until a known helper reload");

    Builder guarded;
    auto* input = guarded.Constant(); guarded.Add(O::SetCarry, T::VOID, V{input});
    auto* optional = guarded.Add(O::VecAdd, T::V128);
    auto* following = guarded.Add(O::CallLambda, T::VOID); guarded.Finish();
    plan = la::FlagsPlan::Build(&guarded.block);
    Check(plan.EffectAt(optional).observes_before && !plan.EffectAt(optional).synchronized_after &&
          plan.At(following) == F::Carry, "native/helper guarded join does not assume the native path reloaded flags");

    Builder partial;
    auto* raw = partial.Constant();
    auto* logical = partial.Add(O::And, T::U64, V{raw}, ir::Operand{ir::Imm{u64{1}}});
    auto* save = partial.Add(O::SaveFlags, T::VOID, V{logical}, F::AuxiliaryCarry);
    partial.Finish();
    Check(la::FlagsPlan::Effect(save).writes == F::None,
          "SaveFlags does not claim to replace undefined logical auxiliary carry");
    auto* opaque_producer = partial.Add(O::PopCount, T::U64, V{raw});
    auto* unsupported = partial.Add(O::SaveFlags, T::VOID, V{opaque_producer}, F::CV);
    Check(la::FlagsPlan::Effect(unsupported).writes == F::None,
          "unsupported carry/overflow producer does not kill old demand");

    Builder invalid;
    auto* condition = invalid.Constant(); auto* branch = invalid.Add(O::Goto, T::VOID, V{condition});
    invalid.Finish();
    bool rejected{};
    try { (void)la::FlagsPlan::Build(&invalid.block); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "unbound local CFG is rejected before dirty decisions are used");
    rejected = false;
    try { (void)la::FlagsPlan::Build(&invalid.block, {{branch, retire}}); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "foreign function CFG coordinate is rejected");
}

void LoweredFunctionCFG() {
    ir::HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(ir::Location{0xa000});
    auto input = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
    auto condition = function->TestZero<ir::U8>(input);
    builder.If(ir::terminal::If{ir::BOOL{condition}, ir::terminal::LinkBlock{ir::Location{0xa100}},
                              ir::terminal::LinkBlock{ir::Location{0xa200}}});
    builder.SetCurBlock(ir::Location{0xa100});
    function->SetCarry(input);
    auto left = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{1}}});
    builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xa300}});
    builder.SetCurBlock(ir::Location{0xa200});
    function->SetOverflow(input);
    auto right = function->Sub<ir::U64>(input, ir::Operand{ir::Imm{u64{1}}});
    builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xa300}});
    builder.SetCurBlock(ir::Location{0xa300});
    ir::Params params; params.Push(left); params.Push(right);
    auto phi = function->AddPhi<ir::U64>(params);
    function->StoreUniform(ir::Uniform{8, T::U64}, phi);
    function->EndBlock(ir::terminal::CheckHalt{ir::terminal::ReturnToHost{}});
    function->EndFunction();
    std::vector<ir::HIRBlock*> ordered;
    for (u64 location : {0xa000ULL, 0xa100ULL, 0xa200ULL, 0xa300ULL})
        for (auto* block : function->GetHIRBlocks())
            if (block->GetBlock()->GetStartLocation() == ir::Location{location}) ordered.push_back(block);
    auto lowered = backend::FunctionPlan::Build(function, ordered);
    const auto plan = la::FlagsPlan::Build(lowered.body.get(), lowered.targets, lowered.stops,
                                          lowered.exits, lowered.halt_checks);
    bool found_phi{};
    for (auto& inst : lowered.body->GetInstList()) {
        if (inst.GetOp() == O::AddPhi) {
            found_phi = true;
            Check(plan.At(&inst) == F::CV, "lowered whole-function Phi joins the two flag domains");
        }
        if (lowered.exits.contains(&inst) || lowered.halt_checks.contains(&inst))
            Check(plan.At(&inst) == F::CV, "lowered function return and halt-check see the join dirty mask");
    }
    Check(found_phi && plan.ExitDirty() == F::CV && plan.Stats().opaque == 0,
          "real FunctionPlan metadata is accepted without opaque or missing CFG paths");
}

// Independent finite-state oracle explores (instruction, six-bit dirty set)
// directly instead of solving a fixed point. It catches a dirty bit lost on
// an edge, backedge or publication and distinguishes unreachable instructions.
void RandomizedCFGOracle() {
    std::mt19937 rng{0x466c6167};
    constexpr u32 nodes = 10;
    const std::array<F, 6> flags{F::Carry, F::Overflow, F::Zero, F::Negate, F::Parity, F::AuxiliaryCarry};
    for (u32 sample = 0; sample < 96; ++sample) {
        Builder b;
        auto* value = b.Constant();
        std::array<I*, nodes> labels{}, actions{}, branches{};
        std::array<u32, nodes> action{}, target{};
        std::array<bool, nodes> fallthrough{};
        for (u32 n = 0; n < nodes; ++n) {
            labels[n] = b.Label(); action[n] = rng() % 7;
            if (action[n] == 0) actions[n] = b.Add(O::SetCarry, T::VOID, V{value});
            else if (action[n] == 1) actions[n] = b.Add(O::SetOverflow, T::VOID, V{value});
            else if (action[n] == 2) actions[n] = b.Add(O::ClearFlags, T::VOID, F::Zero);
            else if (action[n] == 3) actions[n] = b.Add(O::CallLambda, T::VOID);
            else if (action[n] == 4) actions[n] = b.Add(O::VecAdd, T::V128);
            else if (action[n] == 5) actions[n] = b.Add(static_cast<O>(0x7fff), T::VOID);
            else actions[n] = b.Add(O::Nop, T::VOID);
            branches[n] = b.Add(n % 2 ? O::NotGoto : O::Goto, T::VOID, V{value});
            target[n] = rng() % nodes; fallthrough[n] = (rng() & 1) != 0;
        }
        b.Finish();
        la::FlagsPlan::Targets targets;
        la::FlagsPlan::Stops stops;
        for (u32 n = 0; n < nodes; ++n) {
            targets.emplace(branches[n], labels[target[n]]);
            if (!fallthrough[n]) stops.insert(branches[n]);
        }
        const auto plan = la::FlagsPlan::Build(&b.block, targets, stops);
        std::array<std::array<bool, 64>, nodes> seen{};
        std::array<u32, nodes> in{}, out{};
        std::queue<std::pair<u32, u32>> queue;
        queue.emplace(0, 0); seen[0][0] = true;
        while (!queue.empty()) {
            const auto [n, dirty] = queue.front(); queue.pop();
            in[n] |= dirty;
            u32 next = dirty;
            if (action[n] < 3) next |= 1u << action[n];
            else if (action[n] == 3) next = 0;
            else if (action[n] == 5) next = 63;
            out[n] |= next;
            const auto visit = [&](u32 target_node) {
                if (!seen[target_node][next]) { seen[target_node][next] = true; queue.emplace(target_node, next); }
            };
            visit(target[n]);
            if (fallthrough[n] && n + 1 < nodes) visit(n + 1);
        }
        const auto convert = [&](u32 compact) {
            F result = F::None;
            for (u32 bit = 0; bit < flags.size(); ++bit) if (compact & (1u << bit)) result |= flags[bit];
            return result;
        };
        for (u32 n = 0; n < nodes; ++n) {
            const bool reached = std::any_of(seen[n].begin(), seen[n].end(), [](bool found) { return found; });
            Check(plan.Reachable(actions[n]) == reached, "random CFG reachability matches the explicit path oracle");
            if (reached) {
                Check(plan.At(actions[n]) == convert(in[n]), "random CFG incoming dirty union matches the path oracle");
                Check(plan.After(actions[n]) == convert(out[n]), "random CFG outgoing dirty union matches the path oracle");
                Check(plan.At(branches[n]) == convert(out[n]), "random CFG branch publication preserves dirty bits");
            } else Check(plan.At(actions[n]) == F::All, "unreachable random CFG cannot justify a clean publication");
        }
    }
}
}  // namespace

void FlagsPlans() {
    const auto begin = checks;
    StraightPublicationAndDemand(); DiamondsPhisAndBackedges(); BoundariesUnreachableAndOpaque();
    LoweredFunctionCFG(); RandomizedCFGOracle();
    std::cout << "PASS software flags CFG dirty/demand plans " << checks - begin << " checks\n";
}

}  // namespace swift::tests::loongarch64
