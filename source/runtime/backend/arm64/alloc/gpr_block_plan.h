#pragma once

#include <map>
#include "aarch64/operands-aarch64.h"
#include "frozen_gpr_plan.h"
#include "fpr_allocation_plan.h"
#include "scalar_fpr_publication_plan.h"
#include "memory_lowering_plan.h"
#include "consumer_plan.h"
#include "boolean_select_plan.h"
#include "flags_token_plan.h"
#include "flags_only_spill_plan.h"
#include "pseudo_flags_plan.h"
#include "integer_branch_plan.h"
#include "narrow_integer_flags_plan.h"
#include "scalar_preparation_plan.h"
#include "narrow_bit_plan.h"
#include "region_preparation_plan.h"
#include "backedge_flags_plan.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

struct GPRMemoryUpdate {
    ir::Inst* memory{};
    ir::Inst* publication{};
    vixl::aarch64::XRegister base{};
    s64 offset{};
};
struct GPRLoadUpdate {
    ir::Inst* load{};
    ir::Inst* update{};
    ir::Inst* publication{};
    ir::Inst* base_read{};
    u16 target{};
    s16 offset{};
    bool operator==(const GPRLoadUpdate&) const = default;
};
struct GPRMemoryAddress {
    ir::Inst* memory{};
    u16 target{};
    s64 offset{};
};
enum class GPRPublicationKind : u8 { Skip, Copy, Transfer, BiasedUpdate, Value };
struct GPRPublicationAction {
    GPRPublicationKind kind{GPRPublicationKind::Skip};
    ir::Value source{};
    std::optional<u16> source_home{};
    u16 target{};
    u8 width{}, bit_offset{};
    bool clear_high{}, adjust_resident_home{}, adopts_spill{};
    u64 decrement{};
};

// Allocator storage and emission expose only shared_ptr<const GPRBlockPlan>.
// The target builder has the sole mutable reference until final verification.
struct GPRBlockPlan {
    PseudoFlagsPlan pseudo_flags;
    std::shared_ptr<const FrozenGPRPlan> pinned;
    std::shared_ptr<const FPRAllocationPlan> fpr;
    std::shared_ptr<const ScalarFPRPublicationPlan> scalar_fpr;
    std::shared_ptr<const MemoryLoweringPlan> memory;
    std::shared_ptr<const ConsumerPlan> consumers;
    std::shared_ptr<const BooleanSelectPlan> boolean_selects;
    std::shared_ptr<const FlagsTokenPlan> flags_tokens;
    FlagsOnlySpillPlan flags_only_spills;
    std::shared_ptr<const IntegerBranchPlan> integer_branch;
    std::shared_ptr<const IntegerFlagPreparationPlan> integer_flags;
    std::shared_ptr<const ScalarPreparationPlan> scalar_preparation;
    std::shared_ptr<const NarrowBitPlan> narrow_bits;
    std::shared_ptr<const RegionPreparationPlan> region;
    std::shared_ptr<const BackedgeFlagsPlan> backedge_flags;
    std::map<ir::Inst*, GPRLoadUpdate> load_updates, load_update_instructions;
    std::map<ir::Inst*, u16> memory_values;
    std::map<ir::Inst*, GPRMemoryAddress> addresses;
    std::map<ir::Inst*, GPRPublicationAction> publications;
    std::map<ir::Inst*, GPRMemoryUpdate> preindex_updates, biased_updates;
};

// Translator construction does not yet have an allocation-bound block. This
// immutable empty view is shared until declaration/import supplies that plan.
inline const GPRBlockPlan& EmptyGPRBlockPlan() {
    static const GPRBlockPlan empty;
    return empty;
}
}
