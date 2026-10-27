#include <algorithm>
#include <limits>
#include <optional>
#include "lower_memory.h"
#include "runtime/backend/arm64/compile/constraints.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/compile/residences.h"
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::backend::arm64 {

namespace {
using namespace ir;
bool IsLoad(OpCode op) { return op == OpCode::LoadMemory || op == OpCode::LoadMemoryTSO; }
u16 RegisterAt(const compile::AllocationView& a, Value v, u32 point) {
    const auto location = a.GPRLocationAt(v, point, compile::AllocationView::GPRView::WholeValue);
    return location.kind == compile::AllocationView::GPR ? location.slot : UINT16_MAX;
}
bool Shares(const compile::AllocationView& a, Value left, Value right, u32 point) {
    if (a.PreservedGPRDefinitionAt(left.Id()) || a.PreservedGPRDefinitionAt(right.Id())) {
        const auto reg = RegisterAt(a, left, point);
        return reg < 31 && reg == RegisterAt(a, right, point);
    }
    return a.ValueType(left) == compile::AllocationView::GPR &&
           a.ValueType(right) == compile::AllocationView::GPR &&
           a.ValueGPR(left).id == a.ValueGPR(right).id;
}
bool WritebackAllowed(const Inst& memory, ValueType type, MemoryOptions options) {
    if (options.biased || type == ValueType::V128) return false;
    const bool tso =
            memory.GetOp() == OpCode::LoadMemoryTSO || memory.GetOp() == OpCode::StoreMemoryTSO;
    const bool scalar = type < ValueType::V8 || type > ValueType::V256;
    // A value address is dynamically aligned by the RCpc path, which needs
    // a bare address. Immediate addresses cannot be writeback candidates.
    return !tso || !options.rcpc || !scalar;
}
}  // namespace

const MemoryAction* MemoryActions::Find(u32 id) const {
    const auto found = std::find_if(actions.begin(), actions.end(), [id](const auto& row) {
        return row.instruction == id;
    });
    return found == actions.end() ? nullptr : &*found;
}
const MemoryAction* MemoryActions::Find(u32 id, MemoryAction::Flag flag) const {
    const auto* action = Find(id);
    return action && action->Has(flag) ? action : nullptr;
}

namespace detail {
std::optional<MemoryCandidates> SelectMemoryCandidates(const ir::Inst& memory,
                                                       compile::AllocationView allocation,
                                                       const MemorySelections& gpr,
                                                       MemoryOptions options,
                                                       const compile::BlockAnalysisIndex& index) {
    if (!IsMemoryAccess(memory.GetOp())) return std::nullopt;
    const bool load = IsLoad(memory.GetOp());
    const auto type = load ? memory.ReturnType() : memory.GetArg<Value>(1).Type();
    const auto operand = memory.GetArg<Operand>(0);
    auto* address = operand.GetRight().Null() && operand.GetLeft().IsValue()
                            ? operand.GetLeft().value.Def()
                            : nullptr;
    std::optional<MemoryCandidates> input;
    const auto record = [&]() -> MemoryCandidates& {
        if (!input)
            input.emplace(MemoryCandidates{
                    .memory = &memory, .address = address, .action = MemoryAction{memory.Id()}});
        return *input;
    };
    if (address && address->GetOp() == OpCode::GetOperand && address->GetUses() == 1 &&
        (options.narrow_fuse || options.address_tie) && !gpr.Address(address)) {
        const auto source = address->GetArg<Operand>(0);
        const auto result = Value{address};
        if (source.GetRight().Null() && source.GetLeft().IsValue() &&
            result.Type() == ValueType::U64 && source.GetLeft().value.Type() == ValueType::U64 &&
            Shares(allocation, result, source.GetLeft().value, address->Id()) &&
            RegisterAt(allocation, result, address->Id()) ==
                    RegisterAt(allocation, result, memory.Id()) &&
            index.Contains(address)) {
            record().cover_address = true;
        }
    }

    if (address && options.rcpc && !options.biased &&
        (memory.GetOp() == OpCode::LoadMemoryTSO || memory.GetOp() == OpCode::StoreMemoryTSO) &&
        (type < ValueType::V8 || type > ValueType::V256) &&
        (address->GetOp() == OpCode::Add || address->GetOp() == OpCode::Sub) &&
        address->GetUses(false) == 1 && !address->HasPseudoOperations() &&
        address->ReturnType() == ValueType::U64 && index.Contains(address) &&
        !gpr.Address(address) && !gpr.Transfer(&memory, MemoryTransferAction::Load) &&
        !gpr.IsLoadUpdateInstruction(&memory) &&
        !gpr.Transfer(address, MemoryTransferAction::PreIndex) &&
        !gpr.Transfer(address, MemoryTransferAction::Biased)) {
        const auto base = address->GetArg<Value>(0);
        const auto delta = address->GetArg<Operand>(1);
        // The base register must still hold the base value at the
        // access: either it is pinned there, or nothing between the
        // EA definition and the access can have taken its register.
        const bool base_stable =
                allocation
                        .GPRLocationAt(
                                base, memory.Id(), compile::AllocationView::GPRView::WholeValue)
                        .pinned ||
                index.Between(address->Id(), memory.Id()).empty();
        if (base.Type() == ValueType::U64 && base.Def() != address && base_stable &&
            delta.GetRight().Null() && delta.GetLeft().IsImm() &&
            RegisterAt(allocation, base, memory.Id()) < 31) {
            const auto imm = delta.GetLeft().imm.GetSigned();
            if (!(address->GetOp() == OpCode::Sub && imm == std::numeric_limits<s64>::min())) {
                const s64 offset = address->GetOp() == OpCode::Add ? imm : -imm;
                if (offset >= -256 && offset <= 255) {
                    auto& action = record().action;
                    action.bits |= MemoryAction::Offset;
                    action.offset = static_cast<s16>(offset);
                    record().cover_address = true;
                }
            }
        }
    }

    if (address && address->GetUses() == 2 && WritebackAllowed(memory, type, options) &&
        !gpr.Transfer(&memory, MemoryTransferAction::Load) &&
        !gpr.Transfer(address, MemoryTransferAction::PreIndex) && index.Contains(address)) {
        const auto source = operand.GetLeft().value;
        const auto base = RegisterAt(allocation, source, memory.Id());
        const auto instructions = index.Instructions();
        auto found = std::lower_bound(instructions.begin(),
                                      instructions.end(),
                                      address->Id(),
                                      [](const Inst* inst, u32 id) { return inst->Id() < id; });
        for (unsigned scanned = 0; found != instructions.end() && scanned < 3; ++found, ++scanned) {
            auto* update = *found;
            if (update->GetOp() != OpCode::Add && update->GetOp() != OpCode::Sub) continue;
            if (source.Type() != ValueType::U64 || update->ReturnType() != ValueType::U64 ||
                update->Id() <= memory.Id() || update->GetArg<Value>(0) != source ||
                update->HasPseudoOperations() || update->GetUses(false) == 0 || base >= 31 ||
                RegisterAt(allocation, Value{update}, memory.Id()) != base ||
                RegisterAt(allocation, Value{update}, update->Id()) != base)
                continue;
            const auto delta = update->GetArg<Operand>(1);
            if (!delta.GetRight().Null()) continue;
            s64 offset{};
            const auto left = delta.GetLeft();
            if (left.IsImm()) {
                const auto imm = left.imm.GetSigned();
                if (update->GetOp() == OpCode::Sub && imm == std::numeric_limits<s64>::min())
                    continue;
                offset = update->GetOp() == OpCode::Add ? imm : -imm;
                if (offset < -256 || offset > 255) continue;
            } else
                continue;  // Scalar LDR/STR post-index encodes an immediate.
            // A register delta needs the ordinary Add after the access. In
            // particular, VIXL's register post-index operand is a SIMD
            // structure form, not a scalar load/store writeback recipe.
            record().post_index = MemoryCandidates::PostIndex{
                    update, source, static_cast<s16>(offset), static_cast<u8>(base)};
            break;
        }
    }
    if (memory.GetOp() == OpCode::LoadMemory && options.narrow_fuse && !IsFloatValueType(type) &&
        GetValueSizeByte(type) <= 2 && memory.GetUses() == 1)
        record().narrow = true;
    if (!input || !index.Contains(&memory)) return std::nullopt;
    return input;
}

void LowerMemory(const ir::Block* block,
                 const MemoryCandidates& input,
                 compile::AllocationView allocation,
                 const FeatureSet& features,
                 const MemorySelections& gpr,
                 const GPRResidences& residences,
                 MemoryOptions options,
                 const compile::BlockAnalysisIndex& index,
                 MemoryActions& lowered) {
    ASSERT(input.memory && index.Contains(input.memory));
    ASSERT(!input.post_index || index.ResolvedUsesReady());
    ASSERT(!input.narrow || index.UsesReady());
    const auto& memory = *input.memory;
    const auto* address = input.address;
    const bool load = IsLoad(memory.GetOp());
    const auto type = load ? memory.ReturnType() : memory.GetArg<Value>(1).Type();
    const auto record = [&]() -> MemoryAction& {
        if (lowered.actions.empty() || lowered.actions.back().instruction != memory.Id())
            lowered.actions.push_back(MemoryAction{memory.Id()});
        return lowered.actions.back();
    };
    if (input.action.bits) record() = input.action;
    if (input.cover_address) lowered.covered.push_back(address->Id());
    if (input.post_index) {
        const auto& candidate = *input.post_index;
        const auto* update = candidate.update;
        const auto source = candidate.source;
        const auto base = candidate.base;
        const auto offset = candidate.offset;
        bool safe = true;
        auto owner = source;
        while (owner.Defined() && owner.Def()->IsBitCastOperation())
            owner = owner.Def()->GetArg<Value>(0);
        unsigned actual_uses = 0;
        if (!owner.Defined() || owner.Def()->GetUses(false) != index.DirectUseCount(owner.Def()))
            safe = false;
        else
            for (const auto& use : index.ResolvedUses(owner.Def())) {
                if (use.consumer->IsBitCastOperation()) {
                    if (use.consumer->GetUses(false) != index.DirectUseCount(use.consumer))
                        safe = false;
                } else {
                    if (use.consumer != &memory && use.consumer != update) safe = false;
                    actual_uses += use.count;
                }
            }
        if (actual_uses != 2) safe = false;
        for (auto* node : index.Between(memory.Id(), update->Id())) {
            if (IsFixedGPRPublicationObserver(*node, features) ||
                (FixedGPRClobbers(*node, features) & (1u << base)))
                safe = false;
        }
        if (load && RegisterAt(allocation, Value{&memory}, memory.Id()) == base) safe = false;
        // A scratch SSA update owns no guest state. A fixed home may
        // update early only when its publication immediately follows.
        if (allocation
                    .GPRLocationAt(
                            source, memory.Id(), compile::AllocationView::GPRView::WholeValue)
                    .pinned) {
            const auto uses = index.Uses(update);
            if (uses.size() != 1 || uses.front().count != 1)
                safe = false;
            else {
                auto* publication = uses.front().consumer;
                const auto after_update = std::next(block->GetInstList().iterator_to(*update));
                if (after_update == block->GetInstList().end() ||
                    after_update.operator->() != publication ||
                    publication->GetOp() != OpCode::SetHostGPR ||
                    publication->GetArg<Imm>(1).Get() != base ||
                    publication->GetArg<Imm>(2).Get() != 0)
                    safe = false;
            }
        }
        if (safe) {
            ASSERT(owner.Defined());
            auto& action = record();
            action.bits |= MemoryAction::PostIndex;
            action.base = static_cast<u8>(base);
            action.offset = static_cast<s16>(offset);
            action.access_bytes = static_cast<u8>(GetValueSizeByte(type));
            lowered.writes.push_back(
                    {memory.Id(), update->Id(), owner.Id(), static_cast<u8>(base)});
            lowered.covered.push_back(update->Id());
        }
    }
    if (!input.narrow) return;
    const auto uses = index.Uses(&memory);
    if (uses.size() != 1 || uses.front().count != 1) return;
    auto* extension = uses.front().consumer;
    if ((extension->GetOp() != OpCode::SignExtend && extension->GetOp() != OpCode::ZeroExtend32) ||
        extension->GetArg<Value>(0).Def() != &memory || extension->HasPseudoOperations() ||
        (GetValueSizeByte(extension->ReturnType()) != 4 &&
         GetValueSizeByte(extension->ReturnType()) != 8))
        return;
    const auto value = Value{&memory};
    const auto target = Value{extension};
    const auto definition = allocation.GPRLocationAt(
            value, memory.Id(), compile::AllocationView::GPRView::Definition);
    const bool shared = Shares(allocation, value, target, memory.Id());
    const bool pinned_extension = residences.Has(extension, GPRResidenceAction::SignExtend);
    const auto next = std::next(block->GetInstList().iterator_to(memory));
    const auto result = RegisterAt(allocation, target, memory.Id());
    bool direct = next != block->GetInstList().end() && next.operator->() == extension &&
                  !definition.pinned && result < 31 &&
                  result == RegisterAt(allocation, target, extension->Id());
    if (!shared && !pinned_extension && !direct) return;
    direct = direct && !shared && !pinned_extension;
    if (direct) {
        std::optional<u16> writeback_base;
        if (const auto* update = gpr.Transfer(&memory, MemoryTransferAction::Load))
            writeback_base = update->home;
        else if (!options.biased && address) {
            if (const auto* update = gpr.Transfer(address, MemoryTransferAction::PreIndex);
                update && update->MatchesMemory(memory))
                writeback_base = update->home;
            else if (const auto* update = lowered.Find(memory.Id(), MemoryAction::PostIndex))
                writeback_base = update->base;
        }
        if (writeback_base == result) return;
    }
    auto& action = record();
    action.bits |= MemoryAction::Narrow;
    if (direct) action.bits |= MemoryAction::Direct;
    if (extension->GetOp() == OpCode::SignExtend) action.bits |= MemoryAction::Sign;
    if (GetValueSizeByte(extension->ReturnType()) == 8) action.bits |= MemoryAction::Wide;
    action.target = direct ? static_cast<u8>(result) : UINT8_MAX;
    action.access_bytes = static_cast<u8>(GetValueSizeByte(type));
    lowered.covered.push_back(extension->Id());
}
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
