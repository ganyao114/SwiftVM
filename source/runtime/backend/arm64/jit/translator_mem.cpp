#include "base/logging.h"
#include "runtime/backend/arm64/compile/constraints.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/gpr_coalescing_contract.h"
#include "translator.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {





#define __ masm.

namespace {

bool StructuredAddressModeEnabled(const FeatureSet& features) {
    return features.addrmode_struct;
}

void HostMemMove(void* dst, const void* src, size_t size) {
    std::memmove(dst, src, size);
}

struct TsoEmissionStats {
    const bool enabled{GetSvmConfig().tso_stats};
    std::atomic<u64> load_sites{};
    std::atomic<u64> store_sites{};
    std::atomic<u64> scalar_fast_sites{};
    std::atomic<u64> alignment_check_sites{};
    std::atomic<u64> dmb_instructions{};

    ~TsoEmissionStats() {
        if (enabled) {
            SVM_DIAG_PRINT(Codegen,
                         "SVM_TSO_STATS load_sites=%llu store_sites=%llu "
                         "scalar_fast_sites=%llu alignment_check_sites=%llu "
                         "dmb_instructions=%llu\n",
                         static_cast<unsigned long long>(load_sites.load()),
                         static_cast<unsigned long long>(store_sites.load()),
                         static_cast<unsigned long long>(scalar_fast_sites.load()),
                         static_cast<unsigned long long>(alignment_check_sites.load()),
                         static_cast<unsigned long long>(dmb_instructions.load()));
        }
    }

    void Increment(std::atomic<u64>& counter) {
        if (enabled) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

TsoEmissionStats tso_emission_stats;

}  // namespace

bool HostBaseFoldEligible(bool enabled,
                          bool use_memory_base,
                          u64 guest_addr_mask,
                          ir::ValueType type,
                          bool structured_guest_ea,
                          bool guest_add_form,
                          bool tso_or_atomic) {
    // The W39 structured operand has already computed the guest EA with W
    // arithmetic.  Only the bounded 4GB layout may use UXTW here: unbounded
    // and non-32-bit windows need their existing 64-bit bias/mask sequence.
    return enabled && use_memory_base && guest_addr_mask == UINT32_MAX &&
           type == ir::ValueType::V128 && structured_guest_ea && guest_add_form &&
           !tso_or_atomic;
}

void JitTranslator::LoadUnalignedAtomicLockAddress(const Register& lock) {
    __ Ldr(lock, MemOperand(state, state_offset_unaligned_atomic_lock));
}

void JitTranslator::AcquireUnalignedAtomicLock(const Register& lock,
                                               const Register& scratch) {
    // x12 is last_result when FLAGS_REGS is on. The lock sequence clobbers it.
    if (FlagsRegsEnabled()) {
        EmitSplitFlagsPublish();
    }
    Label retry;
    __ Bind(&retry);
    __ Ldaxr(scratch.W(), MemOperand(lock));
    __ Cbnz(scratch.W(), &retry);
    __ Mov(scratch.W(), 1);
    __ Stxr(ipw, scratch.W(), MemOperand(lock));
    __ Cbnz(ipw, &retry);
}

void JitTranslator::ReleaseUnalignedAtomicLock(const Register& lock) {
    __ Stlr(wzr, MemOperand(lock));
}

void JitTranslator::EmitBasicAtomicLoad(ir::ValueType type,
                                        const Register& result,
                                        const Register& address) {
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldrb(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldrh(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(result, MemOperand(address));
            break;
        default:
            PANIC("unsupported atomic load width");
    }
}

void JitTranslator::EmitBasicAtomicStore(ir::ValueType type,
                                         const Register& value,
                                         const Register& address) {
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Strb(value.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Strh(value.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Str(value.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Str(value, MemOperand(address));
            break;
        default:
            PANIC("unsupported atomic store width");
    }
}

void JitTranslator::EmitAtomicRMWValue(ir::AtomicRMWOp op,
                                       ir::ValueType type,
                                       const Register& output,
                                       const Register& old,
                                       ir::Value operand,
                                       ir::Value carry) {
    const bool wide = ir::GetValueSizeByte(type) == 8;
    const auto dst = wide ? output : output.W();
    const auto lhs = wide ? old : old.W();
    const Register rhs = context.R(operand);
    switch (op) {
        case ir::AtomicRMWOp::Add:
            __ Add(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Sub:
            __ Sub(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::And:
            __ And(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Or:
            __ Orr(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Xor:
            __ Eor(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Neg:
            __ Neg(dst, lhs);
            break;
        case ir::AtomicRMWOp::AddCarry:
            __ Add(dst, lhs, rhs);
            __ Add(dst, dst, context.W(carry));
            break;
        case ir::AtomicRMWOp::SubBorrow:
            __ Sub(dst, lhs, rhs);
            __ Sub(dst, dst, context.W(carry));
            break;
    }
}

void JitTranslator::EmitGuestToHost(const Register& dst, const Register& guest_addr) {
    if (memory_state.window_uxtw) {
        // pt + zext32(guest): one instruction, same as the unbounded Add.
        __ Add(dst, pt, Operand(guest_addr.W(), UXTW));
        return;
    }
    if (memory_state.guest_addr_mask) {
        __ And(dst, guest_addr, memory_state.guest_addr_mask);
        __ Add(dst, dst, pt);
        return;
    }
    __ Add(dst, guest_addr, pt);
}

MemOperand JitTranslator::BiasMem(const Register& base, bool atomic) {
    if (memory_state.window_uxtw) {
        // Bounded 32-bit guest window: [pt, Wbase, UXTW] is the *same*
        // register-offset load the unbounded path emitted, with the
        // truncation folded into the addressing mode — zero extra cost.
        if (!atomic) {
            return MemOperand{pt, base.W(), UXTW};
        }
        __ Add(mem_scratch, pt, Operand(base.W(), UXTW));
        return MemOperand{mem_scratch};
    }
    if (memory_state.guest_addr_mask) {
        // Non-32-bit window: one extra `and` with a logical immediate.
        __ And(mem_scratch, base, memory_state.guest_addr_mask);
        if (!atomic) {
            return MemOperand{mem_scratch, pt};
        }
        __ Add(mem_scratch, mem_scratch, pt);
        return MemOperand{mem_scratch};
    }
    if (!atomic) {
        return MemOperand{base, pt};
    }
    // No register-offset form available: fold the bias into the reserved
    // scratch (mem_scratch is never allocated to a guest value, unlike a
    // GetTmpX register at a VOID instruction — see defines.h).
    __ Add(mem_scratch, base, pt);
    return MemOperand{mem_scratch};
}

MemOperand JitTranslator::BiasMem(const Register& base, s64 imm, bool atomic) {
    if (imm == 0) {
        return BiasMem(base, atomic);
    }
    if (memory_state.window_uxtw) {
        // 32-bit add wraps mod 2^32, so the displacement is applied *inside*
        // the window and the truncation is again free.
        if (imm > 0) {
            __ Add(mem_scratch.W(), base.W(), imm);
        } else {
            __ Sub(mem_scratch.W(), base.W(), -imm);
        }
        if (atomic) {
            __ Add(mem_scratch, pt, Operand(mem_scratch.W(), UXTW));
            return MemOperand{mem_scratch};
        }
        return MemOperand{pt, mem_scratch.W(), UXTW};
    }
    // [guest base + imm + pt]: fold the immediate into the reserved scratch.
    if (imm > 0) {
        __ Add(mem_scratch, base, imm);
    } else {
        __ Sub(mem_scratch, base, -imm);
    }
    if (memory_state.guest_addr_mask) {
        __ And(mem_scratch, mem_scratch, memory_state.guest_addr_mask);
    }
    if (atomic) {
        __ Add(mem_scratch, mem_scratch, pt);
        return MemOperand{mem_scratch};
    }
    return MemOperand{mem_scratch, pt};
}

void JitTranslator::EmitGetHostGPR(const ir::Inst* inst) {
    if (context.GetAllocation().IsFixedGPRDefinitionElided(inst->Id())) return;
    if (context.IsHostReadCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetActions().HostReadVerified(inst->Id()),
                   "GetHostGPR coalescing proof diverged at IR {}",
                   inst->Id());
        return;
    }
    if (HasGPRLowView(inst)) {
        return;
    }
    if (const auto* action = ActiveGPRResidence(inst);
        action && action->Has(GPRResidenceAction::Resident)) {
        return;
    }
    auto offset = inst->GetArg<ir::Imm>(1).Get();
    auto reg_index = inst->GetArg<ir::Imm>(0).Get();
    const u32 value_size = ir::GetValueSizeByte(inst->ReturnType());
    auto host_reg = XRegister(reg_index);
    auto ret_reg = context.X(ir::Value{inst});
    const auto bit_offset = offset * 8;
    const auto bit_width = value_size * 8;
    if (bit_offset == 0 && bit_width == 64) {
        if (host_reg != ret_reg) {
            __ Mov(ret_reg, host_reg);
        }
    } else if (bit_offset == 0 && bit_width == 32 && reg_index <= 9) {
        // A caller-saved-pin U32 GetHostGPR may be allocated directly to its static
        // W register. Reading W already supplies x86's required zero extension.
        if (host_reg.W() != ret_reg.W()) {
            __ Mov(ret_reg.W(), host_reg.W());
        }
    } else {
        __ Ubfx(ret_reg, host_reg, bit_offset, bit_width);
    }
}

void JitTranslator::EmitGetHostFPR(const ir::Inst* inst) {
    if (context.IsHostReadCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetActions().FPRVerified(inst->Id(), FPRAction::Read),
                   "GetHostFPR coalescing proof diverged at IR {}",
                   inst->Id());
        return;
    }
    const u32 reg_index = inst->GetArg<ir::Imm>(0).Get();
    auto host_reg = VRegister::GetQRegFromCode(reg_index);
    const u32 offset = inst->GetArg<ir::Imm>(1).Get();
    const auto value_type = inst->ReturnType();
    const u32 size = ir::GetValueSizeByte(value_type);
    ASSERT_MSG(offset + size <= sizeof(u128) && offset % size == 0,
               "invalid fixed FPR read offset {} size {}", offset, size);

    if (!ir::IsFloatValueType(value_type)) {
        auto result = context.R(ir::Value{inst});
        const u32 lane = offset / size;
        switch (size) {
            case 1: __ Umov(result.W(), host_reg.V16B(), lane); break;
            case 2: __ Umov(result.W(), host_reg.V8H(), lane); break;
            case 4: __ Umov(result.W(), host_reg.V4S(), lane); break;
            case 8: __ Umov(result.X(), host_reg.V2D(), lane); break;
            default: PANIC("unsupported scalar fixed FPR read size {}", size);
        }
        return;
    }

    auto result = context.V(ir::Value{inst});
    if (size == sizeof(u128)) {
        ASSERT(offset == 0);
        if (host_reg != result) {
            __ Orr(result.V16B(), host_reg.V16B(), host_reg.V16B());
        }
        return;
    }
    const u32 lane = offset / size;
    switch (size) {
        case 1: __ Ins(result.V16B(), 0, host_reg.V16B(), lane); break;
        case 2: __ Ins(result.V8H(), 0, host_reg.V8H(), lane); break;
        case 4: __ Ins(result.V4S(), 0, host_reg.V4S(), lane); break;
        case 8: __ Ins(result.V2D(), 0, host_reg.V2D(), lane); break;
        default: PANIC("unsupported vector fixed FPR read size {}", size);
    }
}

void JitTranslator::EmitSetHostGPR(const ir::Inst* inst) {
    const auto* prepared = context.GetActions().Publication(inst->Id());
    ASSERT_MSG(prepared, "unplanned GPR publication");
    const auto& action = *prepared;
    const auto target = XRegister(action.target);
    switch (action.kind) {
        case PublicationAction::Skip:
            return;
        case PublicationAction::Transfer:
            __ Mov(target, XRegister(*action.SourceHome()));
            return;
        case PublicationAction::Copy: {
            const auto source = WRegister(*action.SourceHome());
            if (action.Width() == 8)
                __ Uxtb(target.W(), source);
            else if (action.Width() == 16)
                __ Uxth(target.W(), source);
            else __ Mov(target.W(), source);
            return;
        }
        case PublicationAction::BiasedUpdate:
            __ Sub(target, target, static_cast<u64>(action.parameter));
            return;
        case PublicationAction::Value:
            break;
    }
    const auto source =
            action.SourceHome() ? XRegister(*action.SourceHome()) : context.X(action.Source(*inst));
    if (action.BitOffset() == 0 && action.Width() == 32 &&
        action.Has(PublicationAction::ClearHigh)) {
        if (source != target || action.Has(PublicationAction::AdjustResident))
            __ Mov(target.W(), source.W());
    } else if (action.BitOffset() == 0 && action.Width() == 64) {
        if (source != target || action.Has(PublicationAction::ClearHigh)) {
            if (action.Has(PublicationAction::ClearHigh))
                __ Mov(target.W(), source.W());
            else __ Mov(target, source);
        }
    } else {
        __ Bfi(target, source, action.BitOffset(), action.Width());
    }
}

void JitTranslator::EmitSetHostFPR(const ir::Inst* inst) {
    if (const auto* fusion = context.GetActions().ScalarFPRValue(inst->Id())) {
        const auto value = inst->GetArg<ir::Value>(0);
        auto target = VRegister::GetQRegFromCode(fusion->target);
        if (CanUseZeroStoreRegister(value)) {
            __ Eor(target.V16B(), target.V16B(), target.V16B());
        } else {
            __ Fmov(target.D(), context.X(value));
        }
        return;
    }
    if (context.IsHostWriteCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetActions().FPRVerified(inst->Id(), FPRAction::Write),
                   "SetHostFPR coalescing proof diverged at IR {}",
                   inst->Id());
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    const u32 reg_index = inst->GetArg<ir::Imm>(1).Get();
    const u32 offset = inst->GetArg<ir::Imm>(2).Get();
    const u32 size = ir::GetValueSizeByte(value.Type());
    auto host_reg = VRegister::GetQRegFromCode(reg_index);
    ASSERT_MSG(offset + size <= sizeof(u128) && offset % size == 0,
               "invalid fixed FPR write offset {} size {}", offset, size);

    if (!ir::IsFloatValueType(value.Type())) {
        const bool zero_gpr = CanUseZeroStoreRegister(value);
        Register source = zero_gpr
                ? (size == sizeof(u64) ? Register{xzr} : Register{wzr})
                : context.R(value);
        const u32 lane = offset / size;
        switch (size) {
            case 1: __ Ins(host_reg.V16B(), lane, source.W()); break;
            case 2: __ Ins(host_reg.V8H(), lane, source.W()); break;
            case 4: __ Ins(host_reg.V4S(), lane, source.W()); break;
            case 8: __ Ins(host_reg.V2D(), lane, source.X()); break;
            default: PANIC("unsupported scalar fixed FPR write size {}", size);
        }
        return;
    }

    auto source = context.V(value);
    if (size == sizeof(u128)) {
        ASSERT(offset == 0);
        if (source != host_reg) {
            __ Orr(host_reg.V16B(), source.V16B(), source.V16B());
        }
        return;
    }
    const u32 lane = offset / size;
    switch (size) {
        case 1: __ Ins(host_reg.V16B(), lane, source.V16B(), 0); break;
        case 2: __ Ins(host_reg.V8H(), lane, source.V8H(), 0); break;
        case 4: __ Ins(host_reg.V4S(), lane, source.V4S(), 0); break;
        case 8: __ Ins(host_reg.V2D(), lane, source.V2D(), 0); break;
        default: PANIC("unsupported vector fixed FPR write size {}", size);
    }
}

void JitTranslator::EmitLoadUniform(const ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    s32 offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto reg = context.Get(inst);
    auto value_type = inst->ReturnType() == ir::ValueType::VOID ? uni.GetType() : inst->ReturnType();
    VisitVariant<void>(reg, [this, value_type, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Ldrb(x, MemOperand(state, offset));
                    break;
                case 2:
                    __ Ldrh(x, MemOperand(state, offset));
                    break;
                case 4:
                    __ Ldr(x.W(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Ldr(x, MemOperand(state, offset));
                    break;
            }
        } else if constexpr (std::is_same_v<T, VRegister>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Ldr(x.B(), MemOperand(state, offset));
                    break;
                case 2:
                    __ Ldr(x.H(), MemOperand(state, offset));
                    break;
                case 4:
                    __ Ldr(x.S(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Ldr(x.D(), MemOperand(state, offset));
                    break;
                case 16:
                    __ Ldr(x.Q(), MemOperand(state, offset));
                    break;
            }
        } else {
            PANIC();
        }
    });
}

void JitTranslator::EmitGetUniformAddress(const ir::Inst* inst) {
    const auto offset = inst->GetArg<ir::Imm>(0).Get();
    auto result = context.X(ir::Value{inst});
    __ Add(result, state, offsetof(State, uniform_buffer_begin) + offset);
}

void JitTranslator::EmitStoreUniform(const ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    s32 offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    const auto value = inst->GetArg<ir::Value>(1);
    ObserveEdgeCarryPolarity(uni, value);
    auto value_type = value.Type();
    const bool zero_gpr = CanUseZeroStoreRegister(value);
    CPUReg reg = zero_gpr
            ? CPUReg{ir::GetValueSizeByte(value_type) == sizeof(u64)
                             ? Register{xzr}
                             : Register{wzr}}
            : context.Get(value);
    VisitVariant<void>(reg, [this, value_type, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Strb(x, MemOperand(state, offset));
                    break;
                case 2:
                    __ Strh(x, MemOperand(state, offset));
                    break;
                case 4:
                    __ Str(x.W(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Str(x, MemOperand(state, offset));
                    break;
            }
        } else if constexpr (std::is_same_v<T, VRegister>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Str(x.B(), MemOperand(state, offset));
                    break;
                case 2:
                    __ Str(x.H(), MemOperand(state, offset));
                    break;
                case 4:
                    __ Str(x.S(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Str(x.D(), MemOperand(state, offset));
                    break;
                case 16:
                    __ Str(x.Q(), MemOperand(state, offset));
                    break;
            }
        } else {
            PANIC();
        }
    });
    if (sse_afp_nan &&
        uni.GetOffset() == offsetof(swift::x86::ThreadContext64, mxcsr)) {
        // StoreUniform has no save frame around it. Lease all three operands
        // from the allocator-visible scratch pool so XPOOL cannot place a
        // live guest value in a fixed ip register that this sync clobbers.
        const auto fpcr = context.GetTmpX();
        const auto mxcsr = context.GetTmpX();
        const auto bit = context.GetTmpX();
        EmitSseAFPRestoreGuestFPCRCached(
                masm,
                state,
                0,
                fpcr,
                mxcsr,
                bit);
    }
}

void JitTranslator::EmitLoadLocal(const ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitStoreLocal(const ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitLoadMemory(const ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    const auto pinned_value = context.PlannedGPR<XRegister>(value, cur_instr);
    auto value_w = [&] {
        return pinned_value ? pinned_value->W() : context.W(value);
    };
    auto value_x = [&] {
        return pinned_value ? *pinned_value : context.X(value);
    };
    const auto* prepared_memory = context.GetActions().Memory(inst->Id());
    const auto* narrow = prepared_memory && prepared_memory->Has(MemoryAction::Narrow)
                                 ? prepared_memory
                                 : nullptr;
    const bool direct_narrow_consumer = narrow && narrow->Has(MemoryAction::Direct);
    // Keep the established computed-address path for generic Q loads and
    // do not consume the synthetic post-index produced by the generic address
    // peephole. A1 opens only the exact bounded W39 Plus form, for which the
    // SIMD register-offset encoding can carry pt + UXTW(guest EA) directly.
    // The ARM64 frontend lowers pair writeback into normal Add + two memory
    // operations, so folding writeback here would update after the first half.
    const bool q_access = type == ir::ValueType::V128;
    const bool structured_guest_ea =
            q_access && StructuredAddressModeEnabled(context.GetFeatures()) &&
            !operand.GetRight().Null();
    const bool fold_host_base =
            HostBaseFoldEligible(memory_state.mem_hostbase_fold,
                                 memory_state.use_memory_base,
                                 memory_state.guest_addr_mask,
                                 type,
                                 structured_guest_ea,
                                 operand.GetOp() == ir::OperandOp::Plus,
                                 false);
    const auto* load_update = context.GetActions().MemoryTransfer(inst->Id());
    auto vixl_operand =
            load_update && load_update->kind == MemoryTransferAction::Load
                    ? MemOperand{XRegister(load_update->home), load_update->offset, PreIndex}
                    : EmitMemOperand(operand,
                                     type,
                                     false,
                                     q_access && !fold_host_base,
                                     !q_access,
                                     structured_guest_ea,
                                     inst);
    ASSERT(!direct_narrow_consumer || vixl_operand.GetAddrMode() == Offset ||
           vixl_operand.GetBaseRegister().GetCode() != narrow->target);
    auto narrow_value_w = [&] {
        return direct_narrow_consumer ? WRegister{narrow->target} : value_w();
    };
    auto narrow_value_x = [&] {
        return direct_narrow_consumer ? XRegister{narrow->target} : value_x();
    };
    if (const auto* scalar_fpr = context.GetActions().ScalarFPRLoad(inst->Id())) {
        const auto target = VRegister::GetQRegFromCode(scalar_fpr->target);
        if (scalar_fpr->kind == ScalarFPRAction::Load32) {
            __ Ldr(target.S(), vixl_operand);
        } else {
            __ Ldr(target.D(), vixl_operand);
        }
        return;
    }
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            if (narrow && narrow->Has(MemoryAction::Sign)) {
                if (narrow->ResultBytes() == 8) {
                    __ Ldrsb(narrow_value_x(), vixl_operand);
                } else {
                    __ Ldrsb(narrow_value_w(), vixl_operand);
                }
            } else {
                // LDRB's W destination already performs ZeroExtend32.
                __ Ldrb(narrow_value_w(), vixl_operand);
            }
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            if (narrow && narrow->Has(MemoryAction::Sign)) {
                if (narrow->ResultBytes() == 8) {
                    __ Ldrsh(narrow_value_x(), vixl_operand);
                } else {
                    __ Ldrsh(narrow_value_w(), vixl_operand);
                }
            } else {
                // LDRH's W destination already performs ZeroExtend32.
                __ Ldrh(narrow_value_w(), vixl_operand);
            }
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(value_w(), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(value_x(), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Ldr(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Ldr(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Ldr(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Ldr(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Ldr(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

void JitTranslator::EmitStoreMemory(const ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    // See EmitLoadMemory: A1 opens only the exact bounded W39 Plus form; all
    // other Q accesses keep address materialization and explicit writeback.
    const bool q_access = type == ir::ValueType::V128;
    const bool structured_guest_ea =
            q_access && StructuredAddressModeEnabled(context.GetFeatures()) &&
            !operand.GetRight().Null();
    const bool fold_host_base =
            HostBaseFoldEligible(memory_state.mem_hostbase_fold,
                                 memory_state.use_memory_base,
                                 memory_state.guest_addr_mask,
                                 type,
                                 structured_guest_ea,
                                 operand.GetOp() == ir::OperandOp::Plus,
                                 false);
    auto vixl_operand =
            EmitMemOperand(operand,
                           type,
                           false,
                           q_access && !fold_host_base,
                           !q_access,
                           structured_guest_ea,
                           inst);
    if (const auto* action = context.GetActions().ScalarFPRStore(inst->Id())) {
        auto source = VRegister::GetQRegFromCode(action->target);
        if (ir::GetValueSizeByte(type) == sizeof(u32)) {
            __ Str(source.S(), vixl_operand);
        } else {
            __ Str(source.D(), vixl_operand);
        }
        return;
    }
    const bool zero_gpr = CanUseZeroStoreRegister(value);
    const auto residence = FixedGPRHomeForUse(value, inst);
    const auto store_w = [&]() -> WRegister {
        if (zero_gpr) {
            return wzr;
        }
        if (residence && residence->width == ir::GetValueSizeByte(type)) {
            return WRegister(residence->home);
        }
        if (value.Def()) {
            if (const auto* prepared = context.GetActions().MemoryTransfer(value.Id());
                prepared && prepared->MatchesValue(*inst, value)) {
                return WRegister(prepared->home);
            }
            if (const auto home = GPRLowViewHome(value.Def())) {
                return WRegister(*home);
            }
        }
        return context.W(value);
    };
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Strb(store_w(), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Strh(store_w(), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Str(store_w(), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Str(zero_gpr ? xzr : context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Str(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Str(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Str(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Str(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Str(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

namespace {
// Walks Add/Sub-of-immediate and bitcast views off an address value,
// accumulating the constant offset.  Returns the remaining root value
// ({} when the walk started from an undefined value).
ir::Value AlignmentRootValue(ir::Value value, u64* offset) {
    while (value.Defined()) {
        auto* def = value.Def();
        if (def->IsBitCastOperation()) {
            value = def->GetArg<ir::Value>(0);
            continue;
        }
        if (def->GetOp() != ir::OpCode::Add && def->GetOp() != ir::OpCode::Sub) {
            break;
        }
        const auto right = def->GetArg<ir::Operand>(1);
        if (!right.GetRight().Null() || !right.GetLeft().IsImm()) {
            break;
        }
        const u64 imm = right.GetLeft().imm.Get();
        *offset += (def->GetOp() == ir::OpCode::Add ? imm : -imm);
        value = def->GetArg<ir::Value>(0);
    }
    return value;
}

// Root value of an effective-address operand plus its total constant
// offset from that root, or nullopt when the shape is not trackable.
std::optional<std::pair<ir::Value, u64>> AlignmentOperandRoot(
        const ir::Operand& operand) {
    ir::Value value{};
    u64 offset = 0;
    if (operand.GetRight().Null()) {
        if (!operand.GetLeft().IsValue()) return std::nullopt;
        value = operand.GetLeft().value;
    } else if (operand.GetOp() == ir::OperandOp::Plus &&
               operand.GetLeft().IsValue() && operand.GetRight().IsImm()) {
        value = operand.GetLeft().value;
        offset = operand.GetRight().imm.Get();
    } else {
        return std::nullopt;
    }
    auto root = AlignmentRootValue(value, &offset);
    if (!root.Defined()) return std::nullopt;
    return std::pair{root, offset};
}

// The pinned home read at the root of an address, if any: returns the
// home index and stores the total offset from the home value.
std::optional<u32> AlignmentHomeRoot(const ir::Operand& operand, u64* offset) {
    const auto rooted = AlignmentOperandRoot(operand);
    if (!rooted) return std::nullopt;
    auto* def = rooted->first.Def();
    if (!def || def->GetOp() != ir::OpCode::GetHostGPR ||
        def->GetArg<ir::Imm>(1).Get() != 0) {
        return std::nullopt;
    }
    *offset = rooted->second;
    return static_cast<u32>(def->GetArg<ir::Imm>(0).Get());
}
}  // namespace

std::optional<u32> JitTranslator::AlignmentResidueMod8(ir::Value value) const {
    u64 offset = 0;
    auto root = AlignmentRootValue(value, &offset);
    if (!root.Defined()) return std::nullopt;
    auto* def = root.Def();
    if (def->GetOp() == ir::OpCode::LoadImm) {
        return static_cast<u32>((def->GetArg<ir::Imm>(0).Get() + offset) & 7);
    }
    if (def->GetOp() == ir::OpCode::GetHostGPR &&
        def->GetArg<ir::Imm>(1).Get() == 0) {
        const auto known = memory_state.home_alignment_mod8.find(
                static_cast<u32>(def->GetArg<ir::Imm>(0).Get()));
        if (known != memory_state.home_alignment_mod8.end()) {
            return static_cast<u32>((known->second + offset) & 7);
        }
    }
    return std::nullopt;
}

void JitTranslator::TrackHomeAlignment(const ir::Inst* inst) {
    auto& homes = memory_state.home_alignment_mod8;
    if (homes.empty() && inst->GetOp() != ir::OpCode::SetHostGPR) {
        return;
    }
    switch (inst->GetOp()) {
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
            // Control leaves the block-local model: guest homes may be
            // rewritten by the callee/helper to values we cannot track.
            homes.clear();
            return;
        case ir::OpCode::SetHostGPR: {
            const u32 home = inst->GetArg<ir::Imm>(1).Get();
            if (inst->GetArg<ir::Imm>(2).Get() != 0) {
                homes.erase(home);
                return;
            }
            if (auto residue = AlignmentResidueMod8(inst->GetArg<ir::Value>(0))) {
                homes[home] = *residue;
            } else {
                homes.erase(home);
            }
            return;
        }
        case ir::OpCode::GetHostGPR:
            return;
        default:
            break;
    }
    // An ordinary producer allocated onto a tracked home rewrites it
    // just like a publication does.
    if (!inst->HasValue()) return;
    const auto value = ir::Value{inst};
    if (context.GetAllocation().ValueType(value) != compile::Allocation::GPR) {
        return;
    }
    const u32 home = context.GetAllocation().ValueGPR(value).id;
    if (!homes.contains(home)) return;
    if (auto residue = AlignmentResidueMod8(value)) {
        homes[home] = *residue;
    } else {
        homes.erase(home);
    }
}

void JitTranslator::EmitLoadMemoryTSO(const ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    const bool q_access = type == ir::ValueType::V128;
    const bool scalar = type < ir::ValueType::V8 || type > ir::ValueType::V256;
    const bool supports_rcpc =
            True(context.GetConfig().arm64_features & Arm64Features::RCpc);
    const auto size = ir::GetValueSizeByte(type);
    const bool static_address =
            operand.GetRight().Null() && operand.GetLeft().IsImm();
    const auto static_bias = reinterpret_cast<uintptr_t>(
            context.GetConfig().memory_base ? context.GetConfig().memory_base
                                            : context.GetConfig().page_table);
    const bool statically_unaligned =
            static_address && size > 1 &&
            ((operand.GetLeft().imm.Get() + static_bias) & (size - 1)) != 0;
    const bool scalar_fast_path =
            scalar && supports_rcpc && !statically_unaligned;
    // With rcpc_fault_emulate the dispatch below is skipped entirely:
    // unaligned or 16-byte-window-crossing accesses raise SIGBUS and the
    // runtime fault handler emulates them (see TryEmulateRcpcFault).
    // Units that exhausted their fault budget (RcpcForceChecks) compile
    // the checked form instead, byte-identical to non-emulate emission.
    const bool rcpc_unchecked = context.GetConfig().rcpc_fault_emulate &&
            !context.RcpcForceChecks();
    bool check_alignment =
            scalar_fast_path && size > 1 && !static_address &&
            !rcpc_unchecked;
    if (check_alignment && size == 8) {
        // The home's residue may already be proven even when this
        // address's SSA root differs (the push/pop re-read pattern).
        u64 home_offset = 0;
        if (auto home = AlignmentHomeRoot(operand, &home_offset)) {
            const auto known = memory_state.home_alignment_mod8.find(*home);
            if (known != memory_state.home_alignment_mod8.end() &&
                ((known->second + home_offset) & 7) == 0) {
                check_alignment = false;
            }
        }
    }
    tso_emission_stats.Increment(tso_emission_stats.load_sites);
    if (scalar_fast_path) {
        tso_emission_stats.Increment(tso_emission_stats.scalar_fast_sites);
    }
    if (check_alignment) {
        tso_emission_stats.Increment(tso_emission_stats.alignment_check_sites);
    }
    auto vixl_operand =
            EmitMemOperand(operand,
                           type,
                           false,
                           scalar_fast_path || q_access,
                           !scalar_fast_path && !q_access,
                           false,
                           inst);

    // FEAT_LRCPC2 (RCpcImm) adds unscaled-immediate RCpc forms.  For byte
    // accesses LDAPUR needs neither alignment checks nor a materialized
    // base when the operand already carries a simm9 offset.  Wider forms
    // stay on the LDAPR path below: an unaligned LDAPUR that crosses a
    // 16-byte window faults with SIGBUS on this host (measured), and the
    // branchy alignment check there is precisely what orders it.
    const bool supports_rcpc_imm =
            True(context.GetConfig().arm64_features & Arm64Features::RCpcImm);
    if (scalar && scalar_fast_path && supports_rcpc_imm && !check_alignment &&
        vixl_operand.IsImmediateOffset() &&
        vixl_operand.GetOffset() >= -256 && vixl_operand.GetOffset() <= 255) {
        bool emitted = true;
        {
            vixl::CPUFeaturesScope rcpc(&masm, vixl::CPUFeatures::kRCpc,
                                        vixl::CPUFeatures::kRCpcImm);
            switch (type) {
                case ir::ValueType::S8:
                case ir::ValueType::U8:
                    __ ldapurb(context.W(value), vixl_operand);
                    break;
                case ir::ValueType::S16:
                case ir::ValueType::U16:
                    __ ldapurh(context.W(value), vixl_operand);
                    break;
                case ir::ValueType::S32:
                case ir::ValueType::U32:
                    __ ldapur(context.W(value), vixl_operand);
                    break;
                case ir::ValueType::S64:
                case ir::ValueType::U64:
                    __ ldapur(context.X(value), vixl_operand);
                    break;
                default:
                    emitted = false;
                    break;
            }
        }
        if (emitted) {
            return;
        }
    }

    // FEAT_LRCPC's LDAPR is the scalar x86-TSO fast path. It requires a bare,
    // naturally aligned address, so materialize any offset and branch around
    // it for x86's permitted unaligned accesses. Byte accesses are naturally
    // aligned by definition. Hosts without LRCPC and vector accesses retain
    // the proven basic-load + dmb ishld half-barrier.
    if (scalar_fast_path) {
        Register address = vixl_operand.GetBaseRegister();
        if (!vixl_operand.IsImmediateOffset() || vixl_operand.GetOffset() != 0) {
            // x10 is reserved only for biased memory. In direct mode it can
            // hold a live SSA value, so address folding must use checked scratch.
            address = memory_state.use_memory_base ? mem_scratch : context.GetSharedTmpX();
            __ ComputeAddress(address, vixl_operand);
        }

        Label unaligned;
        Label cross_check;
        Label done;
        Register span;
        bool have_span = false;
        if (check_alignment) {
            // Guest condition codes may remain in NZCV across a memory
            // access. Test address bits without changing those flags. For
            // 8-byte accesses one extract+branch replaces three bit tests;
            // the extracted span is reused by the cross-window check.
            if (size == 8) {
                span = context.GetTmpX();
                have_span = true;
                __ Ubfx(span, address, 0, 4);
                __ Cbnz(span, supports_rcpc_imm ? &cross_check : &unaligned);
            } else {
                for (u32 bit = 0; (u32{1} << bit) < size; ++bit) {
                    __ Tbnz(address, bit, supports_rcpc_imm ? &cross_check : &unaligned);
                }
            }
        }
        {
            vixl::CPUFeaturesScope rcpc(&masm, vixl::CPUFeatures::kRCpc);
            switch (type) {
                case ir::ValueType::S8:
                case ir::ValueType::U8:
                    __ Ldaprb(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S16:
                case ir::ValueType::U16:
                    __ Ldaprh(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S32:
                case ir::ValueType::U32:
                    __ Ldapr(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S64:
                case ir::ValueType::U64:
                    __ Ldapr(context.X(value), MemOperand(address));
                    break;
                default:
                    PANIC("unsupported scalar TSO load width");
            }
        }
        if (size == 1) {
            return;
        }
        if (!check_alignment) {
            return;
        }
        __ B(&done);
        if (supports_rcpc_imm) {
            // LDAPUR accepts unaligned accesses that stay within one
            // 16-byte window; crossing accesses fault on this host class
            // (measured), so divert exactly those to the dmb fallback.
            __ Bind(&cross_check);
            if (!have_span) {
                span = context.GetTmpX();
                __ Ubfx(span, address, 0, 4);
            }
            __ Add(span, span, size - 1);
            __ Tbnz(span, 4, &unaligned);
            {
                vixl::CPUFeaturesScope rcpc(&masm, vixl::CPUFeatures::kRCpc,
                                            vixl::CPUFeatures::kRCpcImm);
                switch (type) {
                    case ir::ValueType::S16:
                    case ir::ValueType::U16:
                        __ ldapurh(context.W(value), MemOperand(address));
                        break;
                    case ir::ValueType::S32:
                    case ir::ValueType::U32:
                        __ ldapur(context.W(value), MemOperand(address));
                        break;
                    case ir::ValueType::S64:
                    case ir::ValueType::U64:
                        __ ldapur(context.X(value), MemOperand(address));
                        break;
                    default:
                        PANIC("unsupported unaligned scalar TSO load width");
                }
            }
            __ B(&done);
        }
        __ Bind(&unaligned);
        switch (type) {
            case ir::ValueType::S16:
            case ir::ValueType::U16:
                __ Ldrh(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S32:
            case ir::ValueType::U32:
                __ Ldr(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S64:
            case ir::ValueType::U64:
                __ Ldr(context.X(value), MemOperand(address));
                break;
            default:
                PANIC("unsupported unaligned scalar TSO load width");
        }
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierReads);
        __ Bind(&done);
        return;
    }

    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldrb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldrh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Ldr(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Ldr(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Ldr(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Ldr(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Ldr(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
    // Acquire half: no later load/store may be observed before this one.
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierReads);
}

void JitTranslator::EmitStoreMemoryTSO(const ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    const bool q_access = type == ir::ValueType::V128;
    const bool scalar = type < ir::ValueType::V8 || type > ir::ValueType::V256;
    const bool supports_rcpc =
            True(context.GetConfig().arm64_features & Arm64Features::RCpc);
    const auto size = ir::GetValueSizeByte(type);
    const bool static_address =
            operand.GetRight().Null() && operand.GetLeft().IsImm();
    const auto static_bias = reinterpret_cast<uintptr_t>(
            context.GetConfig().memory_base ? context.GetConfig().memory_base
                                            : context.GetConfig().page_table);
    const bool statically_unaligned =
            static_address && size > 1 &&
            ((operand.GetLeft().imm.Get() + static_bias) & (size - 1)) != 0;
    const bool scalar_fast_path =
            scalar && supports_rcpc && !statically_unaligned;
    // With rcpc_fault_emulate the dispatch below is skipped entirely:
    // unaligned or 16-byte-window-crossing accesses raise SIGBUS and the
    // runtime fault handler emulates them (see TryEmulateRcpcFault).
    // RcpcForceChecks units compile the checked form (see load path).
    const bool rcpc_unchecked = context.GetConfig().rcpc_fault_emulate &&
            !context.RcpcForceChecks();
    bool check_alignment =
            scalar_fast_path && size > 1 && !static_address &&
            !(rcpc_unchecked &&
              True(context.GetConfig().arm64_features & Arm64Features::RCpcImm));
    if (check_alignment && size == 8) {
        u64 home_offset = 0;
        if (auto home = AlignmentHomeRoot(operand, &home_offset)) {
            const auto known = memory_state.home_alignment_mod8.find(*home);
            if (known != memory_state.home_alignment_mod8.end() &&
                ((known->second + home_offset) & 7) == 0) {
                check_alignment = false;
            }
        }
    }
    tso_emission_stats.Increment(tso_emission_stats.store_sites);
    if (scalar_fast_path) {
        tso_emission_stats.Increment(tso_emission_stats.scalar_fast_sites);
    }
    if (check_alignment) {
        tso_emission_stats.Increment(tso_emission_stats.alignment_check_sites);
    }
    auto vixl_operand =
            EmitMemOperand(operand,
                           type,
                           false,
                           scalar_fast_path || q_access,
                           !scalar_fast_path && !q_access,
                           false,
                           inst);

    // STLUR mirror of the LDAPUR load fast path above: byte accesses
    // with a simm9 immediate offset need neither alignment checks nor
    // address materialization.
    const bool supports_rcpc_imm =
            True(context.GetConfig().arm64_features & Arm64Features::RCpcImm);
    if (scalar && scalar_fast_path && supports_rcpc_imm && !check_alignment &&
        vixl_operand.IsImmediateOffset() &&
        vixl_operand.GetOffset() >= -256 && vixl_operand.GetOffset() <= 255) {
        bool emitted = true;
        {
            vixl::CPUFeaturesScope rcpc(&masm, vixl::CPUFeatures::kRCpc,
                                        vixl::CPUFeatures::kRCpcImm);
            switch (type) {
                case ir::ValueType::S8:
                case ir::ValueType::U8:
                    __ stlurb(context.W(value), vixl_operand);
                    break;
                case ir::ValueType::S16:
                case ir::ValueType::U16:
                    __ stlurh(context.W(value), vixl_operand);
                    break;
                case ir::ValueType::S32:
                case ir::ValueType::U32:
                    __ stlur(context.W(value), vixl_operand);
                    break;
                case ir::ValueType::S64:
                case ir::ValueType::U64:
                    __ stlur(context.X(value), vixl_operand);
                    break;
                default:
                    emitted = false;
                    break;
            }
        }
        if (emitted) {
            return;
        }
    }

    // Gate the complete scalar fast path with the same check as LDAPR. This
    // keeps non-LRCPC hosts on the previous dmb+str implementation and makes
    // SVM_ARM64_LRCPC=0 an exact A/B baseline.
    if (scalar_fast_path) {
        Register address = vixl_operand.GetBaseRegister();
        if (!vixl_operand.IsImmediateOffset() || vixl_operand.GetOffset() != 0) {
            address = memory_state.use_memory_base ? mem_scratch : context.GetSharedTmpX();
            __ ComputeAddress(address, vixl_operand);
        }

        Label unaligned;
        Label cross_check;
        Label done;
        Register span;
        bool have_span = false;
        if (check_alignment) {
            // Same NZCV-preserving check as the load path: one extract+branch
            // for 8-byte accesses, bit tests below that.
            if (size == 8) {
                span = context.GetTmpX();
                have_span = true;
                __ Ubfx(span, address, 0, 4);
                __ Cbnz(span, supports_rcpc_imm ? &cross_check : &unaligned);
            } else {
                for (u32 bit = 0; (u32{1} << bit) < size; ++bit) {
                    __ Tbnz(address, bit, supports_rcpc_imm ? &cross_check : &unaligned);
                }
            }
        }
        if (rcpc_unchecked && size > 1) {
            vixl::CPUFeaturesScope rcpc(&masm, vixl::CPUFeatures::kRCpc,
                                        vixl::CPUFeatures::kRCpcImm);
            switch (type) {
                case ir::ValueType::S16:
                case ir::ValueType::U16:
                    __ stlurh(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S32:
                case ir::ValueType::U32:
                    __ stlur(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S64:
                case ir::ValueType::U64:
                    __ stlur(context.X(value), MemOperand(address));
                    break;
                default:
                    PANIC("unsupported scalar TSO store width");
            }
        } else {
            switch (type) {
                case ir::ValueType::S8:
                case ir::ValueType::U8:
                    __ Stlrb(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S16:
                case ir::ValueType::U16:
                    __ Stlrh(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S32:
                case ir::ValueType::U32:
                    __ Stlr(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S64:
                case ir::ValueType::U64:
                    __ Stlr(context.X(value), MemOperand(address));
                    break;
                default:
                    PANIC("unsupported scalar TSO store width");
            }
        }
        if (size == 1) {
            return;
        }
        if (!check_alignment) {
            return;
        }
        __ B(&done);
        if (supports_rcpc_imm) {
            // STLUR mirror of the load cross-check: unaligned stores that
            // stay within one 16-byte window keep release semantics
            // without the full barrier; crossing stores keep dmb+str.
            __ Bind(&cross_check);
            if (!have_span) {
                span = context.GetTmpX();
                __ Ubfx(span, address, 0, 4);
            }
            __ Add(span, span, size - 1);
            __ Tbnz(span, 4, &unaligned);
            {
                vixl::CPUFeaturesScope rcpc(&masm, vixl::CPUFeatures::kRCpc,
                                            vixl::CPUFeatures::kRCpcImm);
                switch (type) {
                    case ir::ValueType::S16:
                    case ir::ValueType::U16:
                        __ stlurh(context.W(value), MemOperand(address));
                        break;
                    case ir::ValueType::S32:
                    case ir::ValueType::U32:
                        __ stlur(context.W(value), MemOperand(address));
                        break;
                    case ir::ValueType::S64:
                    case ir::ValueType::U64:
                        __ stlur(context.X(value), MemOperand(address));
                        break;
                    default:
                        PANIC("unsupported unaligned scalar TSO store width");
                }
            }
            __ B(&done);
        }
        __ Bind(&unaligned);
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
        switch (type) {
            case ir::ValueType::S16:
            case ir::ValueType::U16:
                __ Strh(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S32:
            case ir::ValueType::U32:
                __ Str(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S64:
            case ir::ValueType::U64:
                __ Str(context.X(value), MemOperand(address));
                break;
            default:
                PANIC("unsupported unaligned scalar TSO store width");
        }
        __ Bind(&done);
        return;
    }

    // Vector stores have no release form on the baseline used here.
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Strb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Strh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Str(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Str(context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Str(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Str(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Str(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Str(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Str(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

void JitTranslator::EmitMemoryCopy(const ir::Inst* inst) {
    auto dst = inst->GetArg<ir::Lambda>(0);
    auto src = inst->GetArg<ir::Lambda>(1);
    const auto size = inst->GetArg<ir::Imm>(2).Get();

    if (size == 0) {
        return;
    }

    MergeNZCV(FlagsRegsAuditMergeCause::Helper,
              FlagsRegsAuditEdgeKind::Host);
    FlushFlags();

    // A memory copy is rare but needs true memmove overlap semantics. Call a
    // host helper after preserving every register the host ABI may clobber.
    // Saving all SIMD registers is deliberate: unlike a normal C++ caller,
    // this JIT can keep a live guest Q value in any V register, including the
    // ABI-volatile range.
    constexpr u32 kGprSaveBytes = 160;
    constexpr u32 kVRegSaveBytes = 32 * 16;
    constexpr u32 kSaveBytes = kGprSaveBytes + kVRegSaveBytes;
    static_assert((kSaveBytes % 16) == 0);

    auto saved_gpr_offset = [](u32 code) -> u32 {
        if (code <= 10) {
            return code * 8;
        }
        switch (code) {
            case 12: return 88;
            case 13: return 96;
            case 14: return 104;
            case 15: return 112;
            case 16: return 120;
            case 17: return 128;
            default: PANIC();
        }
    };
    auto load_lambda = [&](const ir::Lambda& lambda, const XRegister& target) {
        if (!lambda.IsValue()) {
            __ Mov(target, lambda.GetImm().Get());
            return;
        }
        auto value = lambda.GetValue();
        auto source = context.X(value);
        if (source.GetCode() <= 10 || (source.GetCode() >= 12 && source.GetCode() <= 17)) {
            __ Ldr(target, MemOperand(sp, saved_gpr_offset(source.GetCode())));
        } else {
            __ Mov(target, source);
        }
    };

    __ Sub(sp, sp, kSaveBytes);
    __ Stp(x0, x1, MemOperand(sp, 0));
    __ Stp(x2, x3, MemOperand(sp, 16));
    __ Stp(x4, x5, MemOperand(sp, 32));
    __ Stp(x6, x7, MemOperand(sp, 48));
    __ Stp(x8, x9, MemOperand(sp, 64));
    __ Stp(x10, x12, MemOperand(sp, 80));
    __ Stp(x13, x14, MemOperand(sp, 96));
    __ Stp(x15, x16, MemOperand(sp, 112));
    __ Str(x17, MemOperand(sp, 128));
    __ Stp(x29, x30, MemOperand(sp, 144));
    for (u32 i = 0; i < 32; ++i) {
        __ Str(VRegister::GetVRegFromCode(i).Q(), MemOperand(sp, kGprSaveBytes + i * 16));
    }

    load_lambda(dst, x0);
    load_lambda(src, x1);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(x0, x0);
        EmitGuestToHost(x1, x1);
    }
    __ Mov(x2, size);
    if (sse_afp_nan) {
        __ Ldr(ip0,
               MemOperand(sp, kSaveBytes + kSseAFPHostFPCROffset));
        __ Msr(FPCR, ip0);
    }
    __ Mov(ip, reinterpret_cast<uintptr_t>(&HostMemMove));
    __ Blr(ip);
    if (sse_afp_nan) {
        EmitSseAFPRestoreGuestFPCRCached(
                masm,
                state,
                kSaveBytes,
                ip,
                ip0,
                ip1);
    }

    for (u32 i = 0; i < 32; ++i) {
        __ Ldr(VRegister::GetVRegFromCode(i).Q(), MemOperand(sp, kGprSaveBytes + i * 16));
    }
    __ Ldp(x0, x1, MemOperand(sp, 0));
    __ Ldp(x2, x3, MemOperand(sp, 16));
    __ Ldp(x4, x5, MemOperand(sp, 32));
    __ Ldp(x6, x7, MemOperand(sp, 48));
    __ Ldp(x8, x9, MemOperand(sp, 64));
    __ Ldp(x10, x12, MemOperand(sp, 80));
    __ Ldp(x13, x14, MemOperand(sp, 96));
    __ Ldp(x15, x16, MemOperand(sp, 112));
    __ Ldr(x17, MemOperand(sp, 128));
    __ Ldp(x29, x30, MemOperand(sp, 144));
    __ Add(sp, sp, kSaveBytes);
}

void JitTranslator::EmitMemoryCopyTSO(const ir::Inst* inst) {
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
    EmitMemoryCopy(inst);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitMemoryBarrierTSO(const ir::Inst* inst) {
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitCompareAndSwap(const ir::Inst* inst) {
    // Args: (address, expected, desired); returns the old value.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    auto expected = inst->GetArg<ir::Value>(1);
    auto desired = inst->GetArg<ir::Value>(2);
    auto type = expected.Type();
    auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);

    // Exclusive instructions take a base register only (no offset forms), so
    // under guest address virtualization the pt bias must be folded in
    // explicitly (reserved scratch: CAS is VOID-adjacent and GetTmpX cannot
    // be trusted here — see defines.h mem_scratch).
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    const bool lse = CanUseLSE();
    if (!lse) {
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
    }

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        if (auto* fallback = GetUnalignedAtomicFallback(inst)) {
            Label fallback_return;
            __ Adr(atomic_pair_scratch, &fallback_return);
            __ B(fallback);
            __ Bind(&fallback_return);
        } else {
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
            LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
            AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
            EmitBasicAtomicLoad(type, result, address);
            Label cas_done;
            __ Cmp(result, context.R(expected, true));
            __ B(&cas_done, ne);
            EmitBasicAtomicStore(type, context.R(desired, true), address);
            __ Bind(&cas_done);
            ReleaseUnalignedAtomicLock(atomic_pair_scratch);
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
        }
        __ B(&done);
    }

    __ Bind(&aligned);
    if (lse) {
        EmitLSECompareAndSwap(type,
                              result,
                              context.R(expected, true),
                              context.R(desired, true),
                              address);
        __ Bind(&done);
        return;
    }
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cmp(result, context.R(expected, true));
    __ B(&done, ne);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Stlxrb(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Stlxrh(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Stlxr(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Stlxr(ipw, context.X(desired), MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitCompareAndSwap128(const ir::Inst* inst) {
    // Args: (address, expected_lo, expected_hi, desired_lo, desired_hi);
    // returns the observed pair as V128, low half in lane 0.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto expected_lo = inst->GetArg<ir::Value>(1);
    const auto expected_hi = inst->GetArg<ir::Value>(2);
    const auto desired_lo = inst->GetArg<ir::Value>(3);
    const auto desired_hi = inst->GetArg<ir::Value>(4);
    const auto result = context.V(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label fallback_no_store;
    Label aligned_observed;
    Label done;
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);

    // The locked frontend rejects misalignment before this instruction. The
    // no-LOCK form is architecturally legal when unaligned (confirmed under
    // Rosetta), so preserve it with a serialized basic pair load/store.
    __ Tst(address, 15);
    __ B(&aligned, eq);
    LoadUnalignedAtomicLockAddress(atomic_scratch);
    AcquireUnalignedAtomicLock(atomic_scratch, atomic_pair_scratch);
    __ Ldp(atomic_scratch, atomic_pair_scratch, MemOperand(address));
    __ Cmp(atomic_scratch, context.X(expected_lo));
    __ B(&fallback_no_store, ne);
    __ Cmp(atomic_pair_scratch, context.X(expected_hi));
    __ B(&fallback_no_store, ne);
    __ Stp(context.X(desired_lo), context.X(desired_hi), MemOperand(address));
    __ Bind(&fallback_no_store);
    __ Ins(result.V2D(), 0, atomic_scratch);
    __ Ins(result.V2D(), 1, atomic_pair_scratch);
    LoadUnalignedAtomicLockAddress(atomic_scratch);
    ReleaseUnalignedAtomicLock(atomic_scratch);
    __ B(&done);

    __ Bind(&aligned);
    __ Bind(&retry);
    __ Ldaxp(atomic_scratch, atomic_pair_scratch, MemOperand(address));
    __ Cmp(atomic_scratch, context.X(expected_lo));
    __ B(&aligned_observed, ne);
    __ Cmp(atomic_pair_scratch, context.X(expected_hi));
    __ B(&aligned_observed, ne);
    __ Stlxp(ipw,
             context.X(desired_lo),
             context.X(desired_hi),
             MemOperand(address));
    __ Cbnz(ipw, &retry);
    __ B(&aligned_observed);

    __ Bind(&aligned_observed);
    // Clear a still-live monitor after a compare mismatch. It is harmless
    // after a successful STLXP and keeps the exclusive state local to this op.
    __ Clrex();
    __ Ins(result.V2D(), 0, atomic_scratch);
    __ Ins(result.V2D(), 1, atomic_pair_scratch);

    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitAtomicExchange(const ir::Inst* inst) {
    // Args: (address, desired); returns the previous value. Memory XCHG is
    // implicitly locked on x86, so use an unconditional exclusive loop and
    // full barriers regardless of the configured ordinary-memory TSO mode.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto desired = inst->GetArg<ir::Value>(1);
    const auto type = desired.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    const bool lse = CanUseLSE();
    if (!lse) {
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
    }

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        if (auto* fallback = GetUnalignedAtomicFallback(inst)) {
            Label fallback_return;
            __ Adr(atomic_pair_scratch, &fallback_return);
            __ B(fallback);
            __ Bind(&fallback_return);
        } else {
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
            LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
            AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
            EmitBasicAtomicLoad(type, result, address);
            // The store selects the low byte/halfword itself. Extracting a
            // narrow fixed-home input would lease an unnecessary temporary
            // on each emitted path, in addition to the flags publication.
            EmitBasicAtomicStore(type, context.R(desired), address);
            ReleaseUnalignedAtomicLock(atomic_pair_scratch);
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
        }
        __ B(&done);
    }

    __ Bind(&aligned);
    if (lse) {
        EmitLSEExchange(type, result, context.R(desired), address);
        __ Bind(&done);
        return;
    }
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            __ Stlxrb(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            __ Stlxrh(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            __ Stlxr(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            __ Stlxr(ipw, context.X(desired), MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitAtomicFetchAdd(const ir::Inst* inst) {
    // Args: (address, addend); returns the previous value. Used by LOCK XADD.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto addend = inst->GetArg<ir::Value>(1);
    const auto type = addend.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    const bool lse = CanUseLSE();
    if (!lse) {
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
    }

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        if (auto* fallback = GetUnalignedAtomicFallback(inst)) {
            Label fallback_return;
            __ Adr(atomic_pair_scratch, &fallback_return);
            __ B(fallback);
            __ Bind(&fallback_return);
        } else {
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
            LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
            AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
            EmitBasicAtomicLoad(type, result, address);
            if (ir::GetValueSizeByte(type) == 8) {
                __ Add(atomic_scratch, result, context.X(addend));
            } else {
                __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            }
            EmitBasicAtomicStore(type, atomic_scratch, address);
            ReleaseUnalignedAtomicLock(atomic_pair_scratch);
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
        }
        __ B(&done);
    }

    __ Bind(&aligned);
    if (lse) {
        EmitLSEFetchAdd(type, result, context.R(addend, true), address);
        __ Bind(&done);
        return;
    }
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            __ Stlxrb(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            __ Stlxrh(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            __ Stlxr(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            __ Add(atomic_scratch, result, context.X(addend));
            __ Stlxr(ipw, atomic_scratch, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitAtomicRMW(const ir::Inst* inst) {
    // Args: (operation, address, operand, carry); returns the previous value.
    const auto op =
            static_cast<ir::AtomicRMWOp>(inst->GetArg<ir::Imm>(0).Get());
    auto address = context.X(inst->GetArg<ir::Value>(1));
    const auto operand = inst->GetArg<ir::Value>(2);
    const auto carry = inst->GetArg<ir::Value>(3);
    const auto type = operand.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
        AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
        EmitBasicAtomicLoad(type, result, address);
        EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
        EmitBasicAtomicStore(type, atomic_scratch, address);
        ReleaseUnalignedAtomicLock(atomic_pair_scratch);
        __ B(&done);
    }

    __ Bind(&aligned);
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxrb(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxrh(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxr(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxr(ipw, atomic_scratch, MemOperand(address));
            break;
        default:
            PANIC("unsupported AtomicRMW width");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitUniformBarrier(const ir::Inst* inst) {
    // Compiler barrier only: uniform caching is not invalidated across this point.
}

void JitTranslator::EmitXchgBarrier(const ir::Inst* inst) {
    // Metadata-only marker for fixed-home lifetime analysis.
}

}  // namespace swift::runtime::backend::arm64
