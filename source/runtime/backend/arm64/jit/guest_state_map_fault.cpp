#include "guest_state_map.h"

#include <algorithm>

namespace swift::runtime::backend::arm64 {

bool GuestStateMap::NeedsFaultCaptures() const {
    ASSERT(block);
    for (const auto& publication : block->GetInstList()) {
        if (publication.GetOp() != ir::OpCode::SetHostGPR ||
            publication.GetArg<ir::Imm>(2).Get() != 0) {
            continue;
        }
        auto* extend = publication.GetArg<ir::Value>(0).Def();
        if (extend && extend->GetOp() == ir::OpCode::ZeroExtend32To64) {
            auto* producer = extend->GetArg<ir::Value>(0).Def();
            if (producer && producer->GetOp() == ir::OpCode::SelectZero) {
                return true;
            }
        }
    }
    return false;
}

void GuestStateMap::CaptureFaultCapture(
        const ir::Inst& boundary,
        const ActiveState& active,
        const WidthFacts& width_facts) {
    fault_width_captures.push_back({&boundary, width_facts});
    for (u32 home = 0; home < active.homes.size(); ++home) {
        for (auto* version : active.homes[home]) {
            const auto found = active.versions.find(version);
            if (found == active.versions.end()) {
                continue;
            }
            for (const auto& candidate : found->second) {
                if (candidate.location.home == home) {
                    fault_capture_values.push_back(
                            {&boundary, version, candidate.location});
                }
            }
        }
    }
}

bool GuestStateMap::FaultCaptureContains(
        const ir::Inst& boundary,
        u32 home,
        ir::Inst* version,
        bool require_zero_above_32) const {
    if (!version) {
        return false;
    }
    const bool width_matches = !require_zero_above_32 ||
            std::ranges::any_of(fault_width_captures, [&](const auto& capture) {
                return capture.boundary == &boundary &&
                       home < capture.extension_facts.size() &&
                       capture.extension_facts[home].KnownZeroAbove(32);
            });
    return width_matches &&
           std::ranges::any_of(fault_capture_values, [&](const auto& capture) {
               return capture.boundary == &boundary &&
                      capture.version == version && capture.location.home == home;
           });
}

}  // namespace swift::runtime::backend::arm64
