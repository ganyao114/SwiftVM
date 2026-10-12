#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "cfg_register_plan.h"

namespace swift::runtime::backend::x64 {

// Two-producer, ordinary-body native-CF transport. Non-owning IR identities
// remain valid only while the privately prepared body is unchanged/alive.
// This does not change the immutable CFG allocation or metadata-home proofs.
class CarryChainPlan {
public:
    struct Incoming {
        const ir::Inst* first{};
        // x86 ADD/ADC CF = guest C; SUB/SBB CF = !guest C.
        bool complement{};
    };
    static CarryChainPlan Build(const CfgRegisterPlan& graph,
                               const backend::FunctionPlan* function,
                               std::optional<u64> uniform_extent);
    const Incoming* IncomingFor(const ir::Inst* second) const noexcept;
    bool DefersPublication(const ir::Inst* marker) const noexcept {
        return deferred_publications.contains(marker);
    }
    size_t ChainCount() const noexcept { return incoming.size(); }
private:
    std::unordered_map<const ir::Inst*, Incoming> incoming;
    std::unordered_set<const ir::Inst*> deferred_publications;
};

} // namespace swift::runtime::backend::x64
