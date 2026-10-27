#include "ir_builder.h"
#include "runtime/compile/analysis/guest_state.h"

namespace swift::runtime::compile {
using namespace ir;

void IrBuilder::ConfigureGuestState(const swift::runtime::compile::GuestStateLayout* info, FeatureSet features,
                                    BuilderCapabilities capabilities) {
    uniform_features = features;
    target = capabilities;
    const bool forward = GetSvmConfig().builder_uniform;
    if (!info || !info->uniform_size || !forward) {
        return;
    }
    guest_layout = info;
    uniform_bytes.assign(info->uniform_size, GuestStateByteFact{});
    uniform_builder = true;
    uniform_forward = forward;
    uniform_bound = target.fixed_guest_state && forward;
}

void IrBuilder::UniformInvalidateAll() {
    GuestStateForwarding::Invalidate(uniform_bytes);
}

GuestStateRead IrBuilder::UniformReadFor(Uniform uniform,
                                         const GuestStateForwardAccess& access) const {
    if (!access.read) return {};
    const auto policy = uniform_features.xmm_narrow_fwd
            ? GuestStateReadPolicy::ForwardNarrowFPR : GuestStateReadPolicy::Forward;
    return ReadGuestStateBytes(uniform_bytes, uniform, policy);
}

void IrBuilder::UniformOnLoad(Inst* inst) {
    const auto uniform = inst->GetArg<Uniform>(0);
    const auto access = DescribeGuestStateForwardAccess(
            *guest_layout, uniform_features, uniform.GetOffset(), uniform.GetType(),
            GetSvmConfig().xmm_resident);
    if (access.seed_load && !UniformReadFor(uniform, access).Found()) {
        GuestStateForwarding::Write(uniform_bytes, uniform.GetOffset(), Value{inst});
    }
}

void IrBuilder::UniformOnStore(Inst* inst) {
    const auto uniform = inst->GetArg<Uniform>(0);
    const auto value = inst->GetArg<Value>(1);
    const auto access = DescribeGuestStateForwardAccess(
            *guest_layout, uniform_features, uniform.GetOffset(), value.Type(),
            GetSvmConfig().xmm_resident);
    UniformStoreFacts(uniform.GetOffset(), value, access.write);
}

void IrBuilder::UniformStoreFacts(u32 offset, Value value, GuestStateForwardWrite write) {
    if (write == GuestStateForwardWrite::Write) {
        GuestStateForwarding::Write(uniform_bytes, offset, value);
    } else if (write == GuestStateForwardWrite::Invalidate) {
        UniformInvalidateAll();
    }
}

void IrBuilder::UniformAfterAppend(Inst* inst) {
    if (inst->GetOp() == OpCode::LoadUniform) {
        UniformOnLoad(inst);
    } else if (inst->GetOp() == OpCode::StoreUniform) {
        UniformOnStore(inst);
    } else {
        (void)uniform_forwarding.Observe(uniform_bytes, *inst,
                uniform_features.ir_uniform_range, uniform_features.uniform_path_fwd);
    }
}

Value IrBuilder::LoadUniform(Uniform uniform) {
    if (uniform_forward) {
        const auto uni_offset = uniform.GetOffset();
        const auto uni_type = uniform.GetType();
        const auto uni_size = GetValueSizeByte(uni_type);
        const auto append_fold = [&](const GuestStateRead& fold) {
            // Forward an exact FPR value without adding a data-only alias.
            // Integer producer identities may still acquire flag pseudos.
            if (fold.kind == GuestStateReadKind::Alias && IsFloatValueType(uni_type))
                if (auto value = FoldSameTypeCast(uni_type, fold.value, FlagsIntent::None()))
                    return *value;
            Inst* inst = fold.kind == GuestStateReadKind::Extract
                    ? AppendRawTyped<OpCode::BitExtract>(
                              uni_type, fold.value, Imm(fold.offset * 8u), Imm(uni_size * 8u))
                    : AppendRawTyped<OpCode::BitCast>(uni_type, fold.value);
            return Value{inst};
        };
        const auto forward = DescribeGuestStateForwardAccess(
                *guest_layout, uniform_features, uni_offset, uni_type, GetSvmConfig().xmm_resident);
        const auto& access = forward.access;
        const auto& binding = access.binding;
        if (access.kind == GuestStateAccessKind::Register) {
            const bool mapped_fpr = binding.host_reg.is_fpr;
            const auto reg_index = mapped_fpr ? binding.host_reg.fpr.id : binding.host_reg.gpr.id;
            if (const auto fold = UniformReadFor(uniform, forward); fold.Found()) {
                return append_fold(fold);
            }
            const Imm offset_in{static_cast<u8>(uni_offset - binding.uniform.GetOffset())};
            Inst* inst = mapped_fpr
                    ? AppendRawTyped<OpCode::GetHostFPR>(uni_type, HostRegIndex(reg_index), offset_in)
                    : AppendRawTyped<OpCode::GetHostGPR>(uni_type, HostRegIndex(reg_index), offset_in);
            if (inst && forward.seed_load) {
                GuestStateForwarding::Write(uniform_bytes, uni_offset, Value{inst});
            }
            return Value{inst};
        }
        if (access.kind == GuestStateAccessKind::Memory) {
            if (const auto fold = UniformReadFor(uniform, forward); fold.Found()) {
                return append_fold(fold);
            }
            auto* inst = AppendRawTyped<OpCode::LoadUniform>(uni_type, uniform);
            if (inst && forward.seed_load) {
                GuestStateForwarding::Write(uniform_bytes, uni_offset, Value{inst});
            }
            return Value{inst};
        }
    }
    // Invalid mappings remain raw so the residual pass retains its diagnostic.
    Inst* inst = AppendRawTyped<OpCode::LoadUniform>(uniform.GetType(), uniform);
    return Value{inst};
}

Void IrBuilder::StoreUniform(Uniform uniform, Value value) {
    if (uniform_bound) {
        const auto forward = DescribeGuestStateForwardAccess(
                *guest_layout, uniform_features, uniform.GetOffset(), value.Type(),
                GetSvmConfig().xmm_resident);
        const auto& access = forward.access;
        const auto& binding = access.binding;
        // Partial mapped stores still need the pass's complete byte context.
        if (access.kind == GuestStateAccessKind::Register &&
            GetValueSizeByte(value.Type()) == GetValueSizeByte(binding.uniform.GetType())) {
            const bool mapped_fpr = binding.host_reg.is_fpr;
            const auto reg_index = mapped_fpr ? binding.host_reg.fpr.id : binding.host_reg.gpr.id;
            const Imm offset_in{static_cast<u8>(
                    uniform.GetOffset() - binding.uniform.GetOffset())};
            Inst* inst = mapped_fpr
                    ? AppendRawTyped<OpCode::SetHostFPR>(
                              ValueType::VOID, value, HostRegIndex(reg_index), offset_in)
                    : AppendRawTyped<OpCode::SetHostGPR>(
                              ValueType::VOID, value, HostRegIndex(reg_index), offset_in);
            if (inst) UniformStoreFacts(uniform.GetOffset(), value, forward.write);
            return Void{inst};
        }
    }
    Inst* inst = AppendRawTyped<OpCode::StoreUniform>(ValueType::VOID, uniform, value);
    if (uniform_builder && inst) UniformOnStore(inst);
    return Void{inst};
}


}  // namespace swift::runtime::compile
