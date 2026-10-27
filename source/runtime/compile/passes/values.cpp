#include "passes.h"
#include "runtime/compile/workspace.h"
#include "value_worklist.h"

#include "runtime/ir/hir_builder.h"

#include <array>
#include <optional>
#include <unordered_map>

namespace swift::runtime::compile {
using namespace ir;

namespace {

// Visit before overwriting the oldest slot: a definition exactly eight IR
// instructions away is still eligible. Reusing a duplicate does not install
// a new anchor, so it cannot extend the original definition's reuse window.
class ScalarImmWindow final {
public:
    const Inst* Visit(const Inst& inst) {
        const bool constant = inst.GetOp() == OpCode::LoadImm;
        const Inst* previous{};
        if (constant) {
            const auto bits = inst.GetArg<Imm>(0).Get();
            const auto type = inst.ReturnType();
            for (const auto* entry : entries)
                // Both back ends size the destination from the return type;
                // equal bits in U32 and U64 are different constants.
                if (entry && entry->ReturnType() == type && entry->GetArg<Imm>(0).Get() == bits) {
                    previous = entry;
                    break;
                }
        }
        if (constant && !previous) {
            entries[next] = &inst;
            remaining = kConstantReuseWindow;
        } else {
            entries[next] = nullptr;
            if (remaining) --remaining;
        }
        next = (next + 1) % kConstantReuseWindow;
        return previous;
    }
    [[nodiscard]] bool Empty() const { return remaining == 0; }

private:
    std::array<const Inst*, kConstantReuseWindow> entries{};
    u32 next{}, remaining{};
};

struct VecKey {
    u64 low;
    u64 high;

    bool operator==(const VecKey& rhs) const {
        return low == rhs.low && high == rhs.high;
    }
};

struct VecKeyHash {
    std::size_t operator()(const VecKey& key) const {
        return std::hash<u64>{}(key.low) ^
               (std::hash<u64>{}(key.high) + 0x9E3779B97F4A7C15ull);
    }
};

bool KeepFlagResultForPublication(ValueWorklist& work, const Inst& projection, Value input) {
    if (projection.GetOp() != OpCode::ZeroExtend32To64 || !input.Defined() ||
        GetValueSizeByte(input.Type()) != sizeof(u32) ||
        GetValueSizeByte(input.Def()->ReturnType()) != sizeof(u32) ||
        !input.Def()->HasPseudoOperations())
        return false;
    const auto* publication = work.SingleConsumer(&projection);
    // The live flags producer already computes the value. Its widening edge
    // lets a whole-register publication share that definition; a separate
    // constant can require another write and a flags-result capture.
    return publication && publication->GetOp() == OpCode::SetHostGPR &&
           publication->GetArg<Value>(0).Def() == &projection &&
           GetValueSizeByte(publication->GetArg<Value>(0).Type()) == sizeof(u64) &&
           publication->GetArg<Imm>(2).Get() == 0;
}
}  // namespace

void SimplifyValue(RewriteContext& rewrite,
                   ValueWorklist& work,
                   Block* block,
                   const Inst* candidate,
                   const FeatureSet& features,
                   const BuilderCapabilities& target) {
    if (!features.int_imm_fold) return;
    const auto& inst = *candidate;
    const auto op = inst.GetOp();
    if ((IsWidthOperation(op) || IsConditionalValueOperation(op)) && !inst.HasPseudoOperations()) {
        const auto input = inst.GetArg<Value>(0);
        const auto folded = IsWidthOperation(op)
                ? FoldWidthValue(op,
                               inst.ReturnType(),
                               input,
                               FlagsIntent::None(),
                               op == OpCode::BitExtract ? inst.GetArg<Imm>(1).Get() : 0,
                               op == OpCode::BitExtract ? inst.GetArg<Imm>(2).Get() : 0)
                : FoldConditionalValue(inst, FlagsIntent::None());
        if (folded && !KeepFlagResultForPublication(work, inst, input)) {
            const auto type = inst.ReturnType();
            rewrite.Change(block, &inst, [&](Inst& writer) {
                writer.DestroyArgs();
                writer.SetInst(OpCode::LoadImm, *folded);
                writer.SetReturn(type);
            });
        }
        return;
    }
    if (op != OpCode::Add && op != OpCode::Sub && op != OpCode::And && op != OpCode::Or &&
        op != OpCode::Xor)
        return;

    // Keep the short source window and single-use restriction before CSE.
    // Rewrites retain the producer identity and all attached flag pseudos.
    if (op == OpCode::Sub) {
        const auto left = inst.GetArg<Value>(0);
        const auto source = ZeroMinusOperand(left, inst.GetArg<Operand>(1));
        if (source && work.Nearby(left.Def(), &inst, kIntegerImmediateUseWindow) &&
            left.Def()->GetUses(false) == 1) {
            const auto type = inst.ReturnType();
            rewrite.Change(block, &inst, [&](Inst& writer) {
                writer.DestroyArgs();
                writer.SetInst(OpCode::Neg, *source);
                writer.SetReturn(type);
            });
            return;
        }
    }

    const auto operand = inst.GetArg<Operand>(1);
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue() ||
        operand.GetOp() != OperandPlus)
        return;
    const auto* load = operand.GetLeft().value.Def();
    if (!work.Nearby(load, &inst, kIntegerImmediateUseWindow) || load->GetOp() != OpCode::LoadImm ||
        load->GetUses(false) != 1)
        return;

    const bool narrow_add_sub = (op == OpCode::Add || op == OpCode::Sub) &&
                                GetValueSizeByte(inst.ReturnType()) <= sizeof(u16);
    const auto flags = narrow_add_sub ? FlagsIntent::Declared(work.RequestedFlags(&inst))
                                      : FlagsIntent::Unknown();
    if (const auto folded =
                FoldIntegerImmediate(op, inst.ReturnType(), load->GetArg<Imm>(0), flags, target))
        rewrite.SetArg(block, &inst, 1, Operand{*folded});
}

namespace {

template <class Replacement> void RewriteConstantArguments(RewriteContext& rewrite,
                                                           Block* block,
                                                           const Inst& inst,
                                                           Replacement&& replacement) {
    // A pseudo's arg 0 pairs it with its producer. Local branch arguments
    // retain their definitions, just as in the initial ordered walk.
    if (inst.IsPseudoOperation() || IsConstantReuseBoundary(inst.GetOp())) return;
    for (int i = 0; i < Inst::max_args; ++i) {
        const auto& arg = inst.ArgAt(i);
        if (arg.IsValue()) {
            const auto def = arg.Get<Value>().Def();
            if (!def) continue;
            if (const auto* target = replacement(def)) {
                rewrite.SetArg(block, &inst, i, arg.Get<Value>().WithDefinition(target));
                ASSERT(inst.HasValidArguments());
            }
        } else if (arg.IsLambda()) {
            auto lambda = arg.Get<Lambda>();
            if (!lambda.IsValue()) continue;
            const auto def = lambda.GetValue().Def();
            if (!def) continue;
            if (const auto* target = replacement(def)) {
                lambda.GetValue() = lambda.GetValue().WithDefinition(target);
                rewrite.SetArg(block, &inst, i, lambda);
                ASSERT(inst.HasValidArguments());
            }
        } else if (arg.IsParams()) {
            const auto params = arg.Get<Params>();
            bool changed = false;
            for (const auto& param : params) {
                if (param.data.IsValue() && replacement(param.data.value.Def())) {
                    changed = true;
                    break;
                }
            }
            if (!changed) continue;
            Params rewritten;
            for (const auto& param : params) {
                if (param.data.IsValue()) {
                    auto value = param.data.value;
                    if (const auto* target = replacement(value.Def()))
                        value = value.WithDefinition(target);
                    rewritten.Push(value);
                } else {
                    rewritten.Push(param.data.imm);
                }
            }
            rewrite.SetArg(block, &inst, i, std::move(rewritten));
            ASSERT(inst.HasValidArguments());
        }
    }
}

template <class Visit, class Share>
void DedupConstants(Block* block, const FeatureSet& features, Visit&& visit, Share&& share) {
    // Bisect switch, mirroring SVM_UNIFORM_DSE.
    const bool imm_off = !features.const_cse;
    // No scalar payload initialization until the first enabled LoadImm.
    std::optional<ScalarImmWindow> canonical;
    // These two constants exist specifically for the vector lowering gates.
    // Unlike scalar LoadImm, their reuse is block-wide: one 16-byte table or
    // one zero vector is worth keeping live across the GHASH/AES straight-line
    // body.  Control-flow pseudo labels clear them below, so a definition is
    // never reused from a path which may not have executed.
    std::unordered_map<VecKey, Value, VecKeyHash> vec_canonical;
    Value zero_canonical{};
    for (auto& inst : block->GetInstList()) {
        switch (inst.GetOp()) {
            case OpCode::Goto:
            case OpCode::NotGoto:
            case OpCode::BindLabel:
                // A constant defined on one side of an intra-block branch is
                // not available on the other.  Drop every fact; instructions
                // already rewritten stay rewritten -- they were rewritten to a
                // definition that dominated them.
                canonical.reset();
                vec_canonical.clear();
                zero_canonical = {};
                continue;
            default:
                break;
        }
        visit(inst);
        if (!imm_off && inst.GetOp() == OpCode::LoadImm && !canonical) canonical.emplace();
        if (canonical) {
            if (const auto* shared = canonical->Visit(inst)) {
                share(&inst, shared);
            }
            if (canonical->Empty()) canonical.reset();
        }
        if (inst.GetOp() == OpCode::VecLoadConst) {
            const VecKey key{
                    inst.GetArg<Imm>(0).Get(),
                    inst.GetArg<Imm>(1).Get(),
            };
            auto [it, inserted] =
                    vec_canonical.try_emplace(key, Value{&inst});
            if (!inserted) {
                share(&inst, it->second.Def());
            }
            continue;
        }
        if (inst.GetOp() == OpCode::VecSharedZero) {
            if (zero_canonical.Defined()) {
                share(&inst, zero_canonical.Def());
            } else {
                zero_canonical = Value{&inst};
            }
            continue;
        }
    }
}

void ShareBlockConstants(RewriteContext& rewrite,
                         Block* block,
                         const FeatureSet& features,
                         ValueWorklist* work) {
    if (work) {
        // The active epoch already owns stable rows and a mutable use view.
        // Update actual later users, without recording another replacement
        // table or examining every argument of the remaining instructions.
        DedupConstants(
                block,
                features,
                [](const Inst&) {},
                [&](const Inst* from, const Inst* to) {
                    work->ReplaceConstantUses(block, from, to);
                });
        return;
    }
    // Constant-only units keep the ordered lightweight path. They need no
    // roster/reverse graph, and distinct scalar keys allocate no storage.
    std::unordered_map<const Inst*, Value> replacement;
    DedupConstants(
            block,
            features,
            [&](const Inst& inst) {
                if (!replacement.empty())
                    RewriteConstantArguments(
                            rewrite, block, inst, [&](const Inst* definition) -> const Inst* {
                                const auto found = replacement.find(definition);
                                return found == replacement.end() ? nullptr : found->second.Def();
                            });
            },
            [&](const Inst* from, const Inst* to) { replacement.emplace(from, Value{to}); });
}

}  // namespace

void ShareConstants(RewriteContext& rewrite, const FeatureSet& features) {
    if (rewrite.IsSingleBlock())
        ShareBlockConstants(rewrite, rewrite.Blocks().front(), features, rewrite.values);
    else
        for (auto& current : rewrite.Function()->GetHIRBlocksRPO())
            ShareBlockConstants(rewrite,
                                rewrite.Function()->EditBlock(&current).GetBlock(),
                                features,
                                rewrite.values);
}

void RetargetConstantUser(
        RewriteContext& rewrite, Block* block, const Inst* user, const Inst* from, const Inst* to) {
    RewriteConstantArguments(rewrite, block, *user, [&](const Inst* definition) -> const Inst* {
        return definition == from ? to : nullptr;
    });
}

void SimplifyConstant(RewriteContext&,
                      ValueWorklist& work,
                      Block* block,
                      const Inst* inst,
                      const FeatureSet& features) {
    if (!features.const_cse || inst->GetOp() != OpCode::LoadImm || work.SharedConstant(inst))
        return;
    const auto& list = block->GetInstList();
    auto cursor = list.iterator_to(*inst);
    const Inst* shared{};
    for (u32 distance = 0; cursor != list.begin() && distance < kConstantReuseWindow; ++distance) {
        --cursor;
        if (IsConstantReuseBoundary(cursor->GetOp())) break;
        if (cursor->GetOp() == OpCode::LoadImm && !work.SharedConstant(&*cursor) &&
            cursor->ReturnType() == inst->ReturnType() &&
            cursor->GetArg<Imm>(0).Get() == inst->GetArg<Imm>(0).Get())
            shared = &*cursor;
    }
    if (shared) work.ReplaceConstantUses(block, inst, shared);
}

void FoldValues(RewriteContext& rewrite, const FeatureSet& features,
                const BuilderCapabilities& target) {
    std::optional<ValueWorklist> cleanup;
    BeginValueCleanup(cleanup, rewrite, features, target, ValueCleanup::Values);
}

}  // namespace swift::runtime::compile
