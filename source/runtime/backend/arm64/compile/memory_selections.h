#pragma once

#include "actions.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
}
namespace swift::runtime::backend::arm64 {

// Immutable preparation output. Only compact decisions survive declaration;
// publication links and the mutable row index do not cross this boundary.
class MemorySelections final {
public:
    MemorySelections() = default;
    MemorySelections(const MemorySelections&) = delete;
    MemorySelections& operator=(const MemorySelections&) = delete;
    MemorySelections(MemorySelections&&) = default;
    MemorySelections& operator=(MemorySelections&&) = default;
    [[nodiscard]] const MemoryTransferAction* Transfer(const ir::Inst*,
                                                       MemoryTransferAction::Kind) const;
    [[nodiscard]] const AddressAction* Address(const ir::Inst*) const;
    [[nodiscard]] bool IsLoadUpdateInstruction(const ir::Inst*) const;
    [[nodiscard]] std::span<const MemoryTransferAction> Transfers() const { return transfers; }
    [[nodiscard]] std::span<const AddressAction> Addresses() const { return addresses; }

private:
    friend class MemorySelectionBuilder;
    [[nodiscard]] bool Owns(const ir::Inst*) const;
    const compile::BlockAnalysisIndex* analysis{};
    std::vector<MemoryTransferAction> transfers;
    std::vector<AddressAction> addresses;
    std::vector<u16> load_publications;
};

// One active block borrows the shared row workspace. Record keeps the first
// selected payload; Finish releases the borrow and returns sorted decisions.
class MemorySelectionBuilder final : DeleteCopyAndMove {
public:
    explicit MemorySelectionBuilder(compile::CompileWorkspace& workspace) : workspace(workspace) {}
    void Reset(const compile::BlockAnalysisIndex&);
    void Record(MemoryTransferAction);
    void Record(AddressAction);
    void LinkPublication(u16 update, u16 publication);
    [[nodiscard]] const MemoryTransferAction* Transfer(const ir::Inst*,
                                                       MemoryTransferAction::Kind) const;
    [[nodiscard]] const AddressAction* Address(const ir::Inst*) const;
    [[nodiscard]] bool IsLoadPublication(const ir::Inst*) const;
    [[nodiscard]] bool IsLoadUpdateInstruction(const ir::Inst*) const;
    [[nodiscard]] std::optional<u16> PublicationForUpdate(const ir::Inst*) const;
    [[nodiscard]] MemorySelections Finish();

private:
    enum SlotKind : u8 { TransferSlot, AddressSlot, LoadPublicationSlot };
    struct Slot {
        u16 instruction;
        u16 row;
        u16 publication{UINT16_MAX};
        SlotKind kind;
    };
    static_assert(sizeof(Slot) == 8);
    Slot& Insert(u16 instruction, u16 row, SlotKind kind);
    [[nodiscard]] const Slot* Find(const ir::Inst*) const;
    compile::CompileWorkspace& workspace;
    std::optional<compile::InstructionRowScope> index;
    MemorySelections selections;
    std::vector<Slot> slots;
};
}  // namespace swift::runtime::backend::arm64
