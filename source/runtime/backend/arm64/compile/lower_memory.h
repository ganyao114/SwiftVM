#pragma once

#include <optional>
#include "actions.h"
#include "memory_selections.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
}  // namespace swift::runtime::compile
namespace swift::runtime::backend::arm64 {

class GPRResidences;

struct MemoryOptions final {
    bool narrow_fuse{}, address_tie{}, biased{}, rcpc{};
};

// Preparation-only output. Writes are handed to residence verification, not
// retained in UnitActions. Only compact access decisions and coverage survive.
struct MemoryActions final {
    struct Write final {
        u16 instruction, update, consumed_value;
        u8 base;
        bool operator==(const Write&) const = default;
    };
    std::vector<MemoryAction> actions;
    std::vector<Write> writes;
    std::vector<u16> covered;
    [[nodiscard]] const MemoryAction* Find(u32 id) const;
    [[nodiscard]] const MemoryAction* Find(u32 id, MemoryAction::Flag) const;
};

namespace detail {
constexpr bool IsMemoryAccess(ir::OpCode op) {
    return op == ir::OpCode::LoadMemory || op == ir::OpCode::StoreMemory ||
           op == ir::OpCode::LoadMemoryTSO || op == ir::OpCode::StoreMemoryTSO;
}

// Stack-local candidates for one access. Preparation prunes physical conflicts
// and prepares the required graph before handing them to read-only lowering.
// These records never enter a prepared unit or survive the current access.
struct MemoryCandidates final {
    struct PostIndex final {
        const ir::Inst* update;
        ir::Value source;
        s16 offset;
        u8 base;
    };
    const ir::Inst* memory{};
    const ir::Inst* address{};
    MemoryAction action{};
    std::optional<PostIndex> post_index;
    bool cover_address{}, narrow{};
    [[nodiscard]] bool Empty() const {
        return !action.bits && !post_index && !cover_address && !narrow;
    }
};

std::optional<MemoryCandidates> SelectMemoryCandidates(const ir::Inst&,
                                                       compile::AllocationView,
                                                       const MemorySelections&,
                                                       MemoryOptions,
                                                       const compile::BlockAnalysisIndex&);
void LowerMemory(const ir::Block*,
                 const MemoryCandidates&,
                 compile::AllocationView,
                 const FeatureSet&,
                 const MemorySelections&,
                 const GPRResidences&,
                 MemoryOptions,
                 const compile::BlockAnalysisIndex&,
                 MemoryActions&);
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
