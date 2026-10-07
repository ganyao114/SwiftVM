#include "translator.h"

#include "runtime/backend/context.h"
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

bool JitTranslator::EmitVectorHostRegisters(ir::Inst* inst) {
    const auto op = inst->GetOp();
    bool load{}, host{}, fpr{};
    switch (op) {
        case O::GetHostGPR: load = host = true; break;
        case O::GetHostFPR: load = host = fpr = true; break;
        case O::SetHostGPR: host = true; break;
        case O::SetHostFPR: host = fpr = true; break;
        case O::LoadUniform: load = true; break;
        case O::StoreUniform: break;
        default: return false;
    }
    const auto value = load ? ir::Value{inst} : inst->GetArg<ir::Value>(host ? 0 : 1);
    const auto type = load ? inst->ReturnType() : value.Type();
    const u32 size = ir::GetValueSizeByte(type);
    UniformBinding* binding;
    u32 offset;
    if (host) {
        binding = context.FindUniformBinding(fpr, inst->GetArg<ir::Imm>(load ? 0 : 1).Get());
        ASSERT(binding);
        offset = inst->GetArg<ir::Imm>(load ? 1 : 2).Get();
    } else {
        const auto absolute = inst->GetArg<ir::Uniform>(0).GetOffset();
        binding = context.FindUniformRange(absolute, size);
        if (!binding) {
            // Publish before a canonical access spanning several bindings;
            // an overlapping store reloads their register copies afterwards.
            bool overlaps{};
            for (const auto& candidate : context.UniformBindings())
                overlaps |= u64(absolute) < u64(candidate.desc.offset) + candidate.desc.size &&
                            u64(candidate.desc.offset) < u64(absolute) + size;
            if (!overlaps) return false;
            context.PublishUniformBindings();
            binding = nullptr;
            offset = absolute;
        } else offset = absolute - binding->desc.offset;
    }
    auto& as = context.GetMasm();
    const bool vector_result = ir::IsFloatValueType(type);
    const auto absolute = binding ? binding->desc.offset + offset : offset;
    const bool vector_pin = binding && binding->vector_index != UINT32_MAX;
    const bool gpr_pin = binding && binding->gpr_indices[0] != UINT32_MAX;
    const auto vector_home = vector_pin ? Vec{binding->vector_index + 8} : v7;
    const auto read_integer = [&](GPR result, u32 part, u32 bytes) {
        const u32 where = offset + part * 8;
        if (gpr_pin) {
            const auto source = scalar_registers[binding->gpr_indices[where / 8]];
            if (where % 8) as.SRLI(result, source, (where % 8) * 8);
            else if (result != source) as.MV(result, source);
            if (where % 8 + bytes > 8) {
                as.SLLI(t2, scalar_registers[binding->gpr_indices[where / 8 + 1]], (8 - where % 8) * 8);
                as.OR(result, result, t2);
            }
            context.Mask(result, bytes * 8);
        } else if (vector_pin) {
            if (where % bytes) {
                context.SetVectorType(8, 16); as.VSLIDEDOWN(v7, vector_home, where);
                context.SetVectorType(bytes * 8, 16 / bytes); as.VMV_XS(result, v7);
                context.Mask(result, bytes * 8);
                return;
            }
            context.SetVectorType(bytes * 8, 16 / bytes);
            const auto lane = where / bytes;
            if (lane) { as.VSLIDEDOWN(v7, vector_home, lane); as.VMV_XS(result, v7); }
            else as.VMV_XS(result, vector_home);
            context.Mask(result, bytes * 8);
        } else context.LoadUniformInteger(result, absolute + part * 8, bytes);
    };
    if (load) {
        if (vector_result && context.Features().vector) {
            const auto result = context.ResultVector(inst);
            if (vector_pin) {
                if (size == 16) { context.SetVectorType(64, 2); as.VMV(result, vector_home); }
                else {
                    context.SetVectorType(64, 2); as.VMV(result, 0);
                    if (offset % size) {
                        context.SetVectorType(8, size); as.VSLIDEDOWN(result, vector_home, offset);
                    } else { context.SetVectorType(size * 8, 1); as.VRGATHER(result, vector_home, offset / size); }
                }
            } else if (gpr_pin) {
                read_integer(t0, 0, std::min<u32>(size, 8));
                if (size > 8) read_integer(t1, 1, 8); else as.MV(t1, x0);
                context.SetVectorType(64, 2); as.VMV(v7, t1); as.VSLIDE1UP(result, v7, t0);
            } else {
                context.SetVectorType(64, 2); as.VMV(result, 0);
                context.SetVectorType(8, size); context.Address(t6, state, state_offset_uniform_buffer + absolute); as.VLE8(result, t6);
            }
            context.WriteVector(inst, result);
        } else if (vector_result) {
            const auto result = context.ResultPair(inst);
            read_integer(result[0], 0, std::min<u32>(size, 8));
            if (size > 8) read_integer(result[1], 1, 8); else as.MV(result[1], x0);
            context.WritePair(inst, result);
        } else {
            const auto result = context.ResultRegister(inst);
            read_integer(result, 0, size); context.Write(inst, result, true);
        }
        return true;
    }
    const bool clear_upper = host && !fpr && binding->desc.size == 8 && offset == 0 && size == 4 && IsFixedGPRHome(binding->desc.reg);
    const bool normalized_scalar = !ir::IsFloatValueType(value.Def()->ReturnType()) &&
            ir::GetValueSizeByte(value.Def()->ReturnType()) <= size &&
            value.Def()->GetOp() != O::BitCast && value.Def()->GetOp() != O::GetResult;
    if (vector_pin) {
        if (size == 16) {
            const auto source = context.SourceVector(value, v1);
            context.SetVectorType(64, 2); as.VMV(vector_home, source);
        } else if (offset % size) {
            Vec source;
            if (vector_result) source = context.SourceVector(value, v1);
            else {
                context.Read(t0, value); context.SetVectorType(64, 2); as.VMV(v7, t0); source = v7;
            }
            context.SetVectorType(8, 16); as.VMV(v6, 0); as.VSLIDEUP(v6, source, offset);
            as.VID(v4); as.VMSGEU(v0, v4, int(offset)); as.VMSLTU(v5, v4, int(offset + size));
            as.VMAND(v0, v0, v5); as.VMERGE(vector_home, vector_home, v6);
        } else {
            if (vector_result) {
                const auto source = context.SourceVector(value, v1);
                context.SetVectorType(size * 8, 16 / size); as.VRGATHER(v7, source, 0u);
            } else { context.Read(t0, value); context.SetVectorType(size * 8, 16 / size); }
            as.VID(v4); as.VMSEQ(v0, v4, offset / size);
            if (vector_result) as.VMERGE(vector_home, vector_home, v7);
            else as.VMERGE(vector_home, vector_home, t0);
        }
    } else if (gpr_pin) {
        for (u32 part = 0; part < (size > 8 ? 2u : 1u); ++part) {
            const u32 where = offset + part * 8, bytes = std::min<u32>(size, 8);
            const auto target = scalar_registers[binding->gpr_indices[where / 8]];
            if (where % 8 + bytes > 8) {
                // A canonical uniform field may straddle the two pinned
                // 64-bit halves. Merge both parts without publishing/reloading
                // the entire binding through memory.
                const auto source = context.SourcePart(value, part, t0);
                const u32 shift = (where % 8) * 8, first_bits = 64 - shift;
                context.Mask(target, shift);
                as.SLLI(t2, source, shift); as.OR(target, target, t2);
                const auto next = scalar_registers[binding->gpr_indices[where / 8 + 1]];
                const u32 next_bits = bytes * 8 - first_bits;
                const u64 mask = ~((u64{1} << next_bits) - 1);
                if (s64(mask) >= -2048) as.ANDI(next, next, s32(mask));
                else { as.SRLI(next, next, next_bits); as.SLLI(next, next, next_bits); }
                as.SRLI(t2, source, first_bits); context.Mask(t2, next_bits); as.OR(next, next, t2);
                continue;
            }
            if (bytes == 8) {
                const auto source = context.SourcePart(value, part, t0);
                if (target != source) as.MV(target, source);
                continue;
            }
            auto source = context.SourcePart(value, part, t0);
            if (clear_upper) {
                if (target != source) as.MV(target, source);
                if (!normalized_scalar) context.Mask(target, 32);
            } else {
                if (!normalized_scalar) {
                    if (source != t0) as.MV(t0, source);
                    context.Mask(t0, bytes * 8); source = t0;
                }
                const u64 mask = ((u64{1} << (bytes * 8)) - 1) << ((where % 8) * 8);
                if (s64(~mask) >= -2048 && s64(~mask) <= 2047) as.ANDI(target, target, s32(~mask));
                else { as.LI(t1, ~mask); as.AND(target, target, t1); }
                if (where % 8) { as.SLLI(t2, source, (where % 8) * 8); source = t2; }
                as.OR(target, target, source);
            }
        }
    } else if (vector_result && context.Features().vector) {
        const auto source = context.SourceVector(value, v1);
        context.SetVectorType(8, size); context.Address(t6, state, state_offset_uniform_buffer + absolute); as.VSE8(source, t6);
    } else {
        for (u32 part = 0; part < (size > 8 ? 2u : 1u); ++part) {
            context.ReadPart(t0, value, part);
            context.StoreUniformInteger(t0, absolute + part * 8, std::min<u32>(size, 8));
        }
        if (clear_upper) context.StoreUniformInteger(x0, absolute + 4, 4);
    }
    if (!binding) context.LoadUniformBindings();
    return true;
}

}  // namespace swift::runtime::backend::riscv64
