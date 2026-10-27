#include "passes.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::compile {
using namespace ir;

static void FinalizeFPRMappings(RewriteContext& rewrite, Block* block, const GuestStateLayout& info) {
    if (!GetSvmConfig().xmm_resident) {
        return;
    }
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == OpCode::LoadUniform) {
            const auto uniform = inst.GetArg<Uniform>(0);
            const u32 offset = uniform.GetOffset();
            const u32 size = GetValueSizeByte(uniform.GetType());
            const auto mapped = info.uniform_regs_map.GetValueAt(offset);
            if (mapped.Null() || !mapped.host_reg.is_fpr) {
                continue;
            }
            const u32 mapped_offset = mapped.uniform.GetOffset();
            const u32 mapped_size = GetValueSizeByte(mapped.uniform.GetType());
            const bool scalar_from_fpr = !IsFloatValueType(uniform.GetType()) &&
                    size <= sizeof(u64);
            ASSERT_MSG((IsFloatValueType(uniform.GetType()) || scalar_from_fpr) &&
                               offset >= mapped_offset &&
                               offset + size <= mapped_offset + mapped_size &&
                               info.uniform_regs_map.GetContinuousSizeFrom(offset) >= size,
                       "invalid resident FPR load [{}, {})", offset, offset + size);
            const u32 reg = mapped.host_reg.fpr.id;
            rewrite.Change(block, &inst, [&](Inst& inst) {
                inst.Reset();
                inst.GetHostFPR(HostRegIndex(reg), Imm{offset - mapped_offset})
                        .SetReturn(uniform.GetType());
            });
        } else if (inst.GetOp() == OpCode::StoreUniform) {
            const auto uniform = inst.GetArg<Uniform>(0);
            const auto value = inst.GetArg<Value>(1);
            const u32 offset = uniform.GetOffset();
            const u32 size = GetValueSizeByte(value.Type());
            const auto mapped = info.uniform_regs_map.GetValueAt(offset);
            if (mapped.Null() || !mapped.host_reg.is_fpr) {
                continue;
            }
            const u32 mapped_offset = mapped.uniform.GetOffset();
            const u32 mapped_size = GetValueSizeByte(mapped.uniform.GetType());
            const bool scalar_to_fpr = !IsFloatValueType(value.Type()) &&
                    size <= sizeof(u64);
            ASSERT_MSG((IsFloatValueType(value.Type()) || scalar_to_fpr) &&
                               offset >= mapped_offset &&
                               offset + size <= mapped_offset + mapped_size &&
                               info.uniform_regs_map.GetContinuousSizeFrom(offset) >= size,
                       "invalid resident FPR store [{}, {})", offset, offset + size);
            const u32 reg = mapped.host_reg.fpr.id;
            rewrite.Change(block, &inst, [&](Inst& inst) {
                inst.Reset();
                inst.SetHostFPR(value, HostRegIndex(reg), Imm{offset - mapped_offset});
            });
        }
    }
}

void FinalizeStateMappings(RewriteContext& rewrite, const GuestStateLayout& info) {
    for (auto* block : rewrite.Blocks()) FinalizeFPRMappings(rewrite, block, info);
}

}  // namespace swift::runtime::compile
