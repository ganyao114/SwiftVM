#include "translator.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/fpcr_mode.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

bool StructuredAddressModeEnabled() {
    static const bool enabled = [] {
        const char* env = PerfGetenv("SVM_ADDRMODE_STRUCT");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

void HostMemMove(void* dst, const void* src, size_t size) {
    std::memmove(dst, src, size);
}

struct TsoEmissionStats {
    const bool enabled{PerfGetenv("SVM_TSO_STATS") != nullptr};
    std::atomic<u64> load_sites{};
    std::atomic<u64> store_sites{};
    std::atomic<u64> scalar_fast_sites{};
    std::atomic<u64> alignment_check_sites{};
    std::atomic<u64> dmb_instructions{};

    ~TsoEmissionStats() {
        if (enabled) {
            std::fprintf(stderr,
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

void JitTranslator::AcquireUnalignedAtomicLock(const Register& scratch) {
    Label retry;
    __ Mov(atomic_scratch,
           reinterpret_cast<uintptr_t>(&runtime::backend::unaligned_atomic_lock));
    __ Bind(&retry);
    __ Ldaxr(scratch.W(), MemOperand(atomic_scratch));
    __ Cbnz(scratch.W(), &retry);
    __ Mov(scratch.W(), 1);
    __ Stxr(ipw, scratch.W(), MemOperand(atomic_scratch));
    __ Cbnz(ipw, &retry);
}

void JitTranslator::ReleaseUnalignedAtomicLock() {
    __ Mov(atomic_scratch,
           reinterpret_cast<uintptr_t>(&runtime::backend::unaligned_atomic_lock));
    __ Stlr(wzr, MemOperand(atomic_scratch));
}

void JitTranslator::EmitPlainAtomicLoad(ir::ValueType type,
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

void JitTranslator::EmitPlainAtomicStore(ir::ValueType type,
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
    if (window_uxtw) {
        // pt + zext32(guest): one instruction, same as the unbounded Add.
        __ Add(dst, pt, Operand(guest_addr.W(), UXTW));
        return;
    }
    if (guest_addr_mask) {
        __ And(dst, guest_addr, guest_addr_mask);
        __ Add(dst, dst, pt);
        return;
    }
    __ Add(dst, guest_addr, pt);
}

MemOperand JitTranslator::BiasMem(const Register& base, bool atomic) {
    if (window_uxtw) {
        // Bounded 32-bit guest window: [pt, Wbase, UXTW] is the *same*
        // register-offset load the unbounded path emitted, with the
        // truncation folded into the addressing mode — zero extra cost.
        if (!atomic) {
            return MemOperand{pt, base.W(), UXTW};
        }
        __ Add(mem_scratch, pt, Operand(base.W(), UXTW));
        return MemOperand{mem_scratch};
    }
    if (guest_addr_mask) {
        // Non-32-bit window: one extra `and` with a logical immediate.
        __ And(mem_scratch, base, guest_addr_mask);
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
    if (window_uxtw) {
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
    if (guest_addr_mask) {
        __ And(mem_scratch, mem_scratch, guest_addr_mask);
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
    const u32 value_size = ir::GetValueSizeByte(inst->ReturnType());
    const bool pin_ext_reg =
            reg_index <= 9 || reg_index == 22 || reg_index == 23 || reg_index == 29;
    if (offset == 0 && pin_ext_reg &&
        inst->GetUses() == 1 && value_size <= sizeof(u32)) {
        auto& list = cur_block->GetInstList();
        for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
            if (it->GetOp() == ir::OpCode::SetHostGPR &&
                it->GetArg<ir::Imm>(1).Get() == reg_index) {
                break;  // the materialized read must retain snapshot semantics
            }
            bool names_value = false;
            for (auto used : it->GetValues()) {
                names_value |= used.Def() == inst;
            }
            if (!names_value) {
                continue;
            }
            // Narrow And/Xor reads stay fused for the callee-saved pins
            // (22/23/29) below level 3 — the W56-proven shape. The x6-x9
            // caller-saved pins keep codex's conservative restriction.
            const bool direct_alu =
                    (it->GetOp() == ir::OpCode::And || it->GetOp() == ir::OpCode::Xor) &&
                    ir::GetValueSizeByte(it->ReturnType()) <= sizeof(u32) &&
                    (reg_index <= 5 || value_size == sizeof(u32) ||
                     (reg_index >= 22 && !backend::X86PinExtLevel3Requested()));
            const bool direct_caller_pin_alu =
                    reg_index <= 9 &&
                    (it->GetOp() == ir::OpCode::Add || it->GetOp() == ir::OpCode::Sub) &&
                    ir::GetValueSizeByte(it->ReturnType()) <= sizeof(u32) &&
                    (reg_index <= 5 || value_size == sizeof(u32));
            const bool direct_extend =
                    (value_size == sizeof(u8) || value_size == sizeof(u16)) &&
                    it->GetOp() == ir::OpCode::ZeroExtend32 &&
                    it->GetArg<ir::Value>(0).Def() == inst;
            if (direct_alu || direct_caller_pin_alu || direct_extend) {
                fused_pin_gpr_reads.emplace(inst, static_cast<u16>(reg_index));
                return;
            }
            break;
        }
    }
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

void JitTranslator::EmitGetHostFPR(ir::Inst* inst) {
    auto reg_index = inst->GetArg<ir::Imm>(0).Get();
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

void JitTranslator::EmitSetHostGPR(ir::Inst* inst) {
    auto offset = inst->GetArg<ir::Imm>(2).Get();
    auto reg_index = inst->GetArg<ir::Imm>(1).Get();
    auto host_reg = XRegister(reg_index);
    auto value = inst->GetArg<ir::Value>(0);
    const bool fused_zext32 = value.Def() && fused_pin_zext32.contains(value.Def());
    auto value_reg = fused_zext32
            ? context.X(value.Def()->GetArg<ir::Value>(0))
            : context.X(value);
    const auto bit_offset = offset * 8;
    const auto bit_width = ir::GetValueSizeByte(value.Type()) * 8;
    ASSERT_MSG(bit_offset + bit_width <= 64,
               "invalid fixed GPR write offset {} width {}", bit_offset, bit_width);
    const bool pin_ext_reg =
            reg_index <= 9 || reg_index == 22 || reg_index == 23 || reg_index == 29;
    if (bit_offset == 0 && bit_width == 32 && pin_ext_reg) {
        if (value_reg.W() != host_reg.W()) {
            __ Mov(host_reg.W(), value_reg.W());
        }
    } else if (bit_offset == 0 && bit_width == 64) {
        if (value_reg != host_reg) {
            // x86-64 EAX/ECX/EDX writes reach here as a U64
            // ZeroExtend32To64 value so memory-backed StoreUniform can still
            // replace all eight context bytes. For the W55 pinned registers,
            // use the architectural W write: AArch64 clears bits [63:32]
            // naturally, exactly matching the x86 rule.
            const bool zext32 = fused_zext32 || (value.Def() &&
                    value.Def()->GetOp() == ir::OpCode::ZeroExtend32To64);
            if (pin_ext_reg && zext32) {
                __ Mov(host_reg.W(), value_reg.W());
            } else {
                __ Mov(host_reg, value_reg);
            }
        }
    } else {
        // Low byte/word and AH/CH/DH (offset == 1) all lower to one BFI,
        // preserving every untouched bit of the pinned 64-bit parent.
        __ Bfi(host_reg, value_reg, bit_offset, bit_width);
    }
}

void JitTranslator::EmitSetHostFPR(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    const u32 reg_index = inst->GetArg<ir::Imm>(1).Get();
    const u32 offset = inst->GetArg<ir::Imm>(2).Get();
    const u32 size = ir::GetValueSizeByte(value.Type());
    auto host_reg = VRegister::GetQRegFromCode(reg_index);
    ASSERT_MSG(offset + size <= sizeof(u128) && offset % size == 0,
               "invalid fixed FPR write offset {} size {}", offset, size);

    if (!ir::IsFloatValueType(value.Type())) {
        auto source = context.R(value);
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
    if (sse_afp_nan &&
        uni.GetOffset() == offsetof(swift::x86::ThreadContext64, mxcsr)) {
        context.RecordFpcrTaxCounter(FpcrTaxCounter::StoreMxcsr);
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
                bit,
                [this](FpcrTaxCounter counter) {
                    context.RecordFpcrTaxCounter(counter);
                });
    }
}

void JitTranslator::EmitLoadLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitStoreLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitLoadMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    ir::Inst* narrow_consumer = nullptr;
    if (mem_narrow_fuse && ir::GetValueSizeByte(type) <= 2 && inst->GetUses() == 1) {
        auto& list = cur_block->GetInstList();
        auto it = list.iterator_to(*inst);
        for (++it; it != list.end(); ++it) {
            bool uses_load = false;
            for (auto used : it->GetValues()) {
                uses_load |= used.Def() == inst;
            }
            if (!uses_load) {
                continue;
            }
            const bool extending =
                    (it->GetOp() == ir::OpCode::SignExtend ||
                     it->GetOp() == ir::OpCode::ZeroExtend32) &&
                    it->GetArg<ir::Value>(0).Def() == inst;
            if (extending && context.SharesGPR(value, ir::Value{it.operator->()})) {
                narrow_consumer = it.operator->();
            }
            break;
        }
    }
    // Keep the established materialized-address path for generic Q loads and
    // do not consume the synthetic post-index produced by the generic address
    // peephole. A1 opens only the exact bounded W39 Plus form, for which the
    // SIMD register-offset encoding can carry pt + UXTW(guest EA) directly.
    // The ARM64 frontend lowers pair writeback into normal Add + two memory
    // operations, so folding writeback here would update after the first half.
    const bool q_access = type == ir::ValueType::V128;
    const bool structured_guest_ea =
            q_access && StructuredAddressModeEnabled() && !operand.GetRight().Null();
    const bool fold_host_base =
            HostBaseFoldEligible(mem_hostbase_fold,
                                 use_memory_base,
                                 guest_addr_mask,
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
                           structured_guest_ea);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            if (narrow_consumer && narrow_consumer->GetOp() == ir::OpCode::SignExtend) {
                if (ir::GetValueSizeByte(narrow_consumer->ReturnType()) == 8) {
                    __ Ldrsb(context.X(value), vixl_operand);
                } else {
                    __ Ldrsb(context.W(value), vixl_operand);
                }
            } else {
                // LDRB's W destination already performs ZeroExtend32.
                __ Ldrb(context.W(value), vixl_operand);
            }
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            if (narrow_consumer && narrow_consumer->GetOp() == ir::OpCode::SignExtend) {
                if (ir::GetValueSizeByte(narrow_consumer->ReturnType()) == 8) {
                    __ Ldrsh(context.X(value), vixl_operand);
                } else {
                    __ Ldrsh(context.W(value), vixl_operand);
                }
            } else {
                // LDRH's W destination already performs ZeroExtend32.
                __ Ldrh(context.W(value), vixl_operand);
            }
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
    if (narrow_consumer) {
        disable_instructions.set(narrow_consumer->Id());
    }
}

void JitTranslator::EmitStoreMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    // See EmitLoadMemory: A1 opens only the exact bounded W39 Plus form; all
    // other Q accesses keep address materialization and explicit writeback.
    const bool q_access = type == ir::ValueType::V128;
    const bool structured_guest_ea =
            q_access && StructuredAddressModeEnabled() && !operand.GetRight().Null();
    const bool fold_host_base =
            HostBaseFoldEligible(mem_hostbase_fold,
                                 use_memory_base,
                                 guest_addr_mask,
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
                           structured_guest_ea);
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
    const bool check_alignment =
            scalar_fast_path && size > 1 && !static_address;
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
                           !scalar_fast_path && !q_access);

    // FEAT_LRCPC's LDAPR is the scalar x86-TSO fast path. It requires a bare,
    // naturally aligned address, so materialize any offset and branch around
    // it for x86's permitted unaligned accesses. Byte accesses are naturally
    // aligned by definition. Hosts without LRCPC and vector accesses retain
    // the proven plain-load + dmb ishld half-barrier.
    if (scalar_fast_path) {
        Register address = vixl_operand.GetBaseRegister();
        if (!vixl_operand.IsImmediateOffset() || vixl_operand.GetOffset() != 0) {
            __ ComputeAddress(mem_scratch, vixl_operand);
            address = mem_scratch;
        }

        Label unaligned;
        Label done;
        if (check_alignment) {
            __ Tst(address, size - 1);
            __ B(&unaligned, ne);
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

void JitTranslator::EmitStoreMemoryTSO(ir::Inst* inst) {
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
    const bool check_alignment =
            scalar_fast_path && size > 1 && !static_address;
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
                           !scalar_fast_path && !q_access);

    // Gate the complete scalar fast path with the same probe as LDAPR. This
    // keeps non-LRCPC hosts on the previous dmb+str implementation and makes
    // SVM_ARM64_LRCPC=0 an exact A/B baseline.
    if (scalar_fast_path) {
        Register address = vixl_operand.GetBaseRegister();
        if (!vixl_operand.IsImmediateOffset() || vixl_operand.GetOffset() != 0) {
            __ ComputeAddress(mem_scratch, vixl_operand);
            address = mem_scratch;
        }

        Label unaligned;
        Label done;
        if (check_alignment) {
            __ Tst(address, size - 1);
            __ B(&unaligned, ne);
        }
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
        if (size == 1) {
            return;
        }
        if (!check_alignment) {
            return;
        }
        __ B(&done);
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
        EmitGuestToHost(x0, x0);
        EmitGuestToHost(x1, x1);
    }
    __ Mov(x2, size);
    if (sse_afp_nan) {
        context.RecordFpcrTaxCounter(FpcrTaxCounter::MemoryCopy);
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
                ip1,
                [this](FpcrTaxCounter counter) {
                    context.RecordFpcrTaxCounter(counter);
                });
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

void JitTranslator::EmitMemoryCopyTSO(ir::Inst* inst) {
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
    EmitMemoryCopy(inst);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitMemoryBarrierTSO(ir::Inst* inst) {
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
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
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label cas_done;
    Label done;
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        AcquireUnalignedAtomicLock(result);
        EmitPlainAtomicLoad(type, result, address);
        __ Cmp(result, context.R(expected, true));
        __ B(&cas_done, ne);
        EmitPlainAtomicStore(type, context.R(desired, true), address);
        __ Bind(&cas_done);
        ReleaseUnalignedAtomicLock();
        __ B(&done);
    }

    __ Bind(&aligned);
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

void JitTranslator::EmitCompareAndSwap128(ir::Inst* inst) {
    // Args: (address, expected_lo, expected_hi, desired_lo, desired_hi);
    // returns the observed pair as V128, low half in lane 0.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto expected_lo = inst->GetArg<ir::Value>(1);
    const auto expected_hi = inst->GetArg<ir::Value>(2);
    const auto desired_lo = inst->GetArg<ir::Value>(3);
    const auto desired_hi = inst->GetArg<ir::Value>(4);
    const auto result = context.V(ir::Value{inst});

    MergeNZCV();
    if (use_memory_base) {
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
    // Rosetta), so preserve it with a serialized plain pair load/store.
    __ Tst(address, 15);
    __ B(&aligned, eq);
    AcquireUnalignedAtomicLock(atomic_pair_scratch);
    __ Ldp(atomic_scratch, atomic_pair_scratch, MemOperand(address));
    __ Cmp(atomic_scratch, context.X(expected_lo));
    __ B(&fallback_no_store, ne);
    __ Cmp(atomic_pair_scratch, context.X(expected_hi));
    __ B(&fallback_no_store, ne);
    __ Stp(context.X(desired_lo), context.X(desired_hi), MemOperand(address));
    __ Bind(&fallback_no_store);
    __ Ins(result.V2D(), 0, atomic_scratch);
    __ Ins(result.V2D(), 1, atomic_pair_scratch);
    ReleaseUnalignedAtomicLock();
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

void JitTranslator::EmitAtomicExchange(ir::Inst* inst) {
    // Args: (address, desired); returns the previous value. Memory XCHG is
    // implicitly locked on x86, so use an unconditional exclusive loop and
    // full barriers regardless of the configured ordinary-memory TSO mode.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto desired = inst->GetArg<ir::Value>(1);
    const auto type = desired.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV();
    if (use_memory_base) {
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
        AcquireUnalignedAtomicLock(result);
        EmitPlainAtomicLoad(type, result, address);
        EmitPlainAtomicStore(type, context.R(desired, true), address);
        ReleaseUnalignedAtomicLock();
        __ B(&done);
    }

    __ Bind(&aligned);
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

void JitTranslator::EmitAtomicFetchAdd(ir::Inst* inst) {
    // Args: (address, addend); returns the previous value. Used by LOCK XADD.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto addend = inst->GetArg<ir::Value>(1);
    const auto type = addend.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV();
    if (use_memory_base) {
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
        AcquireUnalignedAtomicLock(result);
        EmitPlainAtomicLoad(type, result, address);
        if (ir::GetValueSizeByte(type) == 8) {
            __ Add(atomic_scratch, result, context.X(addend));
        } else {
            __ Add(atomic_scratch.W(), result.W(), context.W(addend));
        }
        EmitPlainAtomicStore(type, atomic_scratch, address);
        ReleaseUnalignedAtomicLock();
        __ B(&done);
    }

    __ Bind(&aligned);
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

void JitTranslator::EmitAtomicRMW(ir::Inst* inst) {
    // Args: (operation, address, operand, carry); returns the previous value.
    const auto op =
            static_cast<ir::AtomicRMWOp>(inst->GetArg<ir::Imm>(0).Get());
    auto address = context.X(inst->GetArg<ir::Value>(1));
    const auto operand = inst->GetArg<ir::Value>(2);
    const auto carry = inst->GetArg<ir::Value>(3);
    const auto type = operand.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV();
    if (use_memory_base) {
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
        AcquireUnalignedAtomicLock(result);
        EmitPlainAtomicLoad(type, result, address);
        EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
        EmitPlainAtomicStore(type, atomic_scratch, address);
        ReleaseUnalignedAtomicLock();
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

void JitTranslator::EmitUniformBarrier(ir::Inst* inst) {
    // Compiler barrier only: uniform caching is not invalidated across this point.
}

}  // namespace swift::runtime::backend::arm64
