#include "translator.h"
#include <stdexcept>
#include "helpers.h"
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

void Translator::EmitMemoryCopyOrdered(ir::Inst* inst, ir::OpCode op) {
    // This path addresses through RSI/RDI and may use REP MOVSB. It cannot
    // satisfy the precise single-instruction R14 recovery contract yet.
    MarkUntrackedGuestAccess();
    const u64 size = Immediate(inst, 2);
    const bool tso = op == O::MemoryCopyTSO;
    if (tso) mfence();
    if (!size) { if (tso) mfence(); return; }
    PublishLocation();
    const auto address = [&](u32 index, const Reg64& target) {
        const auto lambda = inst->GetArg<ir::Lambda>(index);
        if (lambda.IsValue()) Read(target, lambda.GetValue()); else mov(target, lambda.GetImm().Get());
        and_(target, qword[r12 + offsetof(State, guest_addr_mask)]);
        mov(qword[r13 + Home(inst) + index * 8], target);
    };
    address(0, rax); address(1, rcx);
    // Validate both complete guest ranges before admitting one reader
    // or mutating memory. A second lease would not fit State's single
    // ownership bit and could leak on an abandoned copy.
    for (u32 index : {0u, 1u}) {
        mov(rdi, r12); mov(rsi, qword[r13 + Home(inst) + index * 8]); mov(rdx, size);
        Call(reinterpret_cast<u64>(&CheckMemory)); test(eax, eax); jz(epilogue, T_NEAR);
    }
    mov(rdi, r12); Call(reinterpret_cast<u64>(&BeginMemory));
    mov(rdi, qword[r13 + Home(inst)]); mov(rsi, qword[r13 + Home(inst) + 8]);
    add(rdi, qword[r12 + state_offset_pt]); add(rsi, qword[r12 + state_offset_pt]);
    if (size == 16) {
        movdqu(xmm0, ptr[rsi]); movdqu(ptr[rdi], xmm0);
    } else if (size < 16) {
        // Read the whole source before writing. Overlapping first/last
        // chunks stay within the validated range, including odd sizes.
        const u32 chunk = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
        const auto load = [&](const Reg64& target, u32 offset) {
            if (chunk == 8) mov(target, qword[rsi + offset]);
            else if (chunk == 4) mov(target.cvt32(), dword[rsi + offset]);
            else if (chunk == 2) movzx(target.cvt32(), word[rsi + offset]);
            else movzx(target.cvt32(), byte[rsi + offset]);
        };
        const auto store = [&](const Reg64& value, u32 offset) {
            if (chunk == 8) mov(qword[rdi + offset], value);
            else if (chunk == 4) mov(dword[rdi + offset], value.cvt32());
            else if (chunk == 2) mov(word[rdi + offset], value.cvt16());
            else mov(byte[rdi + offset], value.cvt8());
        };
        load(r8, 0); if (size != chunk) load(r9, size - chunk);
        store(r8, 0); if (size != chunk) store(r9, size - chunk);
    } else {
        Label forward, done;
        mov(rcx, size); cmp(rdi, rsi); jbe(forward);
        mov(r8, rdi); sub(r8, rsi); cmp(r8, rcx); jae(forward);
        // Direction is chosen from masked, biased addresses. Keep
        // accesses inside JIT code so Runtime can recover their PC.
        lea(rdi, ptr[rdi + rcx - 1]); lea(rsi, ptr[rsi + rcx - 1]);
        std(); rep(); movsb(); cld(); jmp(done);
        L(forward); cld(); rep(); movsb(); L(done);
    }
    EndMemoryAccess();
    if (tso) mfence();
    return;
}

void Translator::EmitMemoryCopy(ir::Inst* inst) { EmitMemoryCopyOrdered(inst, O::MemoryCopy); }

void Translator::EmitMemoryCopyTSO(ir::Inst* inst) { EmitMemoryCopyOrdered(inst, O::MemoryCopyTSO); }

void Translator::EmitGetUniformAddress(ir::Inst* inst) {
    lea(rax, ptr[r12 + state_offset_uniform_buffer + Immediate(inst, 0)]); Write(inst, rax); return;
}

void Translator::EmitLoadUniform(ir::Inst* inst) {
    const auto uniform = inst->GetArg<ir::Uniform>(0);
    const auto offset = state_offset_uniform_buffer + uniform.GetOffset();
    const auto size = ir::GetValueSizeByte(uniform.GetType());
    if (size == 16) { movdqu(xmm0, ptr[r12 + offset]); WriteVector(inst, xmm0); return; }
    if (size == 8) mov(rax, qword[r12 + offset]);
    else if (size == 4) mov(eax, dword[r12 + offset]);
    else if (size == 2) movzx(eax, word[r12 + offset]);
    else if (size == 1) movzx(eax, byte[r12 + offset]);
    else throw std::runtime_error("x64 JIT unsupported uniform width");
    Write(inst, rax); return;
}

void Translator::EmitStoreUniform(ir::Inst* inst) {
    const auto uniform = inst->GetArg<ir::Uniform>(0);
    const auto offset = state_offset_uniform_buffer + uniform.GetOffset();
    const auto value = inst->GetArg<ir::Value>(1);
    if (value.Type() == ir::ValueType::VOID)
        throw std::runtime_error("x64 JIT uniform store has no defined input view");
    // StoreUniform follows its value view, matching ARM64, RV64,
    // the interpreter and uniform byte-range analyses.
    const auto size = ir::GetValueSizeByte(value.Type());
    if (size > 16) throw std::runtime_error("x64 JIT unsupported uniform store width");
    if (size == 16) { ReadVector(xmm0, value); movdqu(ptr[r12 + offset], xmm0); return; }
    ReadArgument(rax, inst, 1);
    if (size == 8) mov(qword[r12 + offset], rax);
    else if (size == 4) mov(dword[r12 + offset], eax);
    else if (size == 2) mov(word[r12 + offset], ax);
    else if (size == 1) mov(byte[r12 + offset], al);
    else throw std::runtime_error("x64 JIT unsupported uniform width");
    return;
}

void Translator::EmitDefineLocal(ir::Inst* inst) {
    pxor(xmm0, xmm0); movdqu(ptr[r13 + locals.at(inst->GetArg<ir::Local>(0).id)], xmm0); return;
}

void Translator::EmitLoadLocal(ir::Inst* inst) {
    movdqu(xmm0, ptr[r13 + locals.at(inst->GetArg<ir::Local>(0).id)]); WriteVector(inst, xmm0); return;
}

void Translator::EmitStoreLocal(ir::Inst* inst) {
    ReadVector(xmm0, inst->GetArg<ir::Value>(1)); movdqu(ptr[r13 + locals.at(inst->GetArg<ir::Local>(0).id)], xmm0); return;
}

void Translator::EmitLoadMemory(ir::Inst* inst) {
    const auto bits = ResultBits(inst);
    MemoryAddress(inst, bits / 8);
    const auto begin = getSize();
    if (bits == 128) {
        movdqu(xmm0, ptr[r14]); RecordGuestAccess(begin, 16, GuestAccessKind::Read);
        WriteVector(inst, xmm0); EndMemoryAccess(); return;
    }
    if (bits == 64) mov(rax, qword[r14]);
    else if (bits == 32) mov(eax, dword[r14]);
    else if (bits == 16) movzx(eax, word[r14]);
    else if (bits == 8) movzx(eax, byte[r14]);
    else throw std::runtime_error("x64 JIT unsupported load width");
    RecordGuestAccess(begin, bits / 8, GuestAccessKind::Read);
    Write(inst, rax); EndMemoryAccess(); return;
}

void Translator::EmitLoadMemoryTSO(ir::Inst* inst) { EmitLoadMemory(inst); }

void Translator::EmitMemoryStore(ir::Inst* inst, ir::OpCode op) {
    const auto value = inst->GetArg<ir::Value>(1);
    const auto size = ir::GetValueSizeByte(value.Type());
    MemoryAddress(inst, size);
    if (size == 16) {
        ReadVector(xmm0, value);
        const auto begin = getSize();
        movdqu(ptr[r14], xmm0); RecordGuestAccess(begin, 16, GuestAccessKind::Write);
    }
    else {
        Read(rax, value);
        const auto begin = getSize();
        if (size == 8) mov(qword[r14], rax);
        else if (size == 4) mov(dword[r14], eax);
        else if (size == 2) mov(word[r14], ax);
        else if (size == 1) mov(byte[r14], al);
        else throw std::runtime_error("x64 JIT unsupported store width");
        RecordGuestAccess(begin, size, GuestAccessKind::Write);
    }
    if (op == O::StoreMemoryTSO) mfence();
    EndMemoryAccess();
    return;
}

void Translator::EmitStoreMemory(ir::Inst* inst) { EmitMemoryStore(inst, O::StoreMemory); }

void Translator::EmitStoreMemoryTSO(ir::Inst* inst) { EmitMemoryStore(inst, O::StoreMemoryTSO); }

void Translator::EmitMemoryBarrierTSO(ir::Inst* inst) {
    mfence(); return;
}

void Translator::EmitCompareAndSwap128(ir::Inst* inst) {
    // The C++ unaligned-atomic fallback owns both guest reads and writes.
    // Its host instructions are outside the precise generated R14 sites.
    MarkUntrackedGuestAccess();
    PublishLocation();
    ReadArgument(rax, inst, 0); and_(rax, qword[r12 + offsetof(State, guest_addr_mask)]);
    mov(qword[r13 + Home(inst) + 24], rax);
    mov(rdi, r12); mov(rsi, rax); mov(edx, 16);
    Call(reinterpret_cast<u64>(&CheckMemory)); test(eax, eax); jz(epilogue, T_NEAR);
    mov(rdi, qword[r13 + Home(inst) + 24]); add(rdi, qword[r12 + state_offset_pt]);
    ReadArgument(rsi, inst, 1); ReadArgument(rdx, inst, 2); ReadArgument(rcx, inst, 3); ReadArgument(r8, inst, 4);
    Call(reinterpret_cast<u64>(&AtomicCompareExchange128));
    // Only publish the observed pair after the complete helper returns.
    // On either read or write fault the original uniform state survives.
    mov(qword[r13 + Home(inst)], rax); mov(qword[r13 + Home(inst) + 8], rdx);
    return;
}

void Translator::EmitAtomic(ir::Inst* inst, ir::OpCode op) {
    const auto address = inst->GetArg<ir::Value>(op == O::AtomicRMW ? 1 : 0);
    const auto operand = inst->GetArg<ir::Value>(op == O::AtomicRMW ? 2 : 1);
    const auto width = ir::GetValueSizeByte(operand.Type()) * 8;
    if (width != 8 && width != 16 && width != 32 && width != 64)
        throw std::runtime_error("x64 atomic width must be 8,16,32,64");
    MemoryAddressValue(inst, address, width / 8);
    Read(rax, operand);
    if (op == O::CompareAndSwap) {
        Read(rdx, inst->GetArg<ir::Value>(2));
        const auto begin = getSize();
        lock();
        if (width == 8) cmpxchg(byte[r14], dl); else if (width == 16) cmpxchg(word[r14], dx);
        else if (width == 32) cmpxchg(dword[r14], edx); else cmpxchg(qword[r14], rdx);
        RecordGuestAccess(begin, width / 8, GuestAccessKind::ReadWrite);
    } else if (op == O::AtomicExchange) {
        const auto begin = getSize();
        if (width == 8) xchg(byte[r14], al); else if (width == 16) xchg(word[r14], ax);
        else if (width == 32) xchg(dword[r14], eax); else xchg(qword[r14], rax);
        RecordGuestAccess(begin, width / 8, GuestAccessKind::ReadWrite);
    } else if (op == O::AtomicFetchAdd) {
        const auto begin = getSize();
        lock();
        if (width == 8) xadd(byte[r14], al); else if (width == 16) xadd(word[r14], ax);
        else if (width == 32) xadd(dword[r14], eax); else xadd(qword[r14], rax);
        RecordGuestAccess(begin, width / 8, GuestAccessKind::ReadWrite);
    } else {
        const auto kind = static_cast<ir::AtomicRMWOp>(Immediate(inst, 0));
        Read(r8, operand); ReadArgument(r9, inst, 3); and_(r9d, 1);
        const auto load_begin = getSize();
        if (width == 8) movzx(eax, byte[r14]); else if (width == 16) movzx(eax, word[r14]);
        else if (width == 32) mov(eax, dword[r14]); else mov(rax, qword[r14]);
        RecordGuestAccess(load_begin, width / 8, GuestAccessKind::Read);
        Label retry; L(retry); mov(rdx, rax);
        switch (kind) {
            case ir::AtomicRMWOp::Add: add(rdx, r8); break;
            case ir::AtomicRMWOp::Sub: sub(rdx, r8); break;
            case ir::AtomicRMWOp::And: and_(rdx, r8); break;
            case ir::AtomicRMWOp::Or: or_(rdx, r8); break;
            case ir::AtomicRMWOp::Xor: xor_(rdx, r8); break;
            case ir::AtomicRMWOp::Neg: neg(rdx); break;
            case ir::AtomicRMWOp::AddCarry: add(rdx, r8); add(rdx, r9); break;
            case ir::AtomicRMWOp::SubBorrow: sub(rdx, r8); sub(rdx, r9); break;
            case ir::AtomicRMWOp::SignedMax:
            case ir::AtomicRMWOp::SignedMin:
            case ir::AtomicRMWOp::UnsignedMax:
            case ir::AtomicRMWOp::UnsignedMin: {
                const bool signed_compare = kind == ir::AtomicRMWOp::SignedMax || kind == ir::AtomicRMWOp::SignedMin;
                const bool maximum = kind == ir::AtomicRMWOp::SignedMax || kind == ir::AtomicRMWOp::UnsignedMax;
                mov(r10, r8);
                if (signed_compare) {
                    SignExtend(rdx, width);
                    SignExtend(r10, width);
                }
                cmp(rdx, r10);
                if (signed_compare) {
                    if (maximum) cmovl(rdx, r8); else cmovg(rdx, r8);
                } else {
                    if (maximum) cmovb(rdx, r8); else cmova(rdx, r8);
                }
                break;
            }
            default: throw std::runtime_error("x64 unsupported atomic RMW operation");
        }
        const auto store_begin = getSize();
        lock();
        if (width == 8) cmpxchg(byte[r14], dl); else if (width == 16) cmpxchg(word[r14], dx);
        else if (width == 32) cmpxchg(dword[r14], edx); else cmpxchg(qword[r14], rdx);
        RecordGuestAccess(store_begin, width / 8, GuestAccessKind::ReadWrite);
        jne(retry, T_NEAR);
    }
    Write(inst, rax); mov(rdi, r12); Call(reinterpret_cast<u64>(&EndMemory)); return;
}

void Translator::EmitCompareAndSwap(ir::Inst* inst) { EmitAtomic(inst, O::CompareAndSwap); }

void Translator::EmitAtomicExchange(ir::Inst* inst) { EmitAtomic(inst, O::AtomicExchange); }

void Translator::EmitAtomicFetchAdd(ir::Inst* inst) { EmitAtomic(inst, O::AtomicFetchAdd); }

void Translator::EmitAtomicRMW(ir::Inst* inst) { EmitAtomic(inst, O::AtomicRMW); }

void Translator::EmitCheckMemoryAlignment(ir::Inst* inst) {
    PublishLocation(); ReadArgument(rax, inst, 0); mov(rdx, Immediate(inst, 1)); test(rax, rdx); {
    Label aligned; jz(aligned); Return(HaltReason::PageFatal); L(aligned);
            } return;
}

void Translator::EmitHostUniform(ir::Inst* inst, bool get) {
    const auto& desc = HostBinding(inst);
    const auto slice = inst->GetArg<ir::Imm>(get ? 1 : 2).Get();
    const auto offset = state_offset_uniform_buffer + desc.offset + u32(slice);
    const auto type = get ? inst->ReturnType() : inst->GetArg<ir::Value>(0).Type();
    const auto size = ir::GetValueSizeByte(type);
    if (get) {
        if (size == 16) { movdqu(xmm0, ptr[r12 + offset]); WriteVector(inst, xmm0); }
        else {
            if (size == 8) mov(rax, qword[r12 + offset]);
            else if (size == 4) mov(eax, dword[r12 + offset]);
            else if (size == 2) movzx(eax, word[r12 + offset]);
            else movzx(eax, byte[r12 + offset]);
            Write(inst, rax);
        }
    } else if (size == 16) {
        ReadVector(xmm0, inst->GetArg<ir::Value>(0)); movdqu(ptr[r12 + offset], xmm0);
    } else {
        Read(rax, inst->GetArg<ir::Value>(0));
        // Fixed architectural GPR homes obey W-register zero extension.
        // Other slices preserve every byte outside the addressed range.
        const bool clear_upper = !desc.is_float && desc.size == 8 && slice == 0 && size == 4 && IsFixedGPRHome(desc.reg);
        if (size == 8 || clear_upper) mov(qword[r12 + offset], rax);
        else if (size == 4) mov(dword[r12 + offset], eax);
        else if (size == 2) mov(word[r12 + offset], ax);
        else mov(byte[r12 + offset], al);
    }
}

void Translator::EmitGetHostGPR(ir::Inst* inst) { EmitHostUniform(inst, true); }

void Translator::EmitGetHostFPR(ir::Inst* inst) { EmitHostUniform(inst, true); }

void Translator::EmitSetHostGPR(ir::Inst* inst) { EmitHostUniform(inst, false); }

void Translator::EmitSetHostFPR(ir::Inst* inst) { EmitHostUniform(inst, false); }

}  // namespace swift::runtime::backend::x64
