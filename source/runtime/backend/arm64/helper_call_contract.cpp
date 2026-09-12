#include "runtime/backend/arm64/helper_call_contract.h"

#include "runtime/common/helper_abi.h"
#include "runtime/common/sse42str_call_abi.h"

namespace swift::runtime::backend::arm64 {

namespace {

bool LeafHelperABIEnabled(const FeatureSet& features) {
#if SVM_HAS_HELPER_PRESERVE_ALL
    return features.helper_leaf_abi;
#else
    return false;
#endif
}

constexpr bool GeneralRegistersOnlyABIEnabled() {
#if SVM_HAS_HELPER_GENERAL_REGS_ONLY
    return true;
#else
    return false;
#endif
}

HelperCallContract OpaqueContract(const FeatureSet& features) {
    return HelperCallContract::Resolve(ir::Lambda{ir::Value{}}, features);
}

}  // namespace

HelperCallContract HelperCallContract::Resolve(const ir::Lambda& lambda,
                                               const FeatureSet& features) {
    HelperCallContract contract{};
    contract.direct = !lambda.IsValue();
    if (!contract.direct) {
        return contract;
    }
    contract.preserve_all_leaf = LeafHelperABIEnabled(features) &&
                                 lambda.GetHelperABI() == ir::HelperABI::PreserveAllLeaf;
    contract.fpcr_transparent = lambda.GetHostFpEffect() == ir::HostFpEffect::FPCRTransparent;
    contract.general_registers_only =
            GeneralRegistersOnlyABIEnabled() &&
            lambda.GetHostRegisterEffect() == ir::HostRegisterEffect::GeneralOnly;
    contract.preserves_pinned_state =
            lambda.GetHostRegisterEffect() == ir::HostRegisterEffect::PreservesPinnedState;
    contract.guest_state_effect = lambda.GetHelperGuestStateEffect();
    contract.may_fault = lambda.GetHelperFaultEffect() == ir::HelperFaultEffect::MayFault;
    contract.may_reenter =
            lambda.GetHelperReentryEffect() == ir::HelperReentryEffect::MayReenter;
    contract.preserves_host_nzcv =
            lambda.GetHostFlagsEffect() == ir::HostFlagsEffect::PreservesNZCV;
    contract.uniform_effects = lambda.GetUniformEffectId();
    return contract;
}

std::optional<HelperCallContract> HelperCallContract::Resolve(const ir::Inst& inst,
                                                              const FeatureSet& features) {
    switch (inst.GetOp()) {
        case ir::OpCode::CallLambda:
            return Resolve(inst.GetArg<ir::Lambda>(0), features);
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
            return OpaqueContract(features);
        case ir::OpCode::Sse42Str: {
            HelperCallContract contract{};
            contract.direct = true;
            contract.fpcr_transparent = true;
            contract.preserves_pinned_state = true;
            contract.guest_state_effect = ir::HelperGuestStateEffect::None;
            contract.may_fault = false;
            contract.may_reenter = false;
            contract.uniform_effects = ir::UniformEffectId::None;
            contract.exact_register_clobbers = true;
            const auto imm = static_cast<u8>(inst.GetArg<ir::Imm>(2).Get());
            using ABI = swift::x86::Sse42StrVectorCallABI;
            if (ABI::Supports(imm)) {
                contract.gpr_clobber_mask = ABI::GPRClobbers(imm);
                contract.fpr_clobber_mask =
                        ABI::FPRClobbers(imm) | ABI::ArgumentFPRClobbers;
            }
            return contract;
        }
        default:
            return std::nullopt;
    }
}

bool HelperCallContract::InstructionClobbersGPR(const ir::Inst& inst,
                                                u32 code,
                                                const FeatureSet& features) {
    const auto contract = Resolve(inst, features);
    return contract && contract->ClobbersGPR(code);
}

bool HelperCallContract::ReadsGuestState() const {
    return guest_state_effect == ir::HelperGuestStateEffect::MayReadWrite ||
           guest_state_effect == ir::HelperGuestStateEffect::ReadOnly;
}

bool HelperCallContract::WritesGuestState() const {
    return guest_state_effect == ir::HelperGuestStateEffect::MayReadWrite ||
           guest_state_effect == ir::HelperGuestStateEffect::WriteOnly;
}

bool HelperCallContract::RequiresGuestStatePublication() const {
    return ReadsGuestState() || WritesGuestState() || may_fault || may_reenter;
}

bool HelperCallContract::RetainsPendingNZCV() const {
    return preserves_host_nzcv && !RequiresGuestStatePublication();
}

bool HelperCallContract::ClobbersGPR(u32 code) const {
    if (exact_register_clobbers) {
        return code < 32 && (gpr_clobber_mask & (1u << code)) != 0;
    }
    if (code >= 19) {
        return false;
    }
    bool clobbered = true;
    if (preserve_all_leaf) {
        clobbered &= code <= 8 || code >= 16;
    }
    if (preserves_pinned_state) {
        clobbered &= code <= 2 || code == 11 || code >= 16;
    }
    return clobbered;
}

bool HelperCallContract::ClobbersFPR(u32 code) const {
    if (exact_register_clobbers) {
        return code < 32 && (fpr_clobber_mask & (1u << code)) != 0;
    }
    if (general_registers_only) {
        return false;
    }
    bool clobbered = true;
    if (preserve_all_leaf) {
        clobbered &= code <= 7;
    }
    if (preserves_pinned_state) {
        clobbered &= code < 16;
    }
    return clobbered;
}

bool HelperCallContract::ArgumentRequiresSlot(u32 code) const {
    if (exact_register_clobbers) {
        return ClobbersGPR(code);
    }
    return code <= 17 && (!preserves_pinned_state || code <= 2 || code >= 16);
}

bool HelperCallContract::RequiresGPRCapture(u32 code, bool argument_source) const {
    if (exact_register_clobbers) {
        return ClobbersGPR(code);
    }
    return code <= 17 &&
           (ClobbersGPR(code) || (argument_source && (!preserves_pinned_state || code <= 2 ||
                                                      code == 11 || code >= 16)));
}

bool HelperCallContract::RequiresFPRCapture(u32 code) const {
    if (exact_register_clobbers) {
        return ClobbersFPR(code);
    }
    return code < 32 && ClobbersFPR(code);
}

}  // namespace swift::runtime::backend::arm64
