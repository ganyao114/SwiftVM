#include <algorithm>
#include <limits>
#include "aarch64/assembler-aarch64.h"
#include "gpr_inputs.h"
#include "internal.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/gpr_coalescing_contract.h"
#include "runtime/compile/analysis/uses.h"

namespace swift::runtime::backend::arm64 {
using vixl::aarch64::XRegister;
namespace {

bool IsWidthChainHostWWrite(ir::Value value, u32 target, const FeatureSet& features) {
    value = ir::ResolveBitCastSource(value);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return features.ra_width_chain && ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
               def->GetArg<ir::Imm>(1).Get() == 0 && def->GetArg<ir::Imm>(0).Get() != target;
    }
    return HasKnownWWrite(value, WWriteAliases::Resolved);
}

bool TerminalUsesHostCoalesceValue(const ir::Terminal& terminal_value, const ir::Inst* definition) {
    bool used = false;
    compile::VisitTerminalValues(terminal_value, [&](ir::Value value) {
        if (!used) used = ir::ResolveBitCastSource(value).Def() == definition;
    });
    return used;
}

using ::swift::runtime::backend::IsFixedGPRHome;

std::optional<s64> ImmediateValue(const ir::Operand& operand) {
    if (operand.GetRight().IsImm()) {
        return operand.GetRight().imm.GetSigned();
    }
    if (operand.IsImm()) {
        return operand.GetLeft().imm.GetSigned();
    }
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    auto value = ir::ResolveBitCastSource(operand.GetLeft().value);
    if (!value.Def() || value.Def()->GetOp() != ir::OpCode::LoadImm) {
        return std::nullopt;
    }
    return static_cast<s64>(value.Def()->GetArg<ir::Imm>(0).Get());
}

bool IsPinnedMemoryTarget(u32 target) {
    return target <= 9 || target == 19 || target == 20 || target == 21 || target == 22 ||
           target == 23 || target == 29;
}

bool IsCallerSavedAddressBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    return op == O::CallLambda || op == O::CallLocation || op == O::CallDynamic || op == O::X87Op ||
           op == O::Sse42Str;
}

}  // namespace

bool TargetPreparation::PrepareCoalescedHostWriteProof(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostGPR || inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const auto publication_width = ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type());
    if (publication_width != sizeof(u32) && publication_width != sizeof(u64)) return false;
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    auto stored = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    auto* wrapper = stored.Def();
    const bool width_wrapper = stored.Defined() && allocation.IsWidthChainCoalesced(stored.Id());
    const bool live_publish = features.ra_coalesce_live && !width_wrapper;
    if (!wrapper || (!width_wrapper && !live_publish && wrapper->GetUses() != 1)) {
        return false;
    }
    auto produced = stored;
    bool zero_extend_chain = false;
    bool low32_copy = false;
    if (wrapper->GetOp() == ir::OpCode::ZeroExtend32To64) {
        produced = ir::ResolveBitCastSource(wrapper->GetArg<ir::Value>(0));
        zero_extend_chain = true;
        if (produced.Def() && allocation.IsLow32CopyCoalesced(produced.Id()) &&
            produced.Def()->GetOp() == ir::OpCode::BitExtract &&
            produced.Def()->GetArg<ir::Imm>(1).Get() == 0 &&
            produced.Def()->GetArg<ir::Imm>(2).Get() == 32) {
            auto source = ir::ResolveBitCastSource(produced.Def()->GetArg<ir::Value>(0));
            low32_copy = source.Defined() &&
                         allocation.Low32CopySource(produced.Id()) == source.Id() &&
                         SharesAllocatedGPR(allocation, source, produced);
        }
        const bool width_root = produced.Defined() &&
                                allocation.IsWidthChainCoalesced(produced.Id()) &&
                                allocation.WidthChainAnchor(produced.Id()) == produced.Id();
        if (!produced.Def() || (!width_root && !live_publish && produced.Def()->GetUses() != 1) ||
            (!low32_copy && !IsWidthChainHostWWrite(produced, target, features))) {
            return false;
        }
    }
    auto* producer = produced.Def();
    if (!producer || !IsGPRPublicationProducer(producer->GetOp(), features.ra_width_chain) ||
        (!low32_copy && !IsGPRMappedTo(produced, target)) ||
        (zero_extend_chain && !IsGPRMappedTo(stored, target))) {
        return false;
    }
    const u32 width = ir::GetValueSizeByte(produced.Type());
    if ((width != sizeof(u32) && width != sizeof(u64)) ||
        ((width == sizeof(u32) || publication_width == sizeof(u32)) && !low32_copy &&
         !IsWidthChainHostWWrite(produced, target, features))) {
        return false;
    }
    if (width_wrapper) {
        const u32 anchor = allocation.WidthChainAnchor(stored.Id());
        if (allocation.HasWidthComponentOwner(anchor)) {
            const bool actual_high_zero =
                    zero_extend_chain || ir::GetValueSizeByte(produced.Type()) == sizeof(u32);
            if (!allocation.WidthComponentOwnerCommitted(anchor) ||
                allocation.WidthComponentOwnerTarget(anchor) != target ||
                allocation.WidthComponentOwnerHighZero(anchor) != actual_high_zero) {
                return false;
            }
        }
    }
    const ir::Inst* component_source = nullptr;
    if (producer->GetOp() == ir::OpCode::SignExtend) {
        auto source = ir::ResolveBitCastSource(producer->GetArg<ir::Value>(0));
        auto* source_def = source.Def();
        if (source_def && allocation.IsWidthChainCoalesced(source.Id())) {
            if ((source_def->GetOp() != ir::OpCode::LoadMemory &&
                 source_def->GetOp() != ir::OpCode::LoadMemoryTSO) ||
                ir::GetValueSizeByte(source.Type()) > sizeof(u16) || source_def->GetUses() != 1 ||
                allocation.WidthChainAnchor(source.Id()) !=
                        allocation.WidthChainAnchor(produced.Id()) ||
                !SharesAllocatedGPR(allocation, source, produced)) {
                return false;
            }
            component_source = source_def;
        }
    }
    const u32 proof_start = component_source ? component_source->Id() : producer->Id();
    const auto& analysis = gpr_builder.UseFacts();
    auto last_use = [&](const ir::Inst* definition) {
        gpr_builder.PrepareUseFacts().PrepareResolvedUses();
        u32 end = analysis.LastInstructionUse(definition);
        if (TerminalUsesHostCoalesceValue(cur_block->GetTerminal(), definition) &&
            !analysis.Instructions().empty()) {
            end = std::max<u32>(end, analysis.Instructions().back()->Id());
        }
        return end;
    };

    for (auto input : producer->GetValues()) {
        auto root = ir::ResolveBitCastSource(input);
        if (low32_copy && root.Defined() &&
            allocation.Low32CopySource(produced.Id()) == root.Id()) {
            continue;
        }
        if (!root.Defined() || !SharesAllocatedGPR(allocation, root, produced)) {
            continue;
        }
        if (last_use(root.Def()) > producer->Id()) {
            return false;
        }
    }

    u32 live_end = inst->Id();
    if (live_publish) {
        live_end = std::max<u32>(live_end, last_use(wrapper));
        if (zero_extend_chain) {
            live_end = std::max<u32>(live_end, last_use(producer));
        }
    }
    for (auto* node : analysis.Instructions()) {
        auto& other = *node;
        if (other.Id() > live_end) break;
        if (&other == component_source || &other == producer || &other == wrapper ||
            &other == inst || !other.HasValue() || other.IsBitCastOperation()) {
            continue;
        }
        const bool low32_copy_alias = allocation.IsLow32CopyCoalesced(other.Id()) &&
                                      other.GetOp() == ir::OpCode::BitExtract &&
                                      other.GetArg<ir::Imm>(1).Get() == 0 &&
                                      other.GetArg<ir::Imm>(2).Get() == 32 &&
                                      (allocation.Low32CopySource(other.Id()) == stored.Id() ||
                                       allocation.Low32CopySource(other.Id()) == produced.Id());
        if (low32_copy_alias) {
            continue;
        }
        if (other.Id() > inst->Id() && other.GetOp() == ir::OpCode::GetHostGPR &&
            other.GetArg<ir::Imm>(0).Get() == target) {
            continue;
        }
        ir::Value value{&other};
        auto& allocation = this->allocation;
        if (allocation.FixedGPRPublicationOwns(inst->Id(), value.Id())) continue;
        const bool fixed = allocation.HasFixedGPRDefinition(value.Id());
        if (!fixed && !IsGPRMappedTo(value, target)) continue;
        // Fixed segments carry exact ends; ordinary ends come from shared uses.
        const u32 end = fixed ? value.Id() : last_use(&other);
        if (allocation.FixedGPRHomeIntersects(value, target, proof_start, live_end, end)) {
            return false;
        }
    }

    // An incoming definition has no local start node, matching the original
    // local-window contract. Ordinary local proofs inspect only their window.
    const auto* start = component_source ? component_source : producer;
    const auto window = analysis.Contains(start) ? analysis.Between(proof_start, inst->Id())
                                                 : std::span<const ir::Inst* const>{};
    for (auto* node : window) {
        auto& scan = *node;
        if (&scan == producer || &scan == wrapper) {
            continue;
        }
        const bool flags_only = scan.GetOp() == ir::OpCode::SaveFlags;
        if ((!flags_only && IsFixedGPRPublicationObserver(scan, features)) ||
            (scan.GetOp() == ir::OpCode::GetHostGPR && scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostGPR && scan.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
    }
    if (!live_publish) {
        if (!width_wrapper) {
            gpr_builder.PrepareUseFacts().PrepareResolvedUses();
            if (analysis.LastInstructionUse(wrapper) > inst->Id()) return false;
        }
        if (TerminalUsesHostCoalesceValue(cur_block->GetTerminal(), wrapper)) {
            return false;
        }
        return true;
    }
    for (auto* node : analysis.Between(inst->Id(), live_end + 1)) {
        auto& scan = *node;
        if (scan.GetOp() == ir::OpCode::SetHostGPR && scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
        if (target <= 9 && HelperCallContract::InstructionClobbersGPR(scan, target, features)) {
            return false;
        }
    }
    return true;
}

std::optional<TargetPreparation::StoreUpdateCandidate> TargetPreparation::MatchMemoryUpdate(
        const ir::Inst* update) const {
    if (!update || update->GetOp() != ir::OpCode::Sub ||
        ir::GetValueSizeByte(update->ReturnType()) != sizeof(u64) || update->GetUses(false) != 2) {
        return std::nullopt;
    }

    const auto right = update->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null() || !right.GetLeft().IsImm()) {
        return std::nullopt;
    }
    const u64 decrement = right.GetLeft().imm.Get();
    if (decrement == 0 || decrement > 256) {
        return std::nullopt;
    }

    const auto source = update->GetArg<ir::Value>(0);
    auto* source_def = source.Def();
    if (!source_def || source_def->GetOp() != ir::OpCode::GetHostGPR ||
        source_def->GetArg<ir::Imm>(1).Get() != 0 ||
        ir::GetValueSizeByte(source.Type()) != sizeof(u64)) {
        return std::nullopt;
    }
    const u32 target = source_def->GetArg<ir::Imm>(0).Get();
    if (!IsGPRMappedTo(source, target)) {
        return std::nullopt;
    }

    auto& instructions = cur_block->GetInstList();
    auto memory_it = std::next(instructions.iterator_to(*update));
    if (memory_it == instructions.end() || memory_it->GetOp() != ir::OpCode::StoreMemory) {
        return std::nullopt;
    }
    const auto memory_operand = memory_it->GetArg<ir::Operand>(0);
    const auto stored = memory_it->GetArg<ir::Value>(1);
    if (!memory_operand.GetRight().Null() || !memory_operand.GetLeft().IsValue() ||
        memory_operand.GetLeft().value.Def() != update || ir::IsFloatValueType(stored.Type()) ||
        ir::GetValueSizeByte(stored.Type()) != decrement) {
        return std::nullopt;
    }
    if (IsGPRMappedTo(stored, target)) {
        return std::nullopt;
    }

    auto publication_it = std::next(memory_it);
    if (publication_it == instructions.end() || publication_it->GetOp() != ir::OpCode::SetHostGPR ||
        publication_it->GetArg<ir::Value>(0).Def() != update ||
        publication_it->GetArg<ir::Imm>(1).Get() != target ||
        publication_it->GetArg<ir::Imm>(2).Get() != 0) {
        return std::nullopt;
    }

    if (allocation.PrepareGPRPreservations().Conflicts(
                target, memory_it->Id(), publication_it->Id()))
        return std::nullopt;
    return StoreUpdateCandidate{
            .memory = memory_it.operator->(),
            .publication = publication_it.operator->(),
            .home = static_cast<u8>(target),
            .offset = static_cast<s16>(-static_cast<s64>(decrement)),
    };
}

void TargetPreparation::PrepareMemoryUpdates(const ir::Block* block) {
    // The declaration entry resets this frame once; retain earlier selections.
    for (auto& inst : block->GetInstList()) {
        if (const auto update = MatchPreIndexMemoryUpdate(&inst)) {
            memory_selections.Record({inst.Id(),
                                      update->memory->Id(),
                                      update->offset,
                                      update->home,
                                      MemoryTransferAction::PreIndex});
            memory_selections.LinkPublication(inst.Id(), update->publication->Id());
            gpr_builder.RecordPhysicalWrite(update->memory, update->home);
        }
        if (const auto update = MatchBiasedMemoryUpdate(&inst)) {
            memory_selections.Record({inst.Id(),
                                      update->memory->Id(),
                                      update->offset,
                                      update->home,
                                      MemoryTransferAction::Biased});
            memory_selections.LinkPublication(inst.Id(), update->publication->Id());
        }
    }
}

std::optional<TargetPreparation::LoadUpdateCandidate> TargetPreparation::MatchPinnedLoadUpdate(
        const ir::Inst* update) {
    if (use_memory_base || !update || update->GetOp() != ir::OpCode::Add ||
        update->ReturnType() != ir::ValueType::U64 || !GetPseudoFlags(update).Null()) {
        return std::nullopt;
    }
    auto base = ir::ResolveBitCastSource(update->GetArg<ir::Value>(0));
    auto* base_read = base.Def();
    const auto offset = ImmediateValue(update->GetArg<ir::Operand>(1));
    if (!base_read || base_read->GetOp() != ir::OpCode::GetHostGPR ||
        base_read->GetArg<ir::Imm>(1).Get() != 0 || !offset || *offset != 1) {
        return std::nullopt;
    }
    const u32 target = base_read->GetArg<ir::Imm>(0).Get();
    if (!IsFixedGPRHome(target) || base_read->GetUses(false) != 2) {
        return std::nullopt;
    }

    const ir::Inst* load{};
    const auto& analysis = gpr_builder.UseFacts();
    gpr_builder.PrepareUseFacts().PrepareResolvedUses();
    for (const auto& use : analysis.ResolvedUses(base_read)) {
        auto& scan = *use.consumer;
        if (scan.Id() >= update->Id()) {
            break;
        }
        if (scan.GetOp() != ir::OpCode::LoadMemory) {
            continue;
        }
        const auto address = scan.GetArg<ir::Operand>(0);
        if (address.GetOp() != ir::OperandOp::Plus || !address.GetLeft().IsValue() ||
            ir::ResolveBitCastSource(address.GetLeft().value).Def() != base_read) {
            continue;
        }
        const auto memory_offset = ImmediateValue(address);
        if (memory_offset && *memory_offset == *offset) {
            load = &scan;
        }
    }
    const ir::Inst* publication{};
    for (const auto& use : analysis.Uses(update)) {
        auto& scan = *use.consumer;
        if (scan.Id() <= update->Id()) {
            continue;
        }
        if (use.count == 1 && scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Value>(0).Def() == update && scan.GetArg<ir::Imm>(1).Get() == target &&
            scan.GetArg<ir::Imm>(2).Get() == 0) {
            publication = &scan;
        }
        break;
    }
    if (!load || !publication || load->Id() >= update->Id() ||
        IsGPRMappedTo(ir::Value{load}, target)) {
        return std::nullopt;
    }
    u32 ordinary_uses{};
    for (const auto& use : analysis.Uses(update)) {
        auto& scan = *use.consumer;
        if (scan.GetOp() == ir::OpCode::SaveFlags || scan.GetOp() == ir::OpCode::BranchOnlyFlags ||
            scan.GetOp() == ir::OpCode::ClearFlags) {
            continue;
        }
        ordinary_uses += use.count;
    }
    if (ordinary_uses != 1) {
        return std::nullopt;
    }
    for (auto* node : analysis.Between(load->Id(), update->Id())) {
        auto& scan = *node;
        if (MayFaultOrObserve(scan) ||
            (scan.GetOp() == ir::OpCode::GetHostGPR && scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostGPR && scan.GetArg<ir::Imm>(1).Get() == target)) {
            return std::nullopt;
        }
    }
    return LoadUpdateCandidate{
            .load = load,
            .update = update,
            .publication = publication,
            .base_read = base_read,
            .target = static_cast<u16>(target),
            .offset = static_cast<s16>(*offset),
    };
}

void TargetPreparation::PreparePinnedLoadUpdates(const ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        auto candidate = MatchPinnedLoadUpdate(&inst);
        if (!candidate || allocation.PrepareFixedGPRHomes().Conflicts(candidate->target,
                                                                      candidate->load->Id(),
                                                                      candidate->publication->Id(),
                                                                      candidate->base_read->Id(),
                                                                      candidate->update->Id())) {
            continue;
        }
        gpr_builder.AssignLowView(candidate->base_read, candidate->target);
        if (allocation.TargetPlanning()) {
            if (const auto* current = allocation.FixedGPRDefinitionAt(candidate->base_read->Id());
                current && current->elided && !current->preserved) {
                auto residence = *current;
                residence.end = candidate->load->Id();
                allocation.PlanFixedGPRDefinition(candidate->base_read->Id(), residence);
            }
        }
        gpr_builder.RecordPhysicalWrite(candidate->load, candidate->target);
        memory_selections.Record({candidate->load->Id(),
                                  candidate->update->Id(),
                                  candidate->offset,
                                  static_cast<u8>(candidate->target),
                                  MemoryTransferAction::Load});
        memory_selections.Record({candidate->update->Id(),
                                  candidate->publication->Id(),
                                  candidate->offset,
                                  static_cast<u8>(candidate->target),
                                  MemoryTransferAction::LoadUpdate});
    }
}

std::optional<TargetPreparation::MemorySourceCandidate> TargetPreparation::MatchPinnedMemorySource(
        ir::Value source) {
    auto* read = source.Def();
    if (!read || ir::GetValueSizeByte(source.Type()) != sizeof(u64)) {
        return std::nullopt;
    }

    u32 target{};
    u32 live_begin{};
    if (read->GetOp() == ir::OpCode::GetHostGPR) {
        if (read->GetArg<ir::Imm>(1).Get() != 0) {
            return std::nullopt;
        }
        target = read->GetArg<ir::Imm>(0).Get();
        live_begin = read->Id();
    } else if (read->GetOp() == ir::OpCode::BitCast) {
        const auto published = read->GetArg<ir::Value>(0);
        auto* producer = published.Def();
        if (!producer ||
            (producer->GetOp() != ir::OpCode::LoadMemory &&
             producer->GetOp() != ir::OpCode::LoadMemoryTSO) ||
            ir::GetValueSizeByte(published.Type()) != sizeof(u64) || read->GetUses(false) != 1) {
            return std::nullopt;
        }
        const ir::Inst* publication = nullptr;
        u32 producer_uses = 0;
        gpr_builder.PrepareUseFacts().PrepareUses();
        for (const auto& use : gpr_builder.UseFacts().Uses(producer)) {
            auto& scan = *use.consumer;
            producer_uses += use.count;
            if (&scan == read) continue;
            if (use.count != 1 || publication || scan.GetOp() != ir::OpCode::SetHostGPR ||
                scan.GetArg<ir::Value>(0).Def() != producer || scan.GetArg<ir::Imm>(2).Get() != 0 ||
                !allocation.IsHostWriteCoalesced(scan.Id())) {
                return std::nullopt;
            }
            publication = &scan;
        }
        if (!publication || producer_uses != producer->GetUses() ||
            publication->Id() >= read->Id()) {
            return std::nullopt;
        }
        target = publication->GetArg<ir::Imm>(1).Get();
        live_begin = publication->Id();
        if (!IsGPRMappedTo(published, target)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!IsPinnedMemoryTarget(target) || !IsGPRMappedTo(source, target)) {
        return std::nullopt;
    }
    return MemorySourceCandidate{static_cast<u16>(target), live_begin};
}

std::optional<AddressAction> TargetPreparation::MatchPinnedMemoryAddress(const ir::Inst* address) {
    if (!address || address->GetOp() != ir::OpCode::GetOperand || address->GetUses(false) != 1) {
        return std::nullopt;
    }
    const auto operand = address->GetArg<ir::Operand>(0);
    if (!operand.GetLeft().IsValue()) {
        return std::nullopt;
    }
    s64 offset{};
    if (!operand.GetRight().Null()) {
        if (operand.GetOp() != ir::OperandOp::Plus || !operand.GetRight().IsImm()) {
            return std::nullopt;
        }
        offset = operand.GetRight().imm.GetSigned();
        if (offset == std::numeric_limits<s64>::min()) {
            return std::nullopt;
        }
    }
    const auto source = MatchPinnedMemorySource(operand.GetLeft().value);
    if (!source) {
        return std::nullopt;
    }

    const ir::Inst* memory = nullptr;
    const auto& analysis = gpr_builder.UseFacts();
    gpr_builder.PrepareUseFacts().PrepareUses();
    for (const auto& use : analysis.Uses(address)) {
        auto& scan = *use.consumer;
        if (scan.Id() <= address->Id() ||
            (scan.GetOp() != ir::OpCode::LoadMemory && scan.GetOp() != ir::OpCode::StoreMemory)) {
            continue;
        }
        const auto memory_operand = scan.GetArg<ir::Operand>(0);
        if (memory_operand.GetRight().Null() && memory_operand.GetLeft().IsValue() &&
            memory_operand.GetLeft().value.Def() == address) {
            memory = &scan;
            break;
        }
    }
    if (!memory) {
        return std::nullopt;
    }

    if (offset != 0 && !use_memory_base) {
        const auto type = memory->GetOp() == ir::OpCode::LoadMemory
                                  ? memory->ReturnType()
                                  : memory->GetArg<ir::Value>(1).Type();
        const u32 access_size = ir::GetValueSizeByte(type);
        if (!vixl::aarch64::Assembler::IsImmLSUnscaled(offset) &&
            !vixl::aarch64::Assembler::IsImmLSScaled(offset, access_size)) {
            return std::nullopt;
        }
    }

    for (auto* node : analysis.Between(source->live_begin, memory->Id())) {
        auto& scan = *node;
        if ((scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == source->target) ||
            (source->target <= 9 && IsCallerSavedAddressBarrier(scan.GetOp()))) {
            return std::nullopt;
        }
    }
    return AddressAction{offset, address->Id(), memory->Id(), static_cast<u8>(source->target)};
}

std::optional<MemoryTransferAction> TargetPreparation::PreparePinnedMemoryValue(
        const ir::Inst* extract) {
    if (!extract || extract->GetOp() != ir::OpCode::BitExtract || extract->GetUses(false) != 1) {
        return std::nullopt;
    }
    const auto source = extract->GetArg<ir::Value>(0);
    const u32 width = ir::GetValueSizeByte(extract->ReturnType());
    if (!source.Def() || (width != sizeof(u8) && width != sizeof(u16)) ||
        extract->GetArg<ir::Imm>(1).Get() != 0 || extract->GetArg<ir::Imm>(2).Get() != width * 8 ||
        ir::GetValueSizeByte(source.Type()) < width || IsSpilled(source)) {
        return std::nullopt;
    }

    const ir::Inst* store = nullptr;
    const ir::Inst* publication = nullptr;
    const auto& analysis = gpr_builder.UseFacts();
    gpr_builder.PrepareUseFacts().PrepareUses();
    for (const auto& use : analysis.Uses(source.Def())) {
        auto& scan = *use.consumer;
        if (scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Value>(0).Def() == source.Def()) {
            if (publication) {
                return std::nullopt;
            }
            publication = &scan;
        }
    }
    for (const auto& use : analysis.Uses(extract)) {
        auto& scan = *use.consumer;
        if (store || scan.GetOp() != ir::OpCode::StoreMemory ||
            scan.GetArg<ir::Value>(1).Def() != extract ||
            ir::GetValueSizeByte(scan.GetArg<ir::Value>(1).Type()) != width) {
            return std::nullopt;
        }
        store = &scan;
    }
    if (!store || !publication || publication->Id() >= extract->Id() ||
        extract->Id() >= store->Id() || publication->GetArg<ir::Imm>(2).Get() != 0 ||
        !allocation.IsHostWriteCoalesced(publication->Id()) ||
        !PrepareCoalescedHostWriteProof(publication)) {
        return std::nullopt;
    }

    const u32 target = publication->GetArg<ir::Imm>(1).Get();
    if (!IsPinnedMemoryTarget(target) || !IsGPRMappedTo(source, target)) {
        return std::nullopt;
    }
    for (auto* node : analysis.Between(publication->Id(), store->Id())) {
        auto& scan = *node;
        if ((scan.GetOp() == ir::OpCode::SetHostGPR && scan.GetArg<ir::Imm>(1).Get() == target) ||
            (target <= 9 && IsCallerSavedAddressBarrier(scan.GetOp()))) {
            return std::nullopt;
        }
    }
    return MemoryTransferAction{
            extract->Id(), store->Id(), 0, static_cast<u8>(target), MemoryTransferAction::Value};
}

void TargetPreparation::PreparePinnedMemoryValues(const ir::Block* block) {
    // The declaration entry resets this frame once; retain earlier selections.
    for (auto& inst : block->GetInstList()) {
        if (auto value = PreparePinnedMemoryValue(&inst)) {
            memory_selections.Record(*value);
            gpr_builder.AssignLowView(&inst, value->home);
        }
    }
}

void TargetPreparation::PreparePinnedMemoryAddresses(const ir::Block* block) {
    // The declaration entry resets this frame once; retain earlier selections.
    for (auto& inst : block->GetInstList()) {
        if (const auto address = MatchPinnedMemoryAddress(&inst)) {
            memory_selections.Record(*address);
            if (address->offset == 0) gpr_builder.AssignValueHome(&inst, address->home);
        }
    }
}

void TargetPreparation::PreparePinnedZeroExtendPublications(const ir::Block* block) {
    if (GetSvmConfig().zext_no_elide) return;
    for (auto& candidate : block->GetInstList()) {
        auto* inst = &candidate;
        if (inst->GetOp() != ir::OpCode::ZeroExtend32To64 || IsGPRZeroExtendElided(inst)) continue;
        const auto source = inst->GetArg<ir::Value>(0);
        // Keep the existing lowering priority: a tied W write still clears
        // the upper half; the publication-only case comes after those ties.
        if (CanUseZeroStoreRegister(ir::Value{inst}) ||
            allocation.IsWidthChainCoalesced(inst->Id()) ||
            (source.Def() && allocation.IsLow32CopyCoalesced(source.Id())) ||
            (ir::GetValueSizeByte(source.Type()) == sizeof(u32) &&
             SharesAllocatedGPR(allocation, source, ir::Value{inst})) ||
            inst->GetUses() != 1)
            continue;
        auto& list = block->GetInstList();
        for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
            bool names_value = false;
            for (auto used : it->GetValues()) names_value |= used.Def() == inst;
            if (!names_value) continue;
            if (it->GetOp() == ir::OpCode::SetHostGPR && it->GetArg<ir::Value>(0).Def() == inst &&
                it->GetArg<ir::Imm>(2).Get() == 0) {
                const auto target = it->GetArg<ir::Imm>(1).Get();
                bool source_survives = true;
                const auto& allocation = this->allocation;
                // Elision moves the parent's machine use from the extension
                // to this publication. RA still expires it at the original
                // use, so an intervening definition or pre-publication capture
                // can reuse its register (or spill slot).
                const auto kind = allocation.ValueType(source);
                const auto slot =
                        kind == AllocationPreparation::GPR   ? allocation.ValueGPR(source).id
                        : kind == AllocationPreparation::MEM ? allocation.ValueMem(source).offset
                                                             : UINT16_MAX;
                for (auto scan = std::next(list.iterator_to(*inst)); scan != std::next(it);
                     ++scan) {
                    if (kind == AllocationPreparation::GPR) {
                        if (backend::FixedGPRClobbers(*scan, features) & (1u << slot))
                            source_survives = false;
                        if (scan.operator->() != it.operator->() &&
                            scan->GetOp() == ir::OpCode::SetHostGPR &&
                            scan->GetArg<ir::Imm>(1).Get() == slot)
                            source_survives = false;
                        if (const auto helper = HelperCallContract::Resolve(*scan, features);
                            helper && helper->ClobbersGPR(slot))
                            source_survives = false;
                    }
                    for (const auto& capture : allocation.GPRPreservationsBefore(scan->Id()))
                        if (capture.destination_kind == kind && capture.destination == slot)
                            source_survives = false;
                    if (scan->HasValue()) {
                        const ir::Value result{scan.operator->()};
                        if (allocation.ValueType(result) == kind &&
                            ((kind == AllocationPreparation::GPR &&
                              allocation.ValueGPR(result).id == slot) ||
                             (kind == AllocationPreparation::MEM &&
                              allocation.ValueMem(result).offset == slot)))
                            source_survives = false;
                    }
                }
                if (source_survives &&
                    (target <= 9 || target == 22 || target == 23 || target == 29))
                    gpr_builder.ElideZeroExtend(inst);
            }
            break;
        }
    }
}

std::optional<TargetPreparation::StoreUpdateCandidate> TargetPreparation::MatchPreIndexMemoryUpdate(
        const ir::Inst* update) const {
    return use_memory_base ? std::nullopt : MatchMemoryUpdate(update);
}
std::optional<TargetPreparation::StoreUpdateCandidate> TargetPreparation::MatchBiasedMemoryUpdate(
        const ir::Inst* update) const {
    if (!use_memory_base) return std::nullopt;
    auto match = MatchMemoryUpdate(update);
    return match && IsSpilled(ir::Value{update}) ? match : std::nullopt;
}

bool TargetPreparation::CanUseZeroStoreRegister(ir::Value value) const {
    return TargetCanUseZeroStoreRegister(value, value.Def(), allocation.Actions(), features);
}
}  // namespace swift::runtime::backend::arm64
