#include "jit_context.h"

#include <bit>
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

void JitContext::ConfigurePhiRegisters(std::span<ir::Inst* const> phis) {
    const u32 capacity = scalar_registers.size() - u32(flags_enabled);
    for (auto* inst : phis) {
        PhiBinding binding{inst};
        const u32 size = ir::GetValueSizeByte(inst->ReturnType());
        if (features.vector && ir::IsFloatValueType(inst->ReturnType())) {
            for (u32 index = 16; index-- > 4;) if (!(reserved_vectors & (1u << index))) {
                binding.vector_index = index; reserved_vectors |= 1u << index; break;
            }
        } else {
            const u32 needed = size > 8 ? 2 : 1;
            if (std::popcount(reserved_gprs) + needed <= capacity - 2) {
                u32 part{};
                for (u32 index = capacity; index-- > 0 && part < needed;)
                    if (!(reserved_gprs & (1u << index))) {
                        binding.gpr_indices[part++] = index; reserved_gprs |= 1u << index;
                    }
            }
        }
        phi_bindings.push_back(binding);
    }
    used_gprs |= reserved_gprs;
}

void JitContext::AssignPhi(ir::Inst* inst, GPR low, GPR high) {
    for (const auto& binding : phi_bindings) if (binding.inst == inst) {
        if (binding.vector_index != UINT32_MAX) {
            SetVectorType(64, 2); masm.VMV(v7, high);
            masm.VSLIDE1UP(Vec{binding.vector_index + 8}, v7, low);
            return;
        }
        if (binding.gpr_indices[0] != UINT32_MAX) {
            masm.MV(scalar_registers[binding.gpr_indices[0]], low);
            if (binding.gpr_indices[1] != UINT32_MAX) masm.MV(scalar_registers[binding.gpr_indices[1]], high);
            return;
        }
        Store(low, values, s64(inst->Id()) * kValueStride); ++value_stats.stores;
        if (!ZeroHigh(inst)) { Store(high, values, s64(inst->Id()) * kValueStride + 8); ++value_stats.stores; }
        return;
    }
    PANIC("RV64 phi has no edge-assignment binding");
}

bool JitContext::AssignPhiValue(ir::Inst* inst, ir::Value value) {
    for (const auto& binding : phi_bindings) if (binding.inst == inst) {
        const u32 size = ir::GetValueSizeByte(inst->ReturnType());
        if (binding.vector_index != UINT32_MAX) {
            const auto source = SourceVector(value, v1), target = Vec{binding.vector_index + 8};
            if (size == 16) { SetVectorType(64, 2); masm.VMV(target, source); }
            else {
                SetVectorType(64, 2); masm.VMV(target, 0);
                SetVectorType(size * 8, 1); masm.VRGATHER(target, source, 0u);
            }
            return true;
        }
        if (binding.gpr_indices[0] != UINT32_MAX && size <= 8) {
            const auto source = SourcePart(value, 0, t0), target = scalar_registers[binding.gpr_indices[0]];
            if (source != target) masm.MV(target, source);
            Mask(target, size * 8);
            return true;
        }
        if (binding.gpr_indices[0] == UINT32_MAX && features.vector && ir::IsFloatValueType(inst->ReturnType())) {
            auto source = SourceVector(value, v1);
            if (size < 16) {
                SetVectorType(64, 2); masm.VMV(v7, 0);
                SetVectorType(size * 8, 1); masm.VRGATHER(v7, source, 0u); source = v7;
            }
            SetVectorType(64, 2); Address(t6, values, s64(inst->Id()) * kValueStride);
            masm.VSE64(source, t6); ++value_stats.stores;
            return true;
        }
        return false;
    }
    return false;
}

void JitContext::PublishPhiHomes() {
    for (const auto& binding : phi_bindings) {
        if (binding.vector_index != UINT32_MAX) {
            SetVectorType(64, 2); Address(t6, values, s64(binding.inst->Id()) * kValueStride);
            masm.VSE64(Vec{binding.vector_index + 8}, t6); ++value_stats.stores;
        } else if (binding.gpr_indices[0] != UINT32_MAX) {
            Store(scalar_registers[binding.gpr_indices[0]], values, s64(binding.inst->Id()) * kValueStride); ++value_stats.stores;
            if (!ZeroHigh(binding.inst)) {
                Store(binding.gpr_indices[1] == UINT32_MAX ? x0 : scalar_registers[binding.gpr_indices[1]],
                      values, s64(binding.inst->Id()) * kValueStride + 8); ++value_stats.stores;
            }
        }
    }
}

}  // namespace swift::runtime::backend::riscv64
