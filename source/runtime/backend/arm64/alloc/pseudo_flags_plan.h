#pragma once

#include <vector>
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::arm64 {
struct PseudoFlagsDemand {
    ir::Flags set{}, clear{};
    bool branch_only{};
    [[nodiscard]] bool Null() const {
        return set == ir::Flags::None && clear == ir::Flags::None && !branch_only;
    }
    [[nodiscard]] bool IsNZCV() const { return True(set & ir::Flags::NZCV); }
    [[nodiscard]] bool IsCV() const {
        return True(set & ir::Flags::CV) && False(set & ir::Flags::NZ) && False(clear & ir::Flags::NZ);
    }
    [[nodiscard]] bool IsNZ_ZeroCV() const { return True(set & ir::Flags::NZ) && True(clear & ir::Flags::CV); }
    [[nodiscard]] bool NeedsResultBits() const { return True(set & (ir::Flags::Parity | ir::Flags::AuxiliaryCarry)); }
};

class PseudoFlagsPlan {
public:
    // Called only while the shared block index is being constructed,
    // once per SaveFlags/BranchOnlyFlags instruction in the block walk.
    void RecordPseudo(const ir::Inst& pseudo, u16 first, u16 last);
    [[nodiscard]] PseudoFlagsDemand For(const ir::Inst* inst) const {
        const auto flags = PackedFor(inst);
        return {static_cast<ir::Flags>(flags & 0x7f), static_cast<ir::Flags>((flags >> 8) & 0x7f), (flags & 0x80) != 0};
    }
    // SaveFlags demand differs from BranchOnly demand, even for the same bit.
    [[nodiscard]] bool SavesParity(const ir::Inst* inst) const { return inst && (PackedFor(inst) & 0x8000) != 0; }
    [[nodiscard]] bool Empty() const { return packed.empty(); }
private:
    u16 PackedFor(const ir::Inst* inst) const {
        if (packed.empty() || inst->Id() < first_id || inst->Id() - first_id >= packed.size()) return 0;
        return packed[inst->Id() - first_id];
    }
    u16 first_id{};
    std::vector<u16> packed;
};
}
