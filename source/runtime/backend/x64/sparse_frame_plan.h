#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "cfg_register_plan.h"

namespace swift::runtime::backend::x64 {

enum class SparseHomeDecision { Retained, Control, RegisterOnly, UnusedScalar, Rematerialized };
struct SparseFrameStats {
    u32 home_count{}, retained_homes{}, omitted_control{}, omitted_register{}, omitted_unused{}, home_bytes{};
    u32 reused_scalar_homes{}, shared_scalar_home_slots{};
    u32 omitted_rematerialized{};
};

// Only a fresh complete CFG allocation can authorize omissions. Every
// retained home uses an aligned 32-byte slot. Only audited scalar value homes
// with disjoint CFG lifetimes may share; metadata/raw/Phi/scratch stay unique.
class SparseFramePlan {
public:
    static SparseFramePlan Build(const ir::Block* body,
                                 const backend::FunctionPlan* function,
                                 const CfgRegisterPlan& registers,
                                 const std::unordered_set<ir::Inst*>& flag_producers,
                                 bool rematerialization = true);
    bool Enabled() const noexcept { return enabled; }
    bool RematerializationEnabled() const noexcept { return enabled && scalar_rematerialization_enabled; }
    // Caller supplies the same fresh full allocation and final IR authority.
    // Checks eligibility, all historical owners, layout and census separately.
    bool Validate(const ir::Block* body, const backend::FunctionPlan* function,
                  const CfgRegisterPlan& registers,
                  const std::unordered_set<ir::Inst*>& flag_producers,
                  bool rematerialization = true) const;
    std::optional<u32> Home(const ir::Inst* inst) const noexcept;
    std::optional<u64> RematerializedValue(const ir::Inst* inst) const noexcept;
    SparseHomeDecision Decision(const ir::Inst* inst) const noexcept;
    const SparseFrameStats& Statistics() const noexcept { return stats; }

private:
    struct HomeSlot {
        u32 offset{};
        bool scalar_pool{};
        std::vector<const ir::Inst*> owners;
    };
    bool enabled{};
    bool scalar_rematerialization_enabled{};
    const ir::Block* body{};
    SparseFrameStats stats;
    std::unordered_map<const ir::Inst*, u32> homes;
    std::unordered_map<const ir::Inst*, u64> rematerialized_values;
    std::unordered_map<const ir::Inst*, SparseHomeDecision> decisions;
    std::vector<HomeSlot> slots;
};
} // namespace swift::runtime::backend::x64

