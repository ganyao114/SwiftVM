#include "translator.h"

#include <cstring>
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

void HostMemMove(void* dst, const void* src, size_t size) {
    std::memmove(dst, src, size);
}

}  // namespace

MemOperand JitTranslator::BiasMem(const Register& base, bool atomic) {
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
    // [guest base + imm + pt]: fold the immediate into the reserved scratch.
    if (imm > 0) {
        __ Add(mem_scratch, base, imm);
    } else {
        __ Sub(mem_scratch, base, -imm);
    }
    if (atomic) {
        __ Add(mem_scratch, mem_scratch, pt);
        return MemOperand{mem_scratch};
    }
    return MemOperand{mem_scratch, pt};
}

void JitTranslator::EmitGetHostGPR(ir::Inst* inst) {
    auto offset = inst->GetArg<ir::Imm>(1).Get();
    auto reg_index = inst->GetArg<ir::Imm>(0).Get();
    auto host_reg = XRegister(reg_index);
    auto ret_reg = context.X(ir::Value{inst});
    const auto bit_offset = offset * 8;
    const auto bit_width = ir::GetValueSizeByte(inst->ReturnType()) * 8;
    if (bit_offset == 0 && bit_width == 64) {
        if (host_reg != ret_reg) {
            __ Mov(ret_reg, host_reg);
        }
    } else {
        __ Ubfx(ret_reg, host_reg, bit_offset, bit_width);
    }
}

void JitTranslator::EmitGetHostFPR(ir::Inst* inst) {
    auto offset = inst->GetArg<ir::Imm>(1).Get();
    if (!offset) {
        return;
    }
    auto reg_index = inst->GetArg<ir::Imm>(0).Get();
    auto host_reg = VRegister::GetQRegFromCode(reg_index);
    auto ret_reg = context.V(ir::Value{inst});
    if (host_reg != ret_reg) {
        PANIC("GetHostFPR!");
    }
}

void JitTranslator::EmitSetHostGPR(ir::Inst* inst) {
    auto offset = inst->GetArg<ir::Imm>(2).Get();
    auto reg_index = inst->GetArg<ir::Imm>(1).Get();
    auto host_reg = XRegister(reg_index);
    auto value = inst->GetArg<ir::Value>(0);
    auto value_reg = context.X(value);
    const auto bit_offset = offset * 8;
    const auto bit_width = ir::GetValueSizeByte(value.Type()) * 8;
    if (bit_offset == 0 && bit_width == 64) {
        if (value_reg != host_reg) {
            __ Mov(host_reg, value_reg);
        }
    } else {
        __ Bfi(host_reg, value_reg, bit_offset, bit_width);
    }
}

void JitTranslator::EmitSetHostFPR(ir::Inst* inst) {

}

void JitTranslator::EmitLoadUniform(ir::Inst* inst) {
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

void JitTranslator::EmitGetUniformAddress(ir::Inst* inst) {
    const auto offset = inst->GetArg<ir::Imm>(0).Get();
    auto result = context.X(ir::Value{inst});
    __ Add(result, state, offsetof(State, uniform_buffer_begin) + offset);
}

void JitTranslator::EmitStoreUniform(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    s32 offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto reg = context.Get(inst->GetArg<ir::Value>(1));
    auto value_type = inst->GetArg<ir::Value>(1).Type();
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
}

void JitTranslator::EmitLoadLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitStoreLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitLoadMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    // Q loads cannot use the register-offset encoding used for [base + pt],
    // and must not consume the synthetic post-index produced by the generic
    // address peephole. The ARM64 frontend lowers pair writeback into normal
    // Add + two memory operations, so folding it here would write back after
    // the first half of an LDP/STP pair rather than after the pair.
    const bool q_access = type == ir::ValueType::V128;
    auto vixl_operand = EmitMemOperand(operand, type, false, q_access, !q_access);
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
}

void JitTranslator::EmitStoreMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    // See EmitLoadMemory: materialize guest + pt for Q accesses and preserve
    // the frontend's explicit pre/post-index writeback Add instructions.
    const bool q_access = type == ir::ValueType::V128;
    auto vixl_operand = EmitMemOperand(operand, type, false, q_access, !q_access);
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

void JitTranslator::EmitLoadMemoryTSO(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    // Conservative strategy: plain load + a trailing `dmb ishld` (orders the
    // load before all later loads and stores, inner-shareable). This replaces
    // the old Ldar* implementation: Ldar*/Ldapr* only encode [Xn] (no offset
    // forms) and, more importantly, fault on the unaligned accesses x86
    // permits (glibc init_cpu_features does 4-mod-8 qword accesses -> SIGBUS).
    // A possible future optimization is a runtime alignment check
    // (tst addr, width-1; b.ne slow) gating an ldapr/stlr fast path.
    const bool q_access = type == ir::ValueType::V128;
    auto vixl_operand = EmitMemOperand(operand, type, false, q_access, !q_access);
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
    __ Dmb(InnerShareable, BarrierReads);
}

void JitTranslator::EmitStoreMemoryTSO(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    // Release half: `dmb ish` drains all prior loads/stores before this store
    // becomes visible (Stlr* faults on unaligned addresses — see
    // EmitLoadMemoryTSO for why the plain-store + barrier form is used).
    __ Dmb(InnerShareable, BarrierAll);
    const bool q_access = type == ir::ValueType::V128;
    auto vixl_operand = EmitMemOperand(operand, type, false, q_access, !q_access);
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
        // Vector stores have no release form either; the barrier above still
        // orders them (plain store inside the barrier sandwich).
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

void JitTranslator::EmitMemoryCopy(ir::Inst* inst) {
    auto dst = inst->GetArg<ir::Lambda>(0);
    auto src = inst->GetArg<ir::Lambda>(1);
    const auto size = inst->GetArg<ir::Imm>(2).Get();

    if (size == 0) {
        return;
    }

    MergeNZCV();
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
    if (use_memory_base) {
        __ Add(x0, x0, pt);
        __ Add(x1, x1, pt);
    }
    __ Mov(x2, size);
    __ Mov(ip, reinterpret_cast<uintptr_t>(&HostMemMove));
    __ Blr(ip);

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

void JitTranslator::EmitMemoryCopyTSO(ir::Inst* inst) {
    __ Dmb(InnerShareable, BarrierAll);
    EmitMemoryCopy(inst);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitCompareAndSwap(ir::Inst* inst) {
    // Args: (address, expected, desired); returns the old value.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    auto expected = inst->GetArg<ir::Value>(1);
    auto desired = inst->GetArg<ir::Value>(2);
    auto type = expected.Type();
    auto result = context.R(ir::Value{inst});

    MergeNZCV();

    // Exclusive instructions take a base register only (no offset forms), so
    // under guest address virtualization the pt bias must be folded in
    // explicitly (reserved scratch: CAS is VOID-adjacent and GetTmpX cannot
    // be trusted here — see defines.h mem_scratch).
    if (use_memory_base) {
        __ Add(mem_scratch, address, pt);
        address = mem_scratch;
    }

    Label retry;
    Label done;
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
}

void JitTranslator::EmitUniformBarrier(ir::Inst* inst) {
    // Compiler barrier only: uniform caching is not invalidated across this point.
}

}  // namespace swift::runtime::backend::arm64
