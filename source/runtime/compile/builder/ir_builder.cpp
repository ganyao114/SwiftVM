#include "ir_builder.h"

#include <algorithm>

namespace swift::runtime::compile {
using namespace ir;


HIRBuilder::ElseThen IrBuilder::If(const terminal::If& if_) {
    CommitGuestInstruction();
    end_decode = true;
    if (uniform_builder) {
        UniformInvalidateAll();
        uniform_forwarding.Reset();
    }
    if (hir_builder) {
        return hir_builder->If(if_);
    } else {
        ir_block->SetTerminal(terminal::Terminal{if_});
        return {};
    }
}

HIRBlock* IrBuilder::LinkBlock(const terminal::LinkBlock& block) {
    CommitGuestInstruction();
    end_decode = true;
    if (uniform_builder) {
        UniformInvalidateAll();
        uniform_forwarding.Reset();
    }
    if (hir_builder) {
        return hir_builder->LinkBlock(block);
    } else {
        ir_block->SetTerminal(terminal::Terminal{block});
        return {};
    }
}

void IrBuilder::ExternalLinkBlock(const terminal::ExternalLinkBlock& link) {
    CommitGuestInstruction();
    end_decode = true;
    if (uniform_builder) {
        UniformInvalidateAll();
        uniform_forwarding.Reset();
    }
    if (hir_builder) {
        hir_builder->ExternalLinkBlock(link);
    } else {
        ir_block->SetTerminal(terminal::Terminal{link});
    }
}

void IrBuilder::RegisterExternalDirectLink(Location target) {
    if (hir_builder) {
        hir_builder->RegisterExternalDirectLink(target);
    }
}

void IrBuilder::ReturnToDispatcher() {
    CommitGuestInstruction();
    end_decode = true;
    if (uniform_builder) {
        UniformInvalidateAll();
        uniform_forwarding.Reset();
    }
    if (hir_builder) {
        hir_builder->ReturnToDispatcher();
    } else {
        ir_block->SetTerminal(terminal::ReturnToDispatch{});
    }
}

void IrBuilder::ReturnToHost() {
    CommitGuestInstruction();
    end_decode = true;
    if (uniform_builder) {
        UniformInvalidateAll();
        uniform_forwarding.Reset();
    }
    if (hir_builder) {
        hir_builder->ReturnToHost();
    } else {
        ir_block->SetTerminal(terminal::ReturnToHost{});
    }
}

void IrBuilder::Return() {
    CommitGuestInstruction();
    end_decode = true;
    if (uniform_builder) {
        UniformInvalidateAll();
        uniform_forwarding.Reset();
    }
    if (hir_builder) {
        hir_builder->Return();
    } else {
        ir_block->SetTerminal(terminal::PopRSBHint{});
    }
}

void IrBuilder::RegisterCallReturn(Location location) {
    if (hir_builder) {
        hir_builder->RegisterCallReturn(location);
    }
}

bool IrBuilder::EndCommit() const {
    return end_decode;
}

bool IrBuilder::HasFunctionBlock(Location location) const {
    return hir_builder && hir_builder->HasCurrentFunctionBlock(location);
}

void IrBuilder::AfterAppend(Inst* inst, FlagsIntent flags, bool typed) {
    if (!inst) return;
    if (uniform_builder) UniformAfterAppend(inst);
    if (guest_instruction) ObserveFlagOutput(*inst);
    if (typed) {
        const auto op = inst->GetOp();
        const auto position = bound_index ? bound_index - 1 : 0;
        if (guest_instruction && (IsWidthOperation(op) || op == OpCode::Add ||
                op == OpCode::Sub || op == OpCode::And || op == OpCode::Or ||
                op == OpCode::Xor)) {
            pending_values.push_back({inst, flags, position});
        } else {
            FoldImmediate(inst, flags, position);
            FoldValue(inst, flags, position);
        }
    }
}

void IrBuilder::FoldImmediate(Inst* inst, FlagsIntent flags, u32 position) {
    if (!uniform_features.int_imm_fold || !bound_index) return;
    const auto op = inst->GetOp();
    if (op != OpCode::Add && op != OpCode::Sub && op != OpCode::And &&
        op != OpCode::Or && op != OpCode::Xor) return;
    const auto within_window = [&](const Inst* producer) {
        return HasImmediateSource(producer, position);
    };
    if (op == OpCode::Sub && within_window(inst->GetArg<Value>(0).Def())) {
        if (const auto source = ZeroMinusOperand(inst->GetArg<Value>(0), inst->GetArg<Operand>(1))) {
            const auto type = inst->ReturnType();
            RewriteInst(inst, [&] {
                inst->DestroyArgs();
                inst->SetInst(OpCode::Neg, *source);
                inst->SetReturn(type);
            });
            return;
        }
    }
    const auto operand = inst->GetArg<Operand>(1);
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue() ||
        operand.GetOp() != OperandPlus) return;
    const auto* load = operand.GetLeft().value.Def();
    if (!within_window(load) || load->GetOp() != OpCode::LoadImm) return;
    if (const auto folded = FoldIntegerImmediate(
                op, inst->ReturnType(), load->GetArg<Imm>(0), flags, target)) {
        RewriteInst(inst, [&] { inst->SetArg(1, Operand{*folded}); });
    }
}

void IrBuilder::FoldValue(Inst* inst, FlagsIntent flags, u32 position) {
    if (inst->HasPseudoOperations()) return;
    const auto op = inst->GetOp();
    if (!IsWidthOperation(op) && !IsConditionalValueOperation(op)) return;
    // These value operations have no flags output in valid IR. Neg and other
    // potential producers are deliberately absent from this list.
    if (flags.IsUnknown()) flags = FlagsIntent::None();
    const auto folded = IsWidthOperation(op)
            ? FoldWidthValue(op, inst->ReturnType(), inst->GetArg<Value>(0), flags,
                             op == OpCode::BitExtract ? inst->GetArg<Imm>(1).Get() : 0,
                             op == OpCode::BitExtract ? inst->GetArg<Imm>(2).Get() : 0)
            : FoldConditionalValue(*inst, flags);
    if (!folded) return;
    const auto type = inst->ReturnType();
    RewriteInst(inst, [&] {
        inst->DestroyArgs();
        inst->SetInst(OpCode::LoadImm, *folded);
        inst->SetReturn(type);
    });
    if (bound_index) RecordImmediateSource(inst, position);
}

void IrBuilder::RecordImmediateSource(const Inst* inst, u32 position) {
    // Until the first deferred value, no query needs an earlier position.
    // Once one is queued, keep its history until the instruction commits.
    if (!guest_instruction || pending_values.empty()) PruneImmediateSources();
    if (immediate_sources.empty() || immediate_sources.back().position <= position) {
        immediate_sources.push_back({inst, position});
        return;
    }
    // A deferred width fold can turn an earlier node into LoadImm after later
    // constants were appended. Keep historical queries ordered by append time.
    const auto next = std::upper_bound(
            immediate_sources.begin(),
            immediate_sources.end(),
            position,
            [](u32 at, const ImmediateSource& source) { return at < source.position; });
    immediate_sources.insert(next, {inst, position});
}

void IrBuilder::PruneImmediateSources() {
    // With capabilities temporarily disabled, a later query still uses the
    // last tracked position (bound_index - 1). Retain that full window too.
    if (bound_index <= kIntegerImmediateUseWindow + 1 || immediate_sources.empty()) return;
    const auto first_position = bound_index - kIntegerImmediateUseWindow - 1;
    if (immediate_sources.front().position >= first_position) return;
    const auto first = std::lower_bound(
            immediate_sources.begin(),
            immediate_sources.end(),
            first_position,
            [](const ImmediateSource& source, u32 at) { return source.position < at; });
    immediate_sources.erase(immediate_sources.begin(), first);
}

bool IrBuilder::HasImmediateSource(const Inst* inst, u32 position) const {
    if (!inst) return false;
    auto next = std::upper_bound(
            immediate_sources.begin(),
            immediate_sources.end(),
            position,
            [](u32 at, const ImmediateSource& source) { return at < source.position; });
    while (next != immediate_sources.begin()) {
        const auto& source = *--next;
        if (position - source.position > kIntegerImmediateUseWindow) break;
        if (source.inst == inst) return true;
    }
    return false;
}

}  // namespace swift::runtime::compile
