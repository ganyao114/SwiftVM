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

    [[nodiscard]] bool IsDirect() const { return direct; }
    [[nodiscard]] bool PreserveAllLeaf() const { return preserve_all_leaf; }
    [[nodiscard]] bool FPCRTransparent() const { return fpcr_transparent; }
    [[nodiscard]] bool GeneralRegistersOnly() const { return general_registers_only; }
    [[nodiscard]] bool PreservesPinnedState() const { return preserves_pinned_state; }
    [[nodiscard]] bool ReadsGuestState() const;
    [[nodiscard]] bool WritesGuestState() const;
    [[nodiscard]] bool MayFault() const { return may_fault; }
    [[nodiscard]] bool MayReenter() const { return may_reenter; }
    [[nodiscard]] bool PreservesHostNZCV() const { return preserves_host_nzcv; }
    [[nodiscard]] bool RequiresGuestStatePublication() const;
    [[nodiscard]] bool RetainsPendingNZCV() const;
    [[nodiscard]] ir::UniformEffectId UniformEffects() const { return uniform_effects; }
    [[nodiscard]] bool ClobbersGPR(u32 code) const;
    [[nodiscard]] bool ClobbersFPR(u32 code) const;
    [[nodiscard]] bool ArgumentRequiresSlot(u32 code) const;
    [[nodiscard]] bool RequiresGPRCapture(u32 code, bool argument_source) const;
    [[nodiscard]] bool RequiresFPRCapture(u32 code) const;

private:
    bool direct{};
    bool preserve_all_leaf{};
    bool fpcr_transparent{};
    bool general_registers_only{};
    bool preserves_pinned_state{};
    ir::HelperGuestStateEffect guest_state_effect{ir::HelperGuestStateEffect::MayReadWrite};
    bool may_fault{true};
    bool may_reenter{true};
    bool preserves_host_nzcv{};
    ir::UniformEffectId uniform_effects{ir::UniformEffectId::Unknown};
    bool exact_register_clobbers{};
    u32 gpr_clobber_mask{};
    u32 fpr_clobber_mask{};
};

}  // namespace swift::runtime::backend::arm64
