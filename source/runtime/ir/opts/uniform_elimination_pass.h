//
// Created by 甘尧 on 2024/6/20.
//

#pragma once

#include "runtime/ir/hir_builder.h"
#include "runtime/common/range_map.h"
#include "runtime/backend/reg_alloc.h"

#include <vector>

namespace swift::runtime::ir {

struct UniformRegister {
    Uniform uniform{};
    HostReg host_reg{};

    [[nodiscard]] bool Null() const {
        return uniform.GetType() == ValueType::VOID;
    }

    bool operator==(const UniformRegister& rhs) const { return uniform == rhs.uniform; }
    bool operator!=(const UniformRegister& rhs) const { return !(rhs == *this); }
};

struct UniformRange {
    u32 begin{};
    u32 end{};
};

struct UniformInfo {
    u32 uniform_size{};
    backend::GPRSMask uni_gprs{};
    backend::FPRSMask uni_fprs{};
    RangeMap<u32, UniformRegister> uniform_regs_map{};
    std::vector<UniformRange> xmm_uniform_ranges{};
    std::vector<UniformRange> loop_gpr_uniform_ranges{};
    std::vector<UniformRange> store_sink_uniform_ranges{};
    // Some backends poll interrupts/SMC at every AdvancePC and allow memory
    // helpers to observe State. Keep byte facts and stores local to those
    // observation intervals; legacy backends retain their existing policy.
    bool precise_observation_points{};
    // A Checkpoint holds real SSA snapshots for retirement exits, so the
    // AdvancePC marker itself need not invalidate byte facts or sink stores.
    bool retirement_snapshots{};

    [[nodiscard]] bool IsXmmUniformRange(u32 offset, u32 size) const {
        for (const auto& range : xmm_uniform_ranges) {
            if (offset >= range.begin && offset + size <= range.end) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool IsLoopGPRUniformRange(u32 offset, u32 size) const {
        for (const auto& range : loop_gpr_uniform_ranges) {
            if (offset >= range.begin && offset + size <= range.end) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool IsStoreSinkUniformRange(u32 offset, u32 size) const {
        if (IsXmmUniformRange(offset, size)) return true;
        for (const auto& range : store_sink_uniform_ranges) {
            if (offset >= range.begin && offset <= range.end && size <= range.end - offset) {
                return true;
            }
        }
        return false;
    }
};

class UniformEliminationPass {
public:
    static void Run(HIRBuilder *hir_builder, const UniformInfo &info, bool to_regs,
                    const FeatureSet& features);
    static void Run(HIRFunction *hir_function, const UniformInfo &info, bool to_regs,
                    const FeatureSet& features);
    static void Run(Block *block, const UniformInfo &config,
                    const FeatureSet& features, HIRFunction *hir_function = nullptr);
    // Explicit selector for byte-for-byte A/B tests in one process. Production
    // callers use the overload above, which reads SVM_UNIFORM_FAST once.
    static void Run(Block *block, const UniformInfo &config, bool fast_path,
                    const FeatureSet& features, HIRFunction *hir_function = nullptr);
    static void FinalizeStaticFPRMappings(Block* block, const UniformInfo& info);
    static void FinalizeStaticFPRMappings(HIRFunction* function, const UniformInfo& info);
};

}  // namespace swift::runtime::ir
