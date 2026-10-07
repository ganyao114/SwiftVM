#include "jit_context.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include "runtime/backend/context.h"
#include "runtime/ir/block.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

void JitContext::LoadUniformInteger(GPR result, u32 offset, u32 size) {
    if (offset % size == 0) { Load(result, state, state_offset_uniform_buffer + offset, size); return; }
    masm.MV(result, x0);
    for (u32 i = 0; i < size; ++i) {
        Load(t4, state, state_offset_uniform_buffer + offset + i, 1);
        if (i) masm.SLLI(t4, t4, i * 8);
        masm.OR(result, result, t4);
    }
}

void JitContext::StoreUniformInteger(GPR source, u32 offset, u32 size) {
    if (offset % size == 0) { Store(source, state, state_offset_uniform_buffer + offset, size); return; }
    for (u32 i = 0; i < size; ++i) {
        if (i) masm.SRLI(t4, source, i * 8);
        Store(i ? t4 : source, state, state_offset_uniform_buffer + offset + i, 1);
    }
}

UniformBinding* JitContext::FindUniformBinding(bool vector, u32 reg) {
    for (auto& binding : uniform_bindings)
        if (binding.desc.is_float == vector && binding.desc.reg == reg) return &binding;
    return nullptr;
}

UniformBinding* JitContext::FindUniformRange(u32 offset, u32 size) {
    for (auto& binding : uniform_bindings)
        if (offset >= binding.desc.offset && size <= binding.desc.size &&
            offset - binding.desc.offset <= binding.desc.size - size) return &binding;
    return nullptr;
}

void JitContext::ConfigureUniformBindings(ir::Block* block) {
    uniform_bindings.clear();
    std::vector<UniformBinding> active;
    for (const auto& desc : uniform_descriptors) {
        if (!desc.size || desc.size > (desc.is_float ? 16 : 8) ||
            (desc.size & (desc.size - 1))) throw std::runtime_error("invalid RV64 uniform binding width");
        for (const auto& previous : active)
            if ((previous.desc.reg == desc.reg && previous.desc.is_float == desc.is_float) ||
                (u64(desc.offset) < u64(previous.desc.offset) + previous.desc.size &&
                 u64(previous.desc.offset) < u64(desc.offset) + desc.size))
                throw std::runtime_error("overlapping RV64 uniform bindings");
        active.push_back({desc});
    }
    for (auto& inst : block->GetInstList()) {
        const auto op = inst.GetOp();
        const bool get = op == O::GetHostGPR || op == O::GetHostFPR;
        const bool set = op == O::SetHostGPR || op == O::SetHostFPR;
        if (get || set) {
            const auto reg = inst.GetArg<ir::Imm>(get ? 0 : 1).Get();
            const auto offset = inst.GetArg<ir::Imm>(get ? 1 : 2).Get();
            const auto type = get ? inst.ReturnType() : inst.GetArg<ir::Value>(0).Type();
            const u32 size = ir::GetValueSizeByte(type);
            auto found = std::find_if(active.begin(), active.end(), [&](const auto& binding) {
                return binding.desc.reg == reg && binding.desc.is_float == (op == O::GetHostFPR || op == O::SetHostFPR);
            });
            if (found == active.end() || !size || size > found->desc.size || offset > found->desc.size - size ||
                (found->desc.is_float && offset % size))
                throw std::runtime_error("RV64 host-register IR has no compatible uniform binding");
            ++found->accesses; found->written |= set;
        } else if (op == O::LoadUniform || op == O::StoreUniform) {
            const auto uniform = inst.GetArg<ir::Uniform>(0);
            const u32 size = ir::GetValueSizeByte(op == O::LoadUniform ? inst.ReturnType() : inst.GetArg<ir::Value>(1).Type());
            for (auto& binding : active) {
                const auto begin = uniform.GetOffset();
                if (begin >= binding.desc.offset && size <= binding.desc.size &&
                    begin - binding.desc.offset <= binding.desc.size - size) {
                    ++binding.accesses; binding.written |= op == O::StoreUniform;
                } else if (u64(begin) < u64(binding.desc.offset) + binding.desc.size &&
                           u64(binding.desc.offset) < u64(begin) + size) {
                    // A spanning access must stay coherent with each resident
                    // part. It is uncommon enough to use the memory boundary.
                    binding.spanning = true;
                }
            }
        }
    }
    std::stable_sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return a.accesses > b.accesses; });
    const u32 capacity = scalar_registers.size() - u32(flags_enabled);
    for (auto& binding : active) {
        if (!binding.accesses) continue;
        if (!binding.spanning && features.vector && binding.desc.is_float) {
            for (u32 index = 16; index-- > 4;) if (!(reserved_vectors & (1u << index))) {
                binding.vector_index = index; reserved_vectors |= 1u << index; break;
            }
        } else if (!binding.spanning) {
            const u32 needed = binding.desc.size > 8 ? 2 : 1;
            if (std::popcount(reserved_gprs) + needed <= capacity - 2) {
                u32 part{};
                for (u32 index = capacity; index-- > 0 && part < needed;)
                    if (!(reserved_gprs & (1u << index))) {
                        binding.gpr_indices[part++] = index; reserved_gprs |= 1u << index;
                    }
            }
        }
        uniform_bindings.push_back(binding);
    }
    used_gprs |= reserved_gprs;
}

void JitContext::LoadUniformBindings() {
    for (const auto& binding : uniform_bindings) {
        if (binding.vector_index != UINT32_MAX) {
            const auto target = Vec{binding.vector_index + 8};
            SetVectorType(64, 2); masm.VMV(target, 0);
            SetVectorType(8, binding.desc.size);
            Address(t6, state, state_offset_uniform_buffer + binding.desc.offset); masm.VLE8(target, t6);
        } else if (binding.gpr_indices[0] != UINT32_MAX) {
            for (u32 part = 0; part < (binding.desc.size > 8 ? 2u : 1u); ++part)
                LoadUniformInteger(scalar_registers[binding.gpr_indices[part]], binding.desc.offset + part * 8,
                                   std::min<u32>(8, binding.desc.size));
        }
    }
}

void JitContext::PublishUniformBindings() {
    for (const auto& binding : uniform_bindings) {
        if (!binding.written) continue;
        if (binding.vector_index != UINT32_MAX) {
            SetVectorType(8, binding.desc.size);
            Address(t6, state, state_offset_uniform_buffer + binding.desc.offset);
            masm.VSE8(Vec{binding.vector_index + 8}, t6);
        } else if (binding.gpr_indices[0] != UINT32_MAX) {
            for (u32 part = 0; part < (binding.desc.size > 8 ? 2u : 1u); ++part)
                StoreUniformInteger(scalar_registers[binding.gpr_indices[part]], binding.desc.offset + part * 8,
                                    std::min<u32>(8, binding.desc.size));
        }
    }
}

}  // namespace swift::runtime::backend::riscv64
