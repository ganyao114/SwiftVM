#include "memory_lowering_plan.h"
#include "gpr_block_plan.h"
#include "fixed_gpr_constraints.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include <algorithm>
#include <limits>
#include <optional>

namespace swift::runtime::backend::arm64 {
namespace {
using namespace ir;
bool IsMemory(OpCode op) {
    return op == OpCode::LoadMemory || op == OpCode::StoreMemory ||
           op == OpCode::LoadMemoryTSO || op == OpCode::StoreMemoryTSO;
}
bool IsLoad(OpCode op) { return op == OpCode::LoadMemory || op == OpCode::LoadMemoryTSO; }
u16 RegisterAt(const RegAlloc& a, Value v, u32 point) {
    const auto location = a.GPRLocationAt(v, point, RegAlloc::GPRView::WholeValue);
    return location.kind == RegAlloc::GPR ? location.slot : UINT16_MAX;
}
bool Shares(const RegAlloc& a, Value left, Value right, u32 point) {
    if (a.PreservedGPRDefinitionAt(left.Id()) || a.PreservedGPRDefinitionAt(right.Id())) {
        const auto reg = RegisterAt(a, left, point);
        return reg < 31 && reg == RegisterAt(a, right, point);
    }
    return a.ValueType(left) == RegAlloc::GPR && a.ValueType(right) == RegAlloc::GPR &&
           a.ValueGPR(left).id == a.ValueGPR(right).id;
}
bool WritebackAllowed(Inst& memory, ValueType type, MemoryLoweringOptions options) {
    if (options.biased || type == ValueType::V128) return false;
    const bool tso = memory.GetOp() == OpCode::LoadMemoryTSO || memory.GetOp() == OpCode::StoreMemoryTSO;
    const bool scalar = type < ValueType::V8 || type > ValueType::V256;
    // A value address is dynamically aligned by the RCpc path, which needs
    // a bare address. Immediate addresses cannot be writeback candidates.
    return !tso || !options.rcpc || !scalar;
}
}

MemoryLoweringPlan BuildMemoryLoweringPlan(Block* block, const RegAlloc& allocation,
        const FeatureSet& features, const GPRBlockPlan& gpr, const GPRResidenceFacts& residences,
        MemoryLoweringOptions options, const BlockAnalysisIndex* analysis) {
    MemoryLoweringPlan plan;
    std::optional<BlockAnalysisIndex> facts;
    const auto& index = [&]() -> const BlockAnalysisIndex& {
        if (analysis) return *analysis;
        if (!facts) { facts.emplace(); facts->Build(block); }
        return *facts;
    };
    const auto local = [&](Inst* node) {
        return index().Contains(node);
    };
    for (auto& memory : block->GetInstList()) {
        if (!IsMemory(memory.GetOp())) continue;
        const bool load = IsLoad(memory.GetOp());
        const auto type = load ? memory.ReturnType() : memory.GetArg<Value>(1).Type();
        const auto operand = memory.GetArg<Operand>(0);
        auto* address = operand.GetRight().Null() && operand.GetLeft().IsValue()
                ? operand.GetLeft().value.Def() : nullptr;

        if (address && address->GetOp() == OpCode::GetOperand && address->GetUses() == 1 &&
            (options.narrow_fuse || options.address_tie) && !gpr.addresses.contains(address)) {
            const auto source = address->GetArg<Operand>(0);
            const auto result = Value{address};
            if (source.GetRight().Null() && source.GetLeft().IsValue() &&
                result.Type() == ValueType::U64 && source.GetLeft().value.Type() == ValueType::U64 &&
                Shares(allocation, result, source.GetLeft().value, address->Id()) &&
                RegisterAt(allocation, result, address->Id()) == RegisterAt(allocation, result, memory.Id()) &&
                local(address)) {
                plan.address_aliases.push_back(address);
                plan.covered.push_back(address);
            }
        }

        if (address && options.rcpc && !options.biased &&
            (memory.GetOp() == OpCode::LoadMemoryTSO || memory.GetOp() == OpCode::StoreMemoryTSO) &&
            (type < ValueType::V8 || type > ValueType::V256) &&
            (address->GetOp() == OpCode::Add || address->GetOp() == OpCode::Sub) &&
            address->GetUses(false) == 1 && !address->HasPseudoOperations() &&
            address->ReturnType() == ValueType::U64 && local(address) &&
            !gpr.addresses.contains(address) && !gpr.load_updates.contains(&memory) &&
            !gpr.load_update_instructions.contains(&memory) &&
            !gpr.preindex_updates.contains(address) && !gpr.biased_updates.contains(address)) {
            const auto base = address->GetArg<Value>(0);
            const auto delta = address->GetArg<Operand>(1);
            // The base register must still hold the base value at the
            // access: either it is pinned there, or nothing between the
            // EA definition and the access can have taken its register.
            const bool base_stable =
                    allocation.GPRLocationAt(base, memory.Id(), RegAlloc::GPRView::WholeValue).pinned ||
                    index().Between(address->Id(), memory.Id()).empty();
            if (base.Type() == ValueType::U64 && base.Def() != address && base_stable &&
                delta.GetRight().Null() && delta.GetLeft().IsImm() &&
                RegisterAt(allocation, base, memory.Id()) < 31) {
                const auto imm = delta.GetLeft().imm.GetSigned();
                if (!(address->GetOp() == OpCode::Sub && imm == std::numeric_limits<s64>::min())) {
                    const s64 offset = address->GetOp() == OpCode::Add ? imm : -imm;
                    if (offset >= -256 && offset <= 255) {
                        plan.offsets.emplace(&memory, MemoryOffsetAction{base, offset});
                        plan.covered.push_back(address);
                    }
                }
            }
        }

        if (address && address->GetUses() == 2 && WritebackAllowed(memory, type, options) &&
            !gpr.load_updates.contains(&memory) && !gpr.preindex_updates.contains(address) && local(address)) {
            const auto source = operand.GetLeft().value;
            const auto base = RegisterAt(allocation, source, memory.Id());
            const auto instructions = index().Instructions();
            auto found = std::lower_bound(instructions.begin(), instructions.end(), address->Id(),
                    [](const Inst* inst, u32 id) { return inst->Id() < id; });
            for (unsigned scanned = 0; found != instructions.end() && scanned < 3; ++found, ++scanned) {
                auto* update = *found;
                if (update->GetOp() != OpCode::Add && update->GetOp() != OpCode::Sub) continue;
                if (source.Type() != ValueType::U64 || update->ReturnType() != ValueType::U64 ||
                    update->Id() <= memory.Id() || update->GetArg<Value>(0) != source ||
                    update->HasPseudoOperations() || update->GetUses(false) == 0 || base >= 31 ||
                    RegisterAt(allocation, Value{update}, memory.Id()) != base ||
                    RegisterAt(allocation, Value{update}, update->Id()) != base) continue;
                const auto delta = update->GetArg<Operand>(1);
                if (!delta.GetRight().Null()) continue;
                MemoryWritebackAction action{update, base, 0, static_cast<u8>(GetValueSizeByte(type))};
                const auto left = delta.GetLeft();
                if (left.IsImm()) {
                    const auto imm = left.imm.GetSigned();
                    if (update->GetOp() == OpCode::Sub && imm == std::numeric_limits<s64>::min()) continue;
                    action.offset = update->GetOp() == OpCode::Add ? imm : -imm;
                    if (action.offset < -256 || action.offset > 255) continue;
                } else continue; // Scalar LDR/STR post-index encodes an immediate.
                // A register delta needs the ordinary Add after the access. In
                // particular, VIXL's register post-index operand is a SIMD
                // structure form, not a scalar load/store writeback recipe.
                bool safe = !allocation.GPRPreservationConflict(base, memory.Id(), update->Id());
                auto owner = source;
                while (owner.Defined() && owner.Def()->IsBitCastOperation())
                    owner = owner.Def()->GetArg<Value>(0);
                unsigned actual_uses = 0;
                if (!owner.Defined() || owner.Def()->GetUses(false) != index().DirectUseCount(owner.Def())) safe = false;
                else for (const auto& use : index().ResolvedUses(owner.Def())) {
                    if (use.consumer->IsBitCastOperation()) {
                        if (use.consumer->GetUses(false) != index().DirectUseCount(use.consumer)) safe = false;
                    } else {
                        if (use.consumer != &memory && use.consumer != update) safe = false;
                        actual_uses += use.count;
                    }
                }
                if (actual_uses != 2) safe = false;
                action.consumed_value = owner.Defined() ? owner.Id() : UINT32_MAX;
                for (auto* node : index().Between(memory.Id(), update->Id())) {
                    if (IsFixedGPRPublicationObserver(*node, features) ||
                        (FixedGPRClobbers(*node, features) & (1u << base))) safe = false;
                }
                if (load && RegisterAt(allocation, Value{&memory}, memory.Id()) == base) safe = false;
                // A scratch SSA update owns no guest state. A fixed home may
                // update early only when its publication immediately follows.
                if (allocation.GPRLocationAt(source, memory.Id(), RegAlloc::GPRView::WholeValue).pinned) {
                    const auto uses = index().Uses(update);
                    if (uses.size() != 1 || uses.front().count != 1) safe = false;
                    else {
                        auto* publication = uses.front().consumer;
                        const auto after_update = std::next(block->GetInstList().iterator_to(*update));
                        if (after_update == block->GetInstList().end() || after_update.operator->() != publication ||
                            publication->GetOp() != OpCode::SetHostGPR ||
                            publication->GetArg<Imm>(1).Get() != base || publication->GetArg<Imm>(2).Get() != 0)
                            safe = false;
                    }
                }
                if (safe) {
                    plan.writebacks.emplace(&memory, action);
                    plan.covered.push_back(update);
                }
                break;
            }
        }

        if (memory.GetOp() != OpCode::LoadMemory || !options.narrow_fuse ||
            IsFloatValueType(type) || GetValueSizeByte(type) > 2 || memory.GetUses() != 1) continue;
        const auto uses = index().Uses(&memory);
        if (uses.size() != 1 || uses.front().count != 1) continue;
        auto* extension = uses.front().consumer;
        if ((extension->GetOp() != OpCode::SignExtend && extension->GetOp() != OpCode::ZeroExtend32) ||
            extension->GetArg<Value>(0).Def() != &memory || extension->HasPseudoOperations() ||
            (GetValueSizeByte(extension->ReturnType()) != 4 && GetValueSizeByte(extension->ReturnType()) != 8)) continue;
        const auto value = Value{&memory};
        const auto target = Value{extension};
        const auto definition = allocation.GPRLocationAt(value, memory.Id(), RegAlloc::GPRView::Definition);
        const bool shared = Shares(allocation, value, target, memory.Id());
        const bool pinned_extension = residences.sign_extends.contains(extension);
        const auto next = std::next(block->GetInstList().iterator_to(memory));
        const auto result = RegisterAt(allocation, target, memory.Id());
        bool direct = next != block->GetInstList().end() && next.operator->() == extension &&
                      !definition.pinned && result < 31 &&
                      result == RegisterAt(allocation, target, extension->Id());
        if (!shared && !pinned_extension && !direct) continue;
        direct = direct && !shared && !pinned_extension;
        if (direct) {
            std::optional<u16> writeback_base;
            if (const auto update = gpr.load_updates.find(&memory); update != gpr.load_updates.end())
                writeback_base = update->second.target;
            else if (!options.biased && address) {
                if (const auto update = gpr.preindex_updates.find(address);
                    update != gpr.preindex_updates.end() && update->second.memory == &memory)
                    writeback_base = update->second.base.GetCode();
                else if (const auto update = plan.writebacks.find(&memory); update != plan.writebacks.end())
                    writeback_base = update->second.base;
            }
            if (writeback_base == result) continue;
        }
        plan.narrow_loads.emplace(&memory, NarrowLoadAction{extension, direct,
                extension->GetOp() == OpCode::SignExtend, static_cast<u8>(GetValueSizeByte(type)),
                static_cast<u8>(GetValueSizeByte(extension->ReturnType()))});
        plan.covered.push_back(extension);
    }
    return plan;
}
}
