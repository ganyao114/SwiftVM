#include "single_instruction_admission.h"

#include <limits>
#include <unordered_set>
#include "cpu.h"
#include "runtime/ir/block.h"

namespace swift::arm64 {
namespace {
namespace ir = runtime::ir;

bool OnlyArguments(const ir::Inst& inst, int count) {
    for (int slot = count; slot < ir::Inst::max_args; ++slot) {
        if (!inst.ArgAt(slot).IsVoid()) return false;
    }
    return true;
}

const ir::Inst* PriorU64(ir::Arg& arg, const std::unordered_set<const ir::Inst*>& prior) {
    if (!arg.IsValue()) return nullptr;
    const auto value = arg.Get<ir::Value>();
    const auto* definition = value.Def();
    // Check membership before dereferencing a possibly foreign SSA pointer.
    if (!definition || !prior.contains(definition)) return nullptr;
    if (value.Type() != ir::ValueType::U64 || definition->ReturnType() != ir::ValueType::U64) return nullptr;
    return definition;
}

bool ConstantU64(const ir::Inst& inst, u64& result) {
    if (inst.GetOp() != ir::OpCode::LoadImm || !inst.ArgAt(0).IsImm() || !OnlyArguments(inst, 1)) return false;
    const auto immediate = inst.ArgAt(0).Get<ir::Imm>();
    if (immediate.GetType() != ir::ValueType::U64) return false;
    result = immediate.Get();
    return true;
}

bool ArmContext(const ir::Inst& inst) {
    if (inst.GetOp() != ir::OpCode::GetUniformAddress || !inst.ArgAt(0).IsImm() || !OnlyArguments(inst, 1)) return false;
    const auto immediate = inst.ArgAt(0).Get<ir::Imm>();
    return immediate.GetType() == ir::ValueType::U32 && immediate.Get() == 0;
}

bool DecoderTraits(const ir::Lambda& lambda) {
    // Match the audited CallHostFPFree contract exactly. A purportedly pure
    // call or a PreserveAllLeaf annotation cannot authorize a different ABI.
    return lambda.GetUniformEffectId() == ir::UniformEffectId::Unknown &&
           lambda.GetHelperABI() == ir::HelperABI::NormalAAPCS &&
           lambda.GetHostFpEffect() == ir::HostFpEffect::FPCRTransparent &&
           lambda.GetHostRegisterEffect() == ir::HostRegisterEffect::MayTouchSIMD &&
           lambda.GetHelperGuestStateEffect() == ir::HelperGuestStateEffect::MayReadWrite &&
           lambda.GetHelperFaultEffect() == ir::HelperFaultEffect::MayFault &&
           lambda.GetHelperReentryEffect() == ir::HelperReentryEffect::MayReenter &&
           lambda.GetHostFlagsEffect() == ir::HostFlagsEffect::MayTouch;
}

bool KnownHelper(u64 target, u32 word) {
    using Query = bool (*)(u64, u32) noexcept;
    static constexpr Query queries[] = {
        &detail::AdmitScalarFP,
        &detail::AdmitScalarPairwiseFP,
        &detail::AdmitVectorFP16Unary,
        &detail::AdmitFP16Binary,
        &detail::AdmitVectorFPBinary,
        &detail::AdmitVectorUnary,
        &detail::AdmitFPCompare,
        &detail::AdmitFPInteger,
        &detail::AdmitFixedSIMD,
        &detail::AdmitRangeRound,
        &detail::AdmitPrecision,
        &detail::AdmitEstimate,
        &detail::AdmitReduction,
        &detail::AdmitScalarFP16Misc,
        &detail::AdmitVectorFMA,
        &detail::AdmitExtendedFP,
        &detail::AdmitIndexedFP,
    };
    for (const auto query : queries) {
        if (query(target, word)) return true;
    }
    return false;
}

bool AdmittedCall(const ir::Inst& call, u64 pc, const std::unordered_set<const ir::Inst*>& prior) {
    if (call.ReturnType() != ir::ValueType::U64 || !call.ArgAt(0).IsLambda() || !OnlyArguments(call, 4)) return false;
    const auto& lambda = call.ArgAt(0).Get<ir::Lambda>();
    if (!lambda.IsImm() || !DecoderTraits(lambda)) return false;
    const auto target = lambda.GetImm();
    if (target.GetType() != ir::ValueType::U64) return false;
    const auto* context = PriorU64(call.ArgAt(1), prior);
    const auto* instruction = PriorU64(call.ArgAt(2), prior);
    const auto* guest_pc = PriorU64(call.ArgAt(3), prior);
    u64 word{}, helper_pc{};
    return context && instruction && guest_pc && ArmContext(*context) &&
           ConstantU64(*instruction, word) && word <= std::numeric_limits<u32>::max() &&
           ConstantU64(*guest_pc, helper_pc) && helper_pc == pc &&
           KnownHelper(target.Get(), static_cast<u32>(word));
}

bool AdmittedInterruptStore(const ir::Inst& inst, const std::unordered_set<const ir::Inst*>& prior) {
    if (inst.ArgAt(0).GetType() != ir::ArgType::Uniform) return false;
    const auto uniform = inst.ArgAt(0).Get<ir::Uniform>();
    if (uniform.GetOffset() != offsetof(ThreadContext64, padding)) return true;
    if (uniform.GetType() != ir::ValueType::U32 || !inst.ArgAt(1).IsValue()) return false;
    const auto value = inst.ArgAt(1).Get<ir::Value>();
    const auto* definition = value.Def();
    if (!definition || !prior.contains(definition)) return false;
    if (value.Type() != ir::ValueType::U32 || definition->ReturnType() != ir::ValueType::U32 ||
        definition->GetOp() != ir::OpCode::LoadImm || !definition->ArgAt(0).IsImm() ||
        !OnlyArguments(*definition, 1)) return false;
    const auto reason = definition->ArgAt(0).Get<ir::Imm>();
    // Static decoder fallback must reject before publishing padding or PC.
    // An audited FP helper can still report a synchronous runtime fallback.
    return reason.GetType() == ir::ValueType::U32 &&
           reason.Get() != static_cast<u32>(InterruptReason::FALLBACK);
}
}  // namespace

bool Arm64SingleInstructionAdmitted(const runtime::ir::Block& block) {
    std::unordered_set<const ir::Inst*> prior;
    for (const auto& inst : block.GetInstList()) {
        switch (inst.GetOp()) {
            case ir::OpCode::CallLambda:
                if (!AdmittedCall(inst, block.GetStartLocation().Value(), prior)) return false;
                break;
            case ir::OpCode::CallDynamic:
            case ir::OpCode::CallLocation:
            case ir::OpCode::Cpuid:
                // HostFunction signatures and user-supplied impl pointers do
                // not establish guest-memory or callback safety.
                return false;
            case ir::OpCode::StoreUniform:
                if (!AdmittedInterruptStore(inst, prior)) return false;
                break;
            default:
                break;
        }
        prior.insert(&inst);
    }
    return true;
}
}  // namespace swift::arm64

