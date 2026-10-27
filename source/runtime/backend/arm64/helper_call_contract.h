#pragma once

#include <optional>

#include "runtime/common/svm_config.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::arm64 {

class HelperCallContract final {
public:
    [[nodiscard]] static HelperCallContract Resolve(const ir::Lambda& lambda,
                                                    const FeatureSet& features);
    [[nodiscard]] static std::optional<HelperCallContract> Resolve(const ir::Inst& inst,
                                                                   const FeatureSet& features);
    [[nodiscard]] static bool InstructionClobbersGPR(const ir::Inst& inst,
                                                     u32 code,
                                                     const FeatureSet& features);

    [[nodiscard]] bool IsDirect() const { return state.direct; }
    [[nodiscard]] bool PreserveAllLeaf() const { return preserve_all_leaf; }
    [[nodiscard]] bool FPCRTransparent() const { return fpcr_transparent; }
    [[nodiscard]] bool GeneralRegistersOnly() const { return general_registers_only; }
    [[nodiscard]] bool PreservesPinnedState() const { return preserves_pinned_state; }
    [[nodiscard]] bool ReadsGuestState() const { return state.ReadsGuestState(); }
    [[nodiscard]] bool WritesGuestState() const { return state.WritesGuestState(); }
    [[nodiscard]] bool MayFault() const { return state.MayFault(); }
    [[nodiscard]] bool MayReenter() const { return state.MayReenter(); }
    [[nodiscard]] bool PreservesHostNZCV() const { return preserves_host_nzcv; }
    [[nodiscard]] bool RequiresGuestStatePublication() const {
        return state.RequiresGuestStatePublication();
    }
    [[nodiscard]] bool RetainsPendingNZCV() const;
    [[nodiscard]] ir::UniformEffectId UniformEffects() const { return state.uniform; }
    [[nodiscard]] bool ClobbersGPR(u32 code) const;
    [[nodiscard]] bool ClobbersFPR(u32 code) const;
    [[nodiscard]] bool ArgumentRequiresSlot(u32 code) const;
    [[nodiscard]] bool RequiresGPRCapture(u32 code, bool argument_source) const;
    [[nodiscard]] bool RequiresFPRCapture(u32 code) const;

private:
    ir::HelperStateEffects state{};
    bool preserve_all_leaf{};
    bool fpcr_transparent{};
    bool general_registers_only{};
    bool preserves_pinned_state{};
    bool preserves_host_nzcv{};
    bool exact_register_clobbers{};
    u32 gpr_clobber_mask{};
    u32 fpr_clobber_mask{};
};

}  // namespace swift::runtime::backend::arm64
