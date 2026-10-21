#pragma once
#include <vector>
#include "runtime/backend/reg_alloc.h"
namespace swift::runtime::backend::arm64 {
class BlockAnalysisIndex;
void SelectPshufdDirect(const BlockAnalysisIndex& analysis, RegAlloc& allocation);
struct FPRAllocationPlan {
    bool valid{true};
    const ir::Inst* rejected{};
    enum Action : u8 { Read = 1, Write = 2, ScalarTie = 4, AesTie = 8, ShuffleTie = 16, Pshufd = 32 };
    u32 first_id{};
    std::vector<u8> actions;
    std::vector<u16> controls;
    u8 selected{};
    void Certify(const ir::Inst* inst, Action action) {
        actions[inst->Id() - first_id] |= action;
        selected |= action;
    }
    bool Has(const ir::Inst* inst, Action action) const {
        return inst && inst->Id() >= first_id && inst->Id() - first_id < actions.size() &&
               (actions[inst->Id() - first_id] & action);
    }
    std::optional<u8> Control(const ir::Inst* inst) const {
        if (!Has(inst, Pshufd) || inst->Id() - first_id >= controls.size()) return std::nullopt;
        return static_cast<u8>(controls[inst->Id() - first_id]);
    }
    void SetControl(const ir::Inst* inst, u8 control) {
        if (controls.empty()) controls.resize(actions.size(), UINT16_MAX);
        controls[inst->Id() - first_id] = control;
        Certify(inst, Pshufd);
    }
};
FPRAllocationPlan BuildFPRAllocationPlan(ir::Block* block, const RegAlloc& allocation,
        const FeatureSet& features, bool scalar_insert, bool scalar_tie,
        const BlockAnalysisIndex* analysis = nullptr);
}
