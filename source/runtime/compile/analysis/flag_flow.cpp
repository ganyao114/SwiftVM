#include "flag_flow.h"

#include <algorithm>
#include <functional>
#include <type_traits>

#include "effects.h"
#include "runtime/common/variant_util.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {
using namespace ir;

FlagFlow::FlagFlow(const Block* block) : single_block(block) { ASSERT(block); }

FlagFlow::FlagFlow(const HIRFunction* owner) : function(owner) {
    ASSERT(owner);
    states.resize(owner->MaxBlockCount());
    for (const auto* block : owner->GetHIRBlocks())
        if (block) states[block->GetOrderId()].block = block;
}

const FlagFlow::FunctionState* FlagFlow::FindState(const HIRBlock* block) const {
    if (!block) return nullptr;
    const auto index = block->GetOrderId();
    if (index >= states.size() || states[index].block != block) return nullptr;
    return &states[index];
}

std::optional<Flags> FlagFlow::LiveIn(const HIRBlock* block) const {
    const auto* state = FindState(block);
    return state ? std::optional{state->live_in} : std::nullopt;
}

namespace {
Flags TerminalLiveOut(const Terminal& terminal, const HIRBlock* block, const FlagFlow& flow) {
    return VisitVariant<Flags>(terminal, [&](const auto& term) -> Flags {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, terminal::LinkBlock> ||
                      std::is_same_v<T, terminal::LinkBlockFast>) {
            const HIRBlock* successor{};
            for (const auto* candidate : block->GetSuccessors()) {
                if (candidate && candidate->GetBlock()->GetStartLocation() == term.next) {
                    successor = candidate;
                    break;
                }
            }
            const auto demand = flow.LiveIn(successor);
            if (!demand || successor->GetInstList().empty() || !successor->GetBlock()->HasTerminal())
                return Flags::All;
            return *demand;
        } else if constexpr (std::is_same_v<T, terminal::If>) {
            return TerminalLiveOut(term.then_, block, flow) | TerminalLiveOut(term.else_, block, flow);
        } else if constexpr (std::is_same_v<T, terminal::Switch>) {
            Flags live{Flags::None};
            for (const auto& arm : term.cases) live |= TerminalLiveOut(arm.then, block, flow);
            return live;
        } else {
            // Preserve observation boundaries, including CheckHalt/Condition
            // even when one of their children names an internal block.
            return Flags::All;
        }
    });
}
}  // namespace

Flags FlagFlow::LiveOut(const HIRBlock* block) const {
    if (!FindState(block)) return Flags::All;
    const auto* ir = block->GetBlock();
    if (ir->GetInstList().empty() || !ir->HasTerminal()) return Flags::All;
    return TerminalLiveOut(ir->GetTerminal(), block, *this);
}

void FlagFlow::ForgetLabels() {
    if (++epoch) return;
    // Equality alone identifies snapshots from this walk, including a clear
    // at a carry barrier. Avoid reviving an old snapshot after wraparound.
    for (auto& label : labels) label.epoch = 0;
    epoch = 1;
}

void FlagFlow::BeginBlock(const Block* block) {
    ASSERT(block && (!single_block || single_block == block));
    if (current_block != block) {
        current_block = block;
        labels.clear();
        labels_prepared = false;
    }
    ForgetLabels();
}

void FlagFlow::PrepareLabels() {
    if (labels_prepared) return;
    ASSERT(current_block);
    for (const auto& inst : current_block->GetInstList())
        if (inst.GetOp() == OpCode::BindLabel)
            labels.push_back({inst.GetArg<Value>(0).Def()});
    const auto less = [](const LabelState& left, const LabelState& right) {
        return std::less<const Inst*>{}(left.target, right.target);
    };
    // Keep this sparse index in place with bounded stack and O(L log L)
    // worst-case sorting. Its records carry two full flag masks.
    std::make_heap(labels.begin(), labels.end(), less);
    std::sort_heap(labels.begin(), labels.end(), less);
    labels.erase(std::unique(labels.begin(), labels.end(), [](const auto& left, const auto& right) {
                     return left.target == right.target;
                 }), labels.end());
    labels_prepared = true;
}

const FlagFlow::LabelState* FlagFlow::FindLabel(const Inst* target) const {
    if (!labels_prepared) return nullptr;
    const auto found = std::lower_bound(labels.begin(), labels.end(), target,
            [](const LabelState& label, const Inst* key) {
                return std::less<const Inst*>{}(label.target, key);
            });
    return found != labels.end() && found->target == target ? &*found : nullptr;
}

void FlagFlow::RememberTransfer(const Inst* target, Transfer value, bool merge) {
    PrepareLabels();
    const auto* found = FindLabel(target);
    // This pass does not add bindings. An unexpected new target must not
    // supply a false proof: its lookup remains unknown and preserves flags.
    if (!found) return;
    auto& label = labels[found - labels.data()];
    if (merge && label.epoch == epoch) label.value.generated |= value.generated;
    else label.value = value;
    label.epoch = epoch;
}

void FlagFlow::RememberLabel(const Inst* target, Flags needed) {
    RememberTransfer(target, {needed, Flags::None}, true);
}

std::optional<Flags> FlagFlow::LabelDemand(const Inst* target) const {
    const auto* label = FindLabel(target);
    if (!label || label->epoch != epoch) return std::nullopt;
    return label->value.generated;
}

FlagFlow::Transfer FlagFlow::Summarize(const Block* block) {
    BeginBlock(block);
    Transfer summary;
    for (auto it = block->GetInstList().rbegin(); it != block->GetInstList().rend(); ++it) {
        const auto& inst = *it;
        const auto flags = InstructionFlagAccess(inst);
        summary.generated &= ~flags.written;
        summary.preserved &= ~flags.written;
        summary.generated |= flags.read;
        switch (inst.GetOp()) {
            case OpCode::BindLabel:
                RememberTransfer(inst.GetArg<Value>(0).Def(), summary, false);
                break;
            case OpCode::Goto:
            case OpCode::NotGoto:
                if (const auto* target = FindLabel(&inst); target && target->epoch == epoch) {
                    summary.generated |= target->value.generated;
                    summary.preserved |= target->value.preserved;
                } else {
                    summary = {Flags::All, Flags::All};
                }
                break;
            default: break;
        }
    }
    return summary;
}

void FlagFlow::ComputeLiveIn() {
    ASSERT(function);
    for (auto& state : states) state.live_in = Flags::None;
    for (const auto& block : function->GetHIRBlocksRPO()) {
        auto& state = states[block.GetOrderId()];
        ASSERT(state.block == &block);
        state.transfer = Summarize(block.GetBlock());
    }
    bool changed = true;
    while (changed) {
        changed = false;
        const auto& rpo = function->GetHIRBlocksRPO();
        for (auto it = rpo.rbegin(); it != rpo.rend(); ++it) {
            auto& state = states[it->GetOrderId()];
            const auto next = state.transfer.Apply(LiveOut(&*it));
            if (next != state.live_in) {
                state.live_in = next;
                changed = true;
            }
        }
    }
}

}  // namespace swift::runtime::compile
