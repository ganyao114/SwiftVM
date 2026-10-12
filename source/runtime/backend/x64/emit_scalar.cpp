#include "translator.h"
#include "canonical_scalar_publication.h"
#include <stdexcept>
#include "helpers.h"
#include "runtime/common/div128.h"
#include "runtime/common/crc32.h"
#include "runtime/common/sse42str_result.h"
#include "runtime/frontend/x86/sse42str_helper.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitLoadImm(ir::Inst* inst) {
    if (sparse_frame_plan.RematerializedValue(inst)) {
        ++statistics.scalar_rematerialized_definition_sites;
        return;
    }
    mov(rax, Immediate(inst, 0)); Write(inst, rax); return;
}

void Translator::EmitZero(ir::Inst* inst) {
    pxor(xmm0, xmm0); WriteVector(inst, xmm0); return;
}

void Translator::EmitVecSharedZero(ir::Inst* inst) { EmitZero(inst); }

void Translator::EmitBitCast(ir::Inst* inst) {
    ReadVector(xmm0, inst->GetArg<ir::Value>(0)); WriteVector(inst, xmm0); return;
}

void Translator::EmitGetResult(ir::Inst* inst) { EmitBitCast(inst); }

void Translator::EmitGetOperand(ir::Inst* inst) {
    Operand(rax, inst->GetArg<ir::Operand>(0)); Write(inst, rax); return;
}

void Translator::EmitScalarBinary(ir::Inst* inst, ir::OpCode op) {
    const auto bits = ResultBits(inst);
    const auto rhs = inst->GetArg<ir::Operand>(1);
    std::optional<u32> encoded_immediate;
    bool rematerialized_immediate{};
    if (scalar_binary_immediates && (bits == 8 || bits == 16 || bits == 32 || bits == 64) &&
        rhs.GetRight().Null() && rhs.GetOp().shift_ext == 0 &&
        (rhs.GetOp().type == ir::OperandOp::None || rhs.GetOp().type == ir::OperandOp::Plus)) {
        const auto source = rhs.GetLeft();
        std::optional<u64> value;
        if (source.IsImm()) value = source.imm.Get();
        else if (source.IsValue() && source.value.Type() >= ir::ValueType::U8 &&
                 source.value.Type() <= ir::ValueType::S64) {
            value = sparse_frame_plan.RematerializedValue(source.value.Def());
            if (value) {
                // The frame representation has normalized the producer;
                // preserve Read's consumer-view truncation before ALU width.
                const u32 view_bits = ir::GetValueSizeByte(source.value.Type()) * 8;
                if (view_bits < 64) *value &= (u64{1} << view_bits) - 1;
                rematerialized_immediate = true;
            }
        }
        if (value) {
            if (op == O::AndNot) *value = ~*value;
            if (bits < 64) *value &= (u64{1} << bits) - 1;
            const u32 immediate = static_cast<u32>(*value);
            // x64's 64-bit ALU immediate is always sign-extended imm32.
            // Match the full bit pattern, including positive 0xffffffff.
            const u64 extended = (immediate & 0x80000000u) ?
                u64{immediate} | 0xffffffff00000000ULL : u64{immediate};
            if (bits < 64 || *value == extended) {
                // Xbyak's 16-bit range selection expects a sign-extended
                // pattern when bit 15 is set; hardware still takes low 16.
                encoded_immediate = bits == 16 && (immediate & 0x8000u) ?
                    immediate | 0xffff0000u : immediate;
            }
        }
    }
    // Preserve ReadArgument's existing fallback for non-SSA arguments.
    const auto lhs = inst->ArgAt(0).IsValue() ? inst->GetArg<ir::Value>(0) : ir::Value{};
    const auto* lhs_interval = scalar_register_plan.Find(lhs.Def());
    const auto* output_interval = scalar_register_plan.Find(inst);
    const bool binary_op = op == O::Add || op == O::Sub || op == O::Adc || op == O::Sbb ||
        op == O::And || op == O::Or || op == O::Xor || op == O::AndNot;
    const bool legacy_coalesced = binary_op && lhs_interval && output_interval &&
        lhs_interval->slot == output_interval->slot &&
        lhs_interval->last_use == output_interval->definition_position &&
        lhs_interval->last_use_phase == ScalarCachePhase::Read &&
        lhs.Type() >= ir::ValueType::U8 && lhs.Type() <= ir::ValueType::S64;
    const auto physical = AllocatedScalarRegister(inst);
    const bool cfg_coalesced = binary_op && cfg_register_plan.Enabled() && physical &&
        AllocatedScalarRegister(lhs.Def()) == physical &&
        cfg_register_plan.CanReuseBinaryInput(inst, lhs.Def());
    const bool coalesced = legacy_coalesced || cfg_coalesced;
    const Reg64 destination{coalesced ? static_cast<int>(*physical) : rax.getIdx()};
    if (coalesced) {
        // RHS may read this same SSA through wider or repeated views. Finish
        // every Operand read before truncating or overwriting its cache slot.
        if (!encoded_immediate) Operand(rdx, rhs);
        Mask(destination, ir::GetValueSizeByte(lhs.Type()) * 8);
    } else {
        ReadArgument(rax, inst, 0); if (!encoded_immediate) Operand(rdx, rhs);
    }
    if (bits > 64) throw std::runtime_error("x64 JIT scalar operation wider than 64 bits");
    if (op == O::AndNot && !encoded_immediate) not_(rdx);
    if (op == O::Adc || op == O::Sbb) {
        if (const auto* incoming = carry_chain_plan.IncomingFor(inst)) {
            // The preanalysis accepts only MOV/MOVZX reads/publication and
            // two full NZCV markers in one straight-line CFG region. The
            // first packed merge is deferred; its native CF remains intact.
            ++statistics.native_cf_forward_sites;
            if (flags_residency_active) ++statistics.native_cf_resident_bt_omissions;
            else ++statistics.native_cf_state_bt_omissions;
            if (incoming->complement) {
                cmc(); ++statistics.native_cf_polarity_cmc_sites;
            }
        } else {
            // Restore carry after all RHS/mask clobbers. The immediate bit
            // lies within this same flags word; no scratch copy is needed.
            if (flags_residency_active) {
                bt(rbx, 29);
                ++statistics.flags_resident_carry_read_sites;
            } else {
                bt(qword[r12 + state_offset_host_flags], 29);
                ++statistics.flags_state_carry_read_sites;
            }
            if (op == O::Sbb) cmc();
        }
    }
    const auto binary = [&](const auto& a, const auto& b) {
        switch (op) {
            case O::Add: add(a, b); break; case O::Sub: sub(a, b); break;
            case O::Adc: adc(a, b); break; case O::Sbb: sbb(a, b); break;
            case O::And: case O::AndNot: and_(a, b); break;
            case O::Or: or_(a, b); break; case O::Xor: xor_(a, b); break;
            default: break;
        }
    };
    if (encoded_immediate) {
        if (bits == 8) binary(destination.cvt8(), *encoded_immediate);
        else if (bits == 16) binary(destination.cvt16(), *encoded_immediate);
        else if (bits == 32) binary(destination.cvt32(), *encoded_immediate);
        else binary(destination, *encoded_immediate);
        ++statistics.scalar_binary_immediate_sites;
        if (rematerialized_immediate) ++statistics.scalar_binary_rematerialized_immediate_sites;
    } else {
        if (bits == 8) binary(destination.cvt8(), dl); else if (bits == 16) binary(destination.cvt16(), dx);
        else if (bits == 32) binary(destination.cvt32(), edx); else binary(destination, rdx);
    }
    if (flag_producers.contains(inst)) { pushfq(); pop(qword[r13 + Home(inst) + 16]); }
    Write(inst, destination);
    if (coalesced) ++statistics.scalar_coalesced_binary_sites;
    return;
}

void Translator::EmitAdd(ir::Inst* inst) { EmitScalarBinary(inst, O::Add); }

void Translator::EmitSub(ir::Inst* inst) { EmitScalarBinary(inst, O::Sub); }

void Translator::EmitAdc(ir::Inst* inst) { EmitScalarBinary(inst, O::Adc); }

void Translator::EmitSbb(ir::Inst* inst) { EmitScalarBinary(inst, O::Sbb); }

void Translator::EmitAnd(ir::Inst* inst) { EmitScalarBinary(inst, O::And); }

void Translator::EmitOr(ir::Inst* inst) { EmitScalarBinary(inst, O::Or); }

void Translator::EmitXor(ir::Inst* inst) { EmitScalarBinary(inst, O::Xor); }

void Translator::EmitAndNot(ir::Inst* inst) { EmitScalarBinary(inst, O::AndNot); }

Reg64 Translator::ReadUnaryInput(ir::Inst* inst) {
    // Only a full CFG proof can destroy an assigned input. Legacy cache
    // intervals, same register numbers alone and lexical last-use are not
    // sufficient across Phi edges, backedges or raw/opaque late reads.
    if (scalar_unary_coalescing && cfg_register_plan.ScalarUnaryPhasesEnabled() &&
        inst->ArgAt(0).IsValue()) {
        const auto source = inst->GetArg<ir::Value>(0);
        const auto output_register = cfg_register_plan.Register(inst);
        if (output_register && cfg_register_plan.Register(source.Def()) == output_register &&
            cfg_register_plan.CanReuseUnaryInput(inst, source.Def())) {
            const Reg64 destination{static_cast<int>(*output_register)};
            // Read normally truncates the consumer view, independently of
            // the canonical producer width. MOV/MOVZX preserve native flags.
            const auto producer_bytes = ir::GetValueSizeByte(source.Def()->ReturnType());
            const auto view_bytes = ir::GetValueSizeByte(source.Type());
            const bool scalar_view = source.Type() >= ir::ValueType::U8 &&
                                     source.Type() <= ir::ValueType::S64;
            const bool omit_mask = scalar_unary_mask_elision && scalar_view &&
                view_bytes < 8 && producer_bytes <= view_bytes &&
                !cfg_register_plan.RequiresFullContainer(source.Def()) &&
                HasCanonicalScalarPublication(source.Def());
            if (omit_mask) ++statistics.scalar_unary_mask_omissions;
            else Mask(destination, view_bytes * 8);
            ++statistics.scalar_coalesced_unary_sites;
            return destination;
        }
    }
    ReadArgument(rax, inst, 0);
    return rax;
}

void Translator::EmitNeg(ir::Inst* inst) {
    const auto bits = ResultBits(inst);
    const auto destination = ReadUnaryInput(inst);
    if (bits == 8) neg(destination.cvt8()); else if (bits == 16) neg(destination.cvt16());
    else if (bits == 32) neg(destination.cvt32()); else neg(destination);
    // Capture width-correct native flags before Write canonicalizes the result.
    if (flag_producers.contains(inst)) { pushfq(); pop(qword[r13 + Home(inst) + 16]); }
    Write(inst, destination); return;
}

void Translator::EmitNot(ir::Inst* inst) {
    if (inst->ArgAt(1).IsVoid()) { ReadArgument(rax, inst, 0); test(rax, rax); sete(al); movzx(eax, al); } else { Operand(rax, inst->GetArg<ir::Operand>(1)); not_(rax); } Write(inst, rax); return;
}

void Translator::EmitMul(ir::Inst* inst) {
    const auto bits = ResultBits(inst);
    ReadArgument(rax, inst, 0); Operand(rdx, inst->GetArg<ir::Operand>(1)); Mask(rdx, bits);
    const bool sign = ir::IsSignValueType(inst->GetArg<ir::Value>(0).Type());
    if (sign && bits < 64) { SignExtend(rax, bits); SignExtend(rdx, bits); }
    imul(rax, rdx);
    if (bits < 64 && flag_producers.contains(inst)) {
        mov(rcx, rax);
        if (sign) { Mask(rcx, bits); SignExtend(rcx, bits); cmp(rcx, rax); }
        else { shr(rcx, bits); test(rcx, rcx); }
        setne(cl); movzx(ecx, cl); mov(rdx, rcx); shl(rdx, 11); or_(rcx, rdx);
        mov(qword[r13 + Home(inst) + 16], rcx);
    }
    Write(inst, rax); return;
}

void Translator::EmitMulSub(ir::Inst* inst) {
    ReadArgument(rax, inst, 0); ReadArgument(rdx, inst, 1); imul(rax, rdx); ReadArgument(rdx, inst, 2); sub(rdx, rax); mov(rax, rdx); Write(inst, rax); return;
}

void Translator::EmitMulHigh(ir::Inst* inst) {
    ReadArgument(rax, inst, 0); ReadArgument(rcx, inst, 1); if (Immediate(inst, 2)) imul(rcx); else mul(rcx); mov(rax, rdx); Write(inst, rax); return;
}

void Translator::EmitCpuid(ir::Inst* inst) {
    const u64 target = Immediate(inst, 3);
    if (!target) throw std::runtime_error("x64 Cpuid requires a helper address");
    PublishLocation();
    ReadArgument(rdi, inst, 0); ReadArgument(rsi, inst, 1); mov(rdx, Immediate(inst, 2));
    Call(target);
    Write(inst, rax);
    mov(qword[r13 + Home(inst) + 8], rdx);
    PublishPseudoResults(inst, O::CpuidUpper, 8);
    return;
}

void Translator::EmitCpuidUpper(ir::Inst* inst) {
    const auto producer = inst->GetArg<ir::Value>(0).Def();
    if (!producer || producer->GetOp() != O::Cpuid)
        throw std::runtime_error("x64 CpuidUpper requires a Cpuid producer");
    return;
}

void Translator::EmitCrc32(ir::Inst* inst) {
    const u32 width = ValidateIeeeCrc32(inst);
    ReadArgument(rax, inst, 0); ReadArgument(rcx, inst, 1); mov(eax, eax);
    // SSE4.2 CRC32 uses Castagnoli, so IEEE always uses this table.
    mov(r8, reinterpret_cast<u64>(ieee_crc32_table.data()));
    for (u32 byte = 0; byte < width / 8; ++byte) {
        mov(edx, eax); xor_(edx, ecx); and_(edx, 255);
        shr(eax, 8); xor_(eax, dword[r8 + rdx * 4]);
        if (byte + 1 != width / 8) shr(rcx, 8);
    }
    Write(inst, rax); return;
}

void Translator::EmitCrc32c(ir::Inst* inst) {
    const u32 width = Immediate(inst, 2);
    if (width != 8 && width != 16 && width != 32 && width != 64)
        throw std::runtime_error("x64 CRC32C width must be 8,16,32,64");
    ReadArgument(rax, inst, 0); ReadArgument(rcx, inst, 1); mov(eax, eax);
    if (features.sse42) {
        if (width == 8) crc32(eax, cl);
        else if (width == 16) crc32(eax, cx);
        else if (width == 32) crc32(eax, ecx);
        else crc32(rax, rcx);
    } else {
        mov(r8, reinterpret_cast<u64>(Crc32cTable()));
        for (u32 byte = 0; byte < width / 8; ++byte) {
            mov(edx, eax); xor_(edx, ecx); and_(edx, 255);
            shr(eax, 8); xor_(eax, dword[r8 + rdx * 4]);
            if (byte + 1 != width / 8) shr(rcx, 8);
        }
    }
    Write(inst, rax); return;
}

void Translator::EmitDiv128(ir::Inst* inst) {
    ReadArgument(rdx, inst, 0); ReadArgument(rax, inst, 1); ReadArgument(rcx, inst, 2);
    Label zero_divisor, slow, done;
    test(rcx, rcx); jz(zero_divisor);
    const bool sign = Immediate(inst, 3) != 0;
    if (sign) {
        // A sign-extended 64-bit numerator can use IDIV directly,
        // except MIN64/-1, whose specified low quotient wraps.
        mov(r8, rax); sar(r8, 63); cmp(rdx, r8); jne(slow);
        Label ordinary;
        cmp(rcx, -1); jne(ordinary);
        mov(r8, u64{1} << 63); cmp(rax, r8); je(slow);
        L(ordinary); idiv(rcx);
    } else {
        // Unsigned DIV faults when the 128-bit quotient exceeds 64
        // bits. The IR instead retains its low quotient bits.
        cmp(rdx, rcx); jae(slow); div(rcx);
    }
    jmp(done);
    L(slow);
    mov(rdi, rdx); mov(rsi, rax); mov(rdx, rcx);
    Call(reinterpret_cast<u64>(sign ? &DivideSigned128 : &DivideUnsigned128));
    jmp(done);
    L(zero_divisor); xor_(eax, eax); xor_(edx, edx);
    L(done);
    // Secondary pseudos are producer-time definitions. Their homes
    // are reserved from here through all later consumers/markers.
    Write(inst, rax);
    mov(qword[r13 + Home(inst) + 8], rdx);
    PublishPseudoResults(inst, O::Div128Remainder, 8);
    return;
}

void Translator::EmitDiv128Remainder(ir::Inst* inst) {
    const auto producer = inst->GetArg<ir::Value>(0).Def();
    if (!producer || producer->GetOp() != O::Div128)
        throw std::runtime_error("x64 Div128Remainder requires a Div128 producer");
    return;
}

void Translator::EmitDivide(ir::Inst* inst, ir::OpCode op) {
    ReadArgument(rdi, inst, 0);
    if (op == O::Div) Operand(rsi, inst->GetArg<ir::Operand>(1)); else ReadArgument(rsi, inst, 1);
    const auto source = inst->GetArg<ir::Value>(0);
    const auto width = ir::GetValueSizeByte(source.Type()) * 8;
    Mask(rsi, width);
    if (op == O::SignedDiv64 || ir::IsSignValueType(source.Type())) {
        mov(edx, width); Call(reinterpret_cast<u64>(&SignedDivide));
    } else Call(reinterpret_cast<u64>(&UnsignedDivide));
    Write(inst, rax); return;
}

void Translator::EmitDiv(ir::Inst* inst) { EmitDivide(inst, O::Div); }

void Translator::EmitSignedDiv64(ir::Inst* inst) { EmitDivide(inst, O::SignedDiv64); }

void Translator::EmitZeroExtend32(ir::Inst* inst) {
    const auto destination = ReadUnaryInput(inst);
    mov(destination.cvt32(), destination.cvt32()); Write(inst, destination); return;
}

void Translator::EmitZeroExtend32To64(ir::Inst* inst) { EmitZeroExtend32(inst); }

void Translator::EmitZeroExtend64(ir::Inst* inst) {
    const auto destination = ReadUnaryInput(inst); Write(inst, destination); return;
}

void Translator::EmitSignExtend(ir::Inst* inst) {
    const auto destination = ReadUnaryInput(inst);
    SignExtend(destination, ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) * 8);
    Write(inst, destination); return;
}

void Translator::EmitScalarShift(ir::Inst* inst, ir::OpCode op) {
    const auto bits = ResultBits(inst);
    // Only immediate shifts are in the unary phase inventory. Variable
    // shifts therefore retain the original scratch path and capture order.
    const auto destination = ReadUnaryInput(inst);
    const bool fixed = op == O::LslImm || op == O::LsrImm || op == O::AsrImm || op == O::RorImm;
    if (scalar_shift_immediates && fixed && (bits == 8 || bits == 16 || bits == 32 || bits == 64)) {
        // Match the original CL path's declared-result-width modulo before
        // using the hardware immediate. Keep its 64-bit SHL/SHR/SAR semantics
        // when an input view and the result extent differ.
        const auto count = static_cast<u8>(Immediate(inst, 1) & (bits - 1));
        if (op == O::LslImm || op == O::LsrImm) Mask(destination, bits);
        if (op == O::AsrImm)
            SignExtend(destination, ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) * 8);
        if (count) {
            if (op == O::LslImm) shl(destination, count);
            else if (op == O::LsrImm) shr(destination, count);
            else if (op == O::AsrImm) sar(destination, count);
            else if (bits == 32) ror(destination.cvt32(), count);
            else if (bits == 16) ror(destination.cvt16(), count);
            else if (bits == 8) ror(destination.cvt8(), count);
            else ror(destination, count);
        } else ++statistics.scalar_shift_zero_sites;
        ++statistics.scalar_shift_immediate_sites;
        Write(inst, destination); return;
    }
    if (op == O::LslImm || op == O::LsrImm || op == O::AsrImm || op == O::RorImm) mov(ecx, Immediate(inst, 1)); else ReadArgument(rcx, inst, 1);
    and_(ecx, bits - 1);
    if (op == O::LslImm || op == O::LslValue || op == O::LsrImm || op == O::LsrValue) Mask(destination, bits);
    if (op == O::AsrImm || op == O::AsrValue) SignExtend(destination, ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) * 8);
    if (op == O::LslImm || op == O::LslValue) shl(destination, cl);
    else if (op == O::LsrImm || op == O::LsrValue) shr(destination, cl);
    else if (op == O::AsrImm || op == O::AsrValue) sar(destination, cl);
    else if (bits == 32) ror(destination.cvt32(), cl); else if (bits == 16) ror(destination.cvt16(), cl); else if (bits == 8) ror(destination.cvt8(), cl); else ror(destination, cl);
    Write(inst, destination); return;
}

void Translator::EmitLslImm(ir::Inst* inst) { EmitScalarShift(inst, O::LslImm); }

void Translator::EmitLslValue(ir::Inst* inst) { EmitScalarShift(inst, O::LslValue); }

void Translator::EmitLsrImm(ir::Inst* inst) { EmitScalarShift(inst, O::LsrImm); }

void Translator::EmitLsrValue(ir::Inst* inst) { EmitScalarShift(inst, O::LsrValue); }

void Translator::EmitAsrImm(ir::Inst* inst) { EmitScalarShift(inst, O::AsrImm); }

void Translator::EmitAsrValue(ir::Inst* inst) { EmitScalarShift(inst, O::AsrValue); }

void Translator::EmitRorImm(ir::Inst* inst) { EmitScalarShift(inst, O::RorImm); }

void Translator::EmitRorValue(ir::Inst* inst) { EmitScalarShift(inst, O::RorValue); }

void Translator::EmitByteSwap(ir::Inst* inst) {
    const auto destination = ReadUnaryInput(inst);
    if (Immediate(inst, 1) == 16) ror(destination.cvt16(), 8);
    else if (Immediate(inst, 1) == 32) bswap(destination.cvt32()); else bswap(destination);
    Write(inst, destination); return;
}

void Translator::EmitCountZeros(ir::Inst* inst, ir::OpCode op) {
    ReadArgument(rax, inst, 0);
    const auto width = (op == O::CountLeadingZeros32 || op == O::CountTrailingZeros32) ? 32 : 64;
    Mask(rax, width);
    Label zero, done; test(rax, rax); jz(zero);
    if (op == O::CountLeadingZeros32 || op == O::CountLeadingZeros64) { bsr(rax, rax); xor_(rax, width - 1); }
    else bsf(rax, rax);
    jmp(done); L(zero); mov(eax, width); L(done); Write(inst, rax); return;
}

void Translator::EmitCountLeadingZeros32(ir::Inst* inst) { EmitCountZeros(inst, O::CountLeadingZeros32); }

void Translator::EmitCountLeadingZeros64(ir::Inst* inst) { EmitCountZeros(inst, O::CountLeadingZeros64); }

void Translator::EmitCountTrailingZeros32(ir::Inst* inst) { EmitCountZeros(inst, O::CountTrailingZeros32); }

void Translator::EmitCountTrailingZeros64(ir::Inst* inst) { EmitCountZeros(inst, O::CountTrailingZeros64); }

void Translator::EmitBitExtract(ir::Inst* inst) {
    const auto destination = ReadUnaryInput(inst);
    if (const auto lsb = Immediate(inst, 1)) shr(destination, lsb);
    const auto width = Immediate(inst, 2);
    if (width < 64 && width < ResultBits(inst)) {
        if (width == 8 || width == 16 || width == 32) Mask(destination, width);
        else { mov(rdx, (u64{1} << width) - 1); and_(destination, rdx); }
    }
    // Write already canonicalizes the declared result extent. Full-width
    // extraction and extracts at that extent need no additional mask.
    Write(inst, destination);
}

void Translator::EmitBitUpdate(ir::Inst* inst, ir::OpCode op) {
    const auto lsb = Immediate(inst, op == O::BitInsert ? 2 : 1), width = Immediate(inst, op == O::BitInsert ? 3 : 2);
    const auto mask = (width >= 64 ? UINT64_MAX : (u64{1} << width) - 1) << lsb;
    // BitInsert is excluded from unary phases, so its second read still
    // observes the original inputs before any assigned output is overwritten.
    const auto destination = ReadUnaryInput(inst); mov(rdx, ~mask); and_(destination, rdx);
    if (op == O::BitInsert) { ReadArgument(rcx, inst, 1); shl(rcx, lsb); mov(rdx, mask); and_(rcx, rdx); or_(destination, rcx); }
    Write(inst, destination); return;
}

void Translator::EmitBitInsert(ir::Inst* inst) { EmitBitUpdate(inst, O::BitInsert); }

void Translator::EmitBitClear(ir::Inst* inst) { EmitBitUpdate(inst, O::BitClear); }

void Translator::EmitTestBit(ir::Inst* inst) {
    const auto destination = ReadUnaryInput(inst);
    shr(destination, Immediate(inst, 1)); and_(destination.cvt32(), 1); Write(inst, destination); return;
}

void Translator::EmitZeroTest(ir::Inst* inst, ir::OpCode op) {
    const auto destination = ReadUnaryInput(inst); test(destination, destination);
    if (op == O::TestZero) sete(destination.cvt8()); else setne(destination.cvt8());
    movzx(destination.cvt32(), destination.cvt8()); Write(inst, destination); return;
}

void Translator::EmitTestZero(ir::Inst* inst) { EmitZeroTest(inst, O::TestZero); }

void Translator::EmitTestNotZero(ir::Inst* inst) { EmitZeroTest(inst, O::TestNotZero); }

void Translator::EmitSse42Str(ir::Inst* inst) {
    const u8 control = u8(Immediate(inst, 2));
    if (features.sse42) {
        // PCMPISTRM always produces compact IntRes2 here. Bit 6 only
        // selects the later index direction; bit 7 is ignored by IR.
        ReadVector(xmm1, inst->GetArg<ir::Value>(0));
        ReadVector(xmm3, inst->GetArg<ir::Value>(1));
        pcmpistrm(xmm1, xmm3, control & 0x3f);
        sets(dl); setz(sil);
        movd(eax, xmm0); movzx(edx, dl); movzx(esi, sil);
        shl(edx, sse42str::kSignBit); shl(esi, sse42str::kZeroBit);
        or_(eax, edx); or_(eax, esi);
        mov(ecx, eax); and_(ecx, 0xffff);
        mov(r8d, control & 1 ? 8 : 16);
        Label no_match;
        test(ecx, ecx); jz(no_match);
        if (control & 0x40) bsr(r8d, ecx); else bsf(r8d, ecx);
        or_(eax, 1u << sse42str::kCarryBit);
        L(no_match);
        shl(r8d, sse42str::kIndexShift); or_(eax, r8d);
        and_(ecx, 1); shl(ecx, sse42str::kOverflowBit); or_(eax, ecx);
    } else {
        PublishLocation();
        ReadVector(xmm1, inst->GetArg<ir::Value>(0));
        movq(rdi, xmm1); psrldq(xmm1, 8); movq(rsi, xmm1);
        ReadVector(xmm3, inst->GetArg<ir::Value>(1));
        movq(rdx, xmm3); psrldq(xmm3, 8); movq(rcx, xmm3);
        mov(r8d, control);
        Call(reinterpret_cast<u64>(&swift::x86::SwiftSse42StrEvalImplicit));
    }
    Write(inst, rax); return;
}

void Translator::EmitPopCount(ir::Inst* inst) {
    ReadArgument(rdi, inst, 0); Call(reinterpret_cast<u64>(&CountBits)); Write(inst, rax); return;
}

}  // namespace swift::runtime::backend::x64
