#include "runtime/backend/arm64/compile/register_contract.h"
#include "target.h"

#include <algorithm>

#include "aarch64/assembler-aarch64.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/common/signal_diagnostic.h"
#include "runtime/compile/allocate/writer.h"
#include "runtime/compile/analysis/uses.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {

bool SupportsIR(const ir::Block* block) {
    if (!block) return false;
    for (const auto& inst : block->GetInstList()) {
        if (!inst.HasValidResultType() || !inst.HasValidArguments()) return false;
        if (inst.GetOp() == ir::OpCode::AddPhi || inst.GetOp() == ir::OpCode::DefineLocal ||
            inst.GetOp() == ir::OpCode::LoadLocal || inst.GetOp() == ir::OpCode::StoreLocal) {
            SVM_DIAG_FORMAT(Codegen,
                            "[unsupported-arm64-ir] op={} pc={:#x}\n",
                            inst.GetOp(),
                            block->GetStartLocation().Value());
            return false;
        }
    }
    return compile::CheckImplicitUses(*block, 0, [](ir::Value, u32) {});
}

bool SupportsIR(const ir::HIRFunction* function) {
    if (!function || function->HasStandaloneBlocks()) return false;
    for (const auto* block : function->GetHIRBlocks())
        if (block && !SupportsIR(block->GetBlock())) return false;
    return true;
}

bool IsAddSubImmediate(u64 value) {
    return value <= 0xfff || ((value & 0xfff) == 0 && (value >> 12) <= 0xfff);
}

bool IsLogicalImmediate(u64 value, u32 width) {
    if (width != 32 && width != 64) return false;
    if (width == 32) value &= UINT32_MAX;
    return vixl::aarch64::Assembler::IsImmLogical(value, width);
}

compile::BuilderCapabilities BuilderTarget() {
    return {IsAddSubImmediate, IsLogicalImmediate, true};
}

void ConfigureGuestStatePolicy(compile::GuestStateLayout& layout) {
    // These fixed homes use the ARM64 capture/width contracts. A logical
    // forwarding rule must not infer that contract from another target's IDs.
    layout.optimization.forwarded_gprs.Reset(
            0x3ffu | (1u << 22) | (1u << 23) | (1u << 29));
    // Preserve the extended-map ordering, including its XMM cleanup sweep.
    layout.optimization.dse_before_mapping = layout.uni_gprs.Get(22) &&
            layout.uni_gprs.Get(23) && layout.uni_gprs.Get(29);
}

namespace {
compile::HoistRegisterSite DescribeHoistSite(u32 registers, const ir::Inst& inst,
        const compile::FlagsDemand& demand, const FeatureSet& features) {
    const GPRSMask reserved{~registers};
    const bool scratch_only = X86PinExtScratchOnlyEnabled(reserved, features);
    u32 clobbers = FixedGPRClobbers(inst, features, scratch_only);
    if (const auto helper = HelperCallContract::Resolve(inst, features)) {
        for (u32 reg = 0; reg < 31; ++reg)
            if (helper->ClobbersGPR(reg)) clobbers |= 1u << reg;
    }
    if (inst.GetOp() == ir::OpCode::SetHostGPR)
        clobbers |= 1u << inst.GetArg<ir::Imm>(1).Get();
    const u32 scratch = ScratchBudget(inst, features, demand).gpr;
    const u32 extra = ScratchOnlyGPRCount(reserved, inst.GetOp(), features);
    const u32 reserve = std::max(DefaultGPRValueReserve(reserved, features),
                                scratch > extra ? scratch - extra : 0u);
    const bool fixed_read = inst.GetOp() == ir::OpCode::GetHostGPR &&
            inst.GetArg<ir::Imm>(1).Get() == 0 && ir::GetValueSizeByte(inst.ReturnType()) == sizeof(u64);
    return {clobbers, fixed_read ? 1u << inst.GetArg<ir::Imm>(0).Get() : 0u, reserve};
}
}  // namespace

compile::LoopHoistTarget LoopHoistPolicy(const GPRSMask& reserved) {
    u32 available{};
    for (u32 reg = 0; reg < 31; ++reg) if (!reserved.Get(reg)) available |= 1u << reg;
    return {available, DescribeHoistSite};
}

}  // namespace swift::runtime::backend::arm64
