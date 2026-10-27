#pragma once

#include <optional>
#include <span>
#include "admission.h"
#include "edges.h"

namespace swift::runtime::backend::arm64 {

struct LoweredEdges {
    UnitCertificate certificate;
    EdgeActions actions;
};

struct BackedgeInputs {
    const FeatureSet& features;
    bool self_enabled{}, region_enabled{}, canonical_carry{};
    std::span<const u64> flags_dead_targets;
    [[nodiscard]] bool FlagsDead(u64 location) const;
};
struct BackedgeSelection {
    BackedgeAction action;
    std::optional<PFAFAction> pfaf;
};

[[nodiscard]] LoweredEdges LowerUnitEdges(const ir::Block*,
                                          const ir::HIRFunction*,
                                          compile::AllocationView,
                                          const UnitActions&,
                                          const Config&,
                                          const FeatureSet&);
[[nodiscard]] std::optional<BackedgeSelection> LowerBackedge(const ir::Block&,
                                                             const BackedgeInputs&);
[[nodiscard]] std::optional<PFAFAction> LowerPFAF(const ir::Block&,
                                                  bool dead_successor,
                                                  const ir::Inst* producer,
                                                  const ir::Inst* final_save);

}  // namespace swift::runtime::backend::arm64
