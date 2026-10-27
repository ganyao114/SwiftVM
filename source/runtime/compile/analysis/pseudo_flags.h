#pragma once

#include <span>
#include <vector>
#include "runtime/ir/instr.h"

namespace swift::runtime::compile {
struct PseudoFlagsDemand {
    ir::Flags set{}, clear{};
    bool branch_only{};
    [[nodiscard]] bool Null() const {
        return set == ir::Flags::None && clear == ir::Flags::None && !branch_only;
    }
    [[nodiscard]] bool IsNZCV() const { return True(set & ir::Flags::NZCV); }
    [[nodiscard]] bool IsCV() const {
        return True(set & ir::Flags::CV) && False(set & ir::Flags::NZ) &&
               False(clear & ir::Flags::NZ);
    }
    [[nodiscard]] bool IsNZ_ZeroCV() const {
        return True(set & ir::Flags::NZ) && True(clear & ir::Flags::CV);
    }
    [[nodiscard]] bool NeedsResultBits() const {
        return True(set & (ir::Flags::Parity | ir::Flags::AuxiliaryCarry));
    }
};

// Borrows sealed packed words. Prepared targets own their copy independently
// of the semantic index, so no use graph has to survive into emission.
struct PseudoFlagsView {
    const u16* words{};
    u32 count{};
    u16 first_id{};
    [[nodiscard]] u16 PackedFor(const ir::Inst* inst) const {
        if (!inst || inst->Id() < first_id || inst->Id() - first_id >= count) return 0;
        return words[inst->Id() - first_id];
    }
    [[nodiscard]] PseudoFlagsDemand For(const ir::Inst* inst) const {
        const auto flags = PackedFor(inst);
        return {static_cast<ir::Flags>(flags & 0x7f),
                static_cast<ir::Flags>((flags >> 8) & 0x7f),
                (flags & 0x80) != 0};
    }
    [[nodiscard]] bool SavesParity(const ir::Inst* inst) const {
        return (PackedFor(inst) & 0x8000) != 0;
    }
    [[nodiscard]] bool Empty() const { return count == 0; }
    [[nodiscard]] std::span<const u16> Words() const { return {words, count}; }
};

class PseudoFlagsPlan {
public:
    // Called only while the shared block index is being constructed,
    // once per SaveFlags/BranchOnlyFlags instruction in the block walk.
    void RecordPseudo(const ir::Inst& pseudo, u16 first, u16 last);
    [[nodiscard]] PseudoFlagsDemand For(const ir::Inst* inst) const { return View().For(inst); }
    // SaveFlags demand differs from BranchOnly demand, even for the same bit.
    [[nodiscard]] bool SavesParity(const ir::Inst* inst) const { return View().SavesParity(inst); }
    [[nodiscard]] bool Empty() const { return packed.empty(); }
    [[nodiscard]] PseudoFlagsView View() const {
        return {packed.data(), static_cast<u32>(packed.size()), first_id};
    }

private:
    u16 first_id{};
    std::vector<u16> packed;
};
}  // namespace swift::runtime::compile
