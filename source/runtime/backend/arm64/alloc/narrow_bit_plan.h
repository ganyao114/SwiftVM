#pragma once
#include <map>
#include <unordered_set>
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::arm64 {
struct NarrowExtractExtension {
    ir::Inst* extract{}; ir::Value source{}; ir::Inst* shift{};
    u8 width{}; bool source_high_zero{};
    bool operator==(const NarrowExtractExtension&) const = default;
};
struct NarrowMaskedInput {
    ir::Inst* extract{}; ir::Value source{}; u8 width{};
    bool operator==(const NarrowMaskedInput&) const = default;
};
struct ShiftMaskedInput {
    ir::Inst* shift{}; ir::Value source{}; u8 offset{};
    bool operator==(const ShiftMaskedInput&) const = default;
};
struct NarrowBitInputs {
    std::unordered_set<const ir::Inst*> blocked_extracts, discarded, flagged;
    const BitVector* earlier_coverage{};
};
struct NarrowBitPlan {
    std::map<ir::Inst*, NarrowExtractExtension> extensions;
    std::map<ir::Inst*, ir::Inst*> extracts, extract_shifts;
    std::map<ir::Inst*, NarrowMaskedInput> masks;
    std::map<ir::Inst*, ir::Inst*> masked_extracts;
    std::map<ir::Inst*, ShiftMaskedInput> shift_masks;
    // Zero extensions whose narrow load result register is tied to the
    // extension result in the final allocation; emission may elide them.
    std::unordered_set<ir::Inst*> tied_load_extensions;
    std::map<ir::Inst*, ir::Inst*> masked_shifts;
};
NarrowBitPlan BuildNarrowBitPlan(ir::Block* block, const RegAlloc& allocation, const NarrowBitInputs& inputs);
}
