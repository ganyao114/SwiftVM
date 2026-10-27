#include "memory_selections.h"

#include <algorithm>
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::backend::arm64 {
bool MemorySelections::Owns(const ir::Inst* inst) const {
    return analysis && analysis->Contains(inst);
}

const MemoryTransferAction* MemorySelections::Transfer(const ir::Inst* inst,
                                                       MemoryTransferAction::Kind kind) const {
    if (!Owns(inst)) return nullptr;
    const auto row = std::lower_bound(
            transfers.begin(), transfers.end(), inst->Id(), [](const auto& row, u16 id) {
                return row.instruction < id;
            });
    return row != transfers.end() && row->instruction == inst->Id() && row->kind == kind ? &*row
                                                                                         : nullptr;
}

const AddressAction* MemorySelections::Address(const ir::Inst* inst) const {
    if (!Owns(inst)) return nullptr;
    const auto row = std::lower_bound(
            addresses.begin(), addresses.end(), inst->Id(), [](const auto& row, u16 id) {
                return row.instruction < id;
            });
    return row != addresses.end() && row->instruction == inst->Id() ? &*row : nullptr;
}

bool MemorySelections::IsLoadUpdateInstruction(const ir::Inst* inst) const {
    return Transfer(inst, MemoryTransferAction::LoadUpdate) ||
           (Owns(inst) &&
            std::binary_search(load_publications.begin(), load_publications.end(), inst->Id()));
}

void MemorySelectionBuilder::Reset(const compile::BlockAnalysisIndex& input) {
    index.reset();
    selections.analysis = &input;
    selections.transfers.clear();
    selections.addresses.clear();
    selections.load_publications.clear();
    slots.clear();
    index.emplace(workspace, input.Instructions().size());
}

MemorySelectionBuilder::Slot& MemorySelectionBuilder::Insert(u16 instruction,
                                                             u16 row,
                                                             SlotKind kind) {
    ASSERT(index);
    const auto* inst = selections.analysis->FindInstruction(instruction);
    const auto position = selections.analysis->InstructionPosition(inst);
    ASSERT(position);
    auto& slot = (*index)[*position];
    if (slot == UINT32_MAX) {
        slot = static_cast<u32>(slots.size());
        slots.push_back({instruction, row, UINT16_MAX, kind});
    }
    return slots[slot];
}

const MemorySelectionBuilder::Slot* MemorySelectionBuilder::Find(const ir::Inst* inst) const {
    ASSERT(index);
    const auto position = selections.analysis->InstructionPosition(inst);
    if (!position) return nullptr;
    const auto slot = index->Find(*position);
    return slot == UINT32_MAX ? nullptr : &slots[slot];
}

void MemorySelectionBuilder::Record(MemoryTransferAction row) {
    ASSERT(selections.transfers.size() < UINT16_MAX);
    const auto slot =
            Insert(row.instruction, static_cast<u16>(selections.transfers.size()), TransferSlot);
    ASSERT(slot.kind == TransferSlot);
    if (slot.row == selections.transfers.size())
        selections.transfers.push_back(row);
    else
        ASSERT(selections.transfers[slot.row].kind == row.kind);
    if (row.kind == MemoryTransferAction::LoadUpdate)
        (void)Insert(row.related, slot.row, LoadPublicationSlot);
}

void MemorySelectionBuilder::Record(AddressAction row) {
    ASSERT(selections.addresses.size() < UINT16_MAX);
    const auto& slot =
            Insert(row.instruction, static_cast<u16>(selections.addresses.size()), AddressSlot);
    ASSERT(slot.kind == AddressSlot);
    if (slot.row == selections.addresses.size()) selections.addresses.push_back(row);
}

void MemorySelectionBuilder::LinkPublication(u16 update, u16 publication) {
    ASSERT(index);
    const auto* inst = selections.analysis->FindInstruction(update);
    const auto* found = Find(inst);
    ASSERT(found && found->kind == TransferSlot);
    const auto position = selections.analysis->InstructionPosition(inst);
    auto& slot = slots[index->Find(*position)];
    if (slot.publication == UINT16_MAX) slot.publication = publication;
}

const MemoryTransferAction* MemorySelectionBuilder::Transfer(
        const ir::Inst* inst, MemoryTransferAction::Kind kind) const {
    const auto* slot = Find(inst);
    if (!slot || slot->kind != TransferSlot) return nullptr;
    const auto& row = selections.transfers[slot->row];
    return row.kind == kind ? &row : nullptr;
}

const AddressAction* MemorySelectionBuilder::Address(const ir::Inst* inst) const {
    const auto* slot = Find(inst);
    return slot && slot->kind == AddressSlot ? &selections.addresses[slot->row] : nullptr;
}

bool MemorySelectionBuilder::IsLoadPublication(const ir::Inst* inst) const {
    const auto* slot = Find(inst);
    return slot && slot->kind == LoadPublicationSlot;
}

bool MemorySelectionBuilder::IsLoadUpdateInstruction(const ir::Inst* inst) const {
    return Transfer(inst, MemoryTransferAction::LoadUpdate) || IsLoadPublication(inst);
}

std::optional<u16> MemorySelectionBuilder::PublicationForUpdate(const ir::Inst* inst) const {
    const auto* slot = Find(inst);
    return slot && slot->kind == TransferSlot && slot->publication != UINT16_MAX
                   ? std::optional{slot->publication}
                   : std::nullopt;
}

MemorySelections MemorySelectionBuilder::Finish() {
    ASSERT(index);
    for (const auto& slot : slots)
        if (slot.kind == LoadPublicationSlot)
            selections.load_publications.push_back(slot.instruction);
    const auto before = [](const auto& a, const auto& b) { return a.instruction < b.instruction; };
    std::sort(selections.transfers.begin(), selections.transfers.end(), before);
    std::sort(selections.addresses.begin(), selections.addresses.end(), before);
    std::sort(selections.load_publications.begin(), selections.load_publications.end());
    index.reset();
    slots.clear();
    return std::move(selections);
}
}  // namespace swift::runtime::backend::arm64
