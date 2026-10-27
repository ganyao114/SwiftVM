#pragma once

#include <array>
#include <memory>
#include <span>

#include "constraints.h"
#include "fixed_validation.h"
#include "fpr_inputs.h"
#include "gpr_inputs.h"
#include "runtime/backend/register_mask.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/args.h"
#include "scratch.h"
#include "spill_inputs.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile {

struct AllocationBudget {
    u32 gpr_reserve{}, fpr_reserve{};
    bool allow_zero_gpr{}, incremental_rungs{};
};

// Target preparation produces these tables before entering the common scan.
// The request owns them as const data across every physical retry. They carry
// no target callbacks, emitters, mutable mapping or independently owned IR.
class AllocationInputs final {
public:
    struct PoolPolicy {
        backend::GPRSMask pool;
        AllocationBudget budget;
        bool fixed_class{};
        ScratchNeed max_scratch{};
        bool fixed_publications{};
        bool gpr_coalescing{};
        u8 width_component_sites{1};
    };
    struct Clobber {
        u16 instruction;
        u32 mask;
    };
    struct Tie {
        u16 value, source;
        bool allow_fixed_owner{}, integer_width{};
    };
    struct Preference {
        u16 value, home;
    };
    struct Publication {
        u16 instruction, source, home;
    };
    struct ScratchProfile {
        ScratchNeed need{};
        std::array<u8, 2> outside_value_pool{};
        std::array<u8, 2> spill_outside_value_pool{};
        u8 reload_headroom{}, overlap_pools{};
        bool operator==(const ScratchProfile&) const = default;
    };
    // Sorted change points; one profile applies until the next instruction.
    struct ScratchSite {
        u16 instruction;
        ScratchProfile profile;
    };

    AllocationInputs(const ir::Block*,
                     const FeatureSet&,
                     backend::GPRSMask,
                     backend::FPRSMask,
                     AllocationBudget);
    AllocationInputs(const ir::HIRFunction*,
                     const FeatureSet&,
                     backend::GPRSMask,
                     backend::FPRSMask,
                     AllocationBudget);
    AllocationInputs(const AllocationInputs&) = delete;
    AllocationInputs& operator=(const AllocationInputs&) = delete;
    AllocationInputs(AllocationInputs&&) noexcept = default;

    [[nodiscard]] const PoolPolicy& PolicyFor(backend::GPRSMask pool) const;
    [[nodiscard]] bool UsesDeclaredPool(backend::GPRSMask pool) const;
    [[nodiscard]] std::span<const Tie> TiesFor(u32 value) const;
    [[nodiscard]] u16 PreferredGPR(u32 value) const;
    [[nodiscard]] bool DirectlyFeedsMemory(u32 value) const;
    [[nodiscard]] bool Publishes(u32 instruction, u32 value, u16 home) const;
    [[nodiscard]] std::span<const Publication> PublicationsFor(const ir::Block*) const;
    [[nodiscard]] ScratchRequirements ScratchFor(u32 instruction, backend::GPRSMask pool) const;
    void Validate(const ir::Block*,
                  const ir::HIRFunction*,
                  backend::GPRSMask,
                  backend::FPRSMask) const;

    const ir::Block* const block;
    const ir::HIRFunction* const function;
    const FeatureSet features;
    const u32 instruction_count;
    const backend::FPRSMask fprs;
    // Guest-home declarations can grow a caller's reserved GPR set on attempt
    // one. Describe both actual pools; retries must not silently reuse a
    // policy derived from the earlier pool.
    std::array<PoolPolicy, 2> pools;
    u32 declared_gprs{};
    u32 fixed_gpr_homes{};
    Vector<Clobber> clobbers, declared_clobber_deltas;
    Vector<Tie> ties;
    Vector<Preference> preferences;
    Vector<Publication> publications;
    Vector<u16> memory_feeders;
    Vector<FixedGPRInput> fixed;
    Vector<FixedRegisterRead> fixed_reads;
    std::unique_ptr<const GPRInputs> gpr;
    Vector<ScratchSite> scratch;
    u32 reload_bound{};
    std::unique_ptr<const FPRInputs> fpr;
    std::unique_ptr<const SpillInputs> spill;
    std::unique_ptr<const FixedValidationInputs> fixed_validation;
};

}  // namespace swift::runtime::compile
