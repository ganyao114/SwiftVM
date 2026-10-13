#include "translator.h"
#include "helpers.h"
#include "fault_scope.h"

#include <limits>
#include <thread>
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/riscv64/memory_protocol.h"

namespace swift::runtime::backend::loongarch64 {
void AcquireMemory(State* current) noexcept {
    ClearGuestFaultScope(current);
    if (current->spill_area[kRiscvMemoryOwnedSlot]) return;
    auto* participant = reinterpret_cast<riscv64::MemoryParticipant*>(current->spill_area[kRiscvMemoryParticipantSlot]);
    for (;;) {
        if (participant) participant->active.store(1, std::memory_order_seq_cst);
        else riscv64::anonymous_memory_readers.fetch_add(1, std::memory_order_seq_cst);
        if (!unaligned_atomic_lock.load(std::memory_order_seq_cst)) break;
        if (participant) participant->active.store(0, std::memory_order_seq_cst);
        else riscv64::anonymous_memory_readers.fetch_sub(1, std::memory_order_seq_cst);
        while (unaligned_atomic_lock.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    current->spill_area[kRiscvMemoryOwnedSlot] = 1;
}

void ReleaseMemory(State* current) noexcept {
    ClearGuestFaultScope(current);
    if (!current->spill_area[kRiscvMemoryOwnedSlot]) return;
    auto* participant = reinterpret_cast<riscv64::MemoryParticipant*>(current->spill_area[kRiscvMemoryParticipantSlot]);
    if (participant) participant->active.store(0, std::memory_order_release);
    else riscv64::anonymous_memory_readers.fetch_sub(1, std::memory_order_seq_cst);
    current->spill_area[kRiscvMemoryOwnedSlot] = 0;
}

u64 GuestAddress(State* current, u64 guest, u64 size) noexcept {
    ClearGuestFaultScope(current);
    guest &= current->guest_addr_mask;
    try {
        if (!size || guest >= current->guest_addr_limit || size > current->guest_addr_limit - guest ||
            size - 1 > current->guest_addr_mask - guest ||
            reinterpret_cast<u64>(current->pt) > UINT64_MAX - guest ||
            (current->interp_range_check &&
             !current->interp_range_check(current->interp_range_check_ctx, guest, size))) {
            current->halt_reason = HaltReason::PageFatal; return 0;
        }
        if (current->halt_reason != HaltReason::None) return 0;
        // A range oracle is external code and may change State. Validate the
        // complete translated host extent after it returns, while inactive.
        const auto bias = reinterpret_cast<u64>(current->pt);
        if (guest >= current->guest_addr_limit || size > current->guest_addr_limit - guest ||
            guest > current->guest_addr_mask || size - 1 > current->guest_addr_mask - guest ||
            bias > UINT64_MAX - guest || size - 1 > UINT64_MAX - (bias + guest)) {
            current->halt_reason = HaltReason::PageFatal; return 0;
        }
        const auto host = bias + guest;
        SetGuestFaultScope(current, {host, size});
        return host;
    } catch (...) { current->halt_reason = HaltReason::IllegalCode; return 0; }
}

void JitTranslator::EmitLoadUniform(ir::Inst* inst) {
    const auto uniform = inst->GetArg<ir::Uniform>(0);
    const u32 size = ir::GetValueSizeByte(inst->ReturnType());
    if (!size || size > 16) { EmitHelper(inst); return; }
    context.RecordUniformLoad((size + 7) / 8);
    context.Load(t0, state, state_offset_uniform_buffer + uniform.GetOffset(), std::min(size, u32{8}));
    context.Write(inst, t0);
    if (size > 8) {
        context.Load(t0, state, state_offset_uniform_buffer + uniform.GetOffset() + 8);
        context.WritePart(inst, t0, 1);
    }
}

void JitTranslator::EmitStoreUniform(ir::Inst* inst) {
    const auto uniform = inst->GetArg<ir::Uniform>(0);
    const auto value = inst->GetArg<ir::Value>(1);
    const u32 size = ir::GetValueSizeByte(value.Type());
    if (!size || size > 16) { EmitHelper(inst); return; }
    context.RecordUniformStore((size + 7) / 8);
    if (vectors.StoreUniform(inst)) return;
    context.Read(t0, value);
    context.Store(t0, state, state_offset_uniform_buffer + uniform.GetOffset(), std::min(size, u32{8}));
    if (size > 8) {
        context.ReadPart(t0, value, 1);
        context.Store(t0, state, state_offset_uniform_buffer + uniform.GetOffset() + 8);
    }
}

void JitTranslator::EmitGetUniformAddress(ir::Inst* inst) {
    context.Address(t0, state, state_offset_uniform_buffer + inst->GetArg<ir::Imm>(0).Get());
    context.Write(inst, t0);
}

void JitTranslator::EmitMemory(ir::Inst* inst, bool store, bool ordered) {
    auto& as = context.GetMasm();
    const auto value = store ? inst->GetArg<ir::Value>(1) : ir::Value{inst};
    const u32 size = ir::GetValueSizeByte(value.Type());
    if (!size || size > 16) { EmitHelper(inst); return; }
    context.FlushValues(); context.DiscardValues(); context.PublishFlags();
    as.MV(a0, state); context.HostAddress(t0, reinterpret_cast<u64>(&AcquireMemory)); Call(t0);
    as.MV(a0, state); context.Operand(a1, inst->GetArg<ir::Operand>(0));
    if (memory_continuations.contains(inst)) as.addi_d(a1, a1, 16);
    const auto validated_size = memory_extents.contains(inst) ? memory_extents.at(inst) : size;
    as.LI(a2, validated_size); context.HostAddress(t0, reinterpret_cast<u64>(&GuestAddress)); Call(t0);
    as.MV(t6, a0); context.ReloadFlags(); CheckHalt();
    if (ordered) as.dbar(0);
    // Preserve the naturally aligned scalar access as a single instruction.
    // Baseline hosts need not implement UAL, so misaligned backing addresses
    // use byte accesses without affecting aligned atomicity.
    Label unaligned, finished;
    const u32 natural_alignment = std::min(size, u32{8});
    as.andi(t0, t6, natural_alignment - 1); as.BNE(t0, zero, &unaligned);
    for (u32 part = 0; part < (size + 7) / 8; ++part) {
        const u32 bytes = std::min(u32{8}, size - part * 8);
        if (store) {
            context.ReadPart(t0, value, part);
            const auto before = context.CurrentBufferSize();
            context.Store(t0, t6, part * 8, bytes);
            RecordGuestAccess(before, bytes, GuestAccessKind::Write, t6, part * 8);
        } else {
            const auto before = context.CurrentBufferSize();
            context.Load(t0, t6, part * 8, bytes);
            RecordGuestAccess(before, bytes, GuestAccessKind::Read, t6, part * 8);
            context.WritePart(inst, t0, part);
        }
    }
    as.J(&finished);
    as.Bind(&unaligned);
    for (u32 part = 0; part < (size + 7) / 8; ++part) {
        const u32 bytes = std::min(u32{8}, size - part * 8);
        if (store) {
            context.ReadPart(t0, value, part);
            for (u32 byte = 0; byte < bytes; ++byte) {
                const auto before = context.CurrentBufferSize();
                as.st_b(t0, t6, part * 8 + byte);
                RecordGuestAccess(before, 1, GuestAccessKind::Write, t6, part * 8 + byte);
                if (byte + 1 < bytes) as.srli_d(t0, t0, 8);
            }
        } else {
            as.MV(t0, zero);
            for (u32 byte = 0; byte < bytes; ++byte) {
                const auto before = context.CurrentBufferSize();
                as.ld_bu(t1, t6, part * 8 + byte);
                RecordGuestAccess(before, 1, GuestAccessKind::Read, t6, part * 8 + byte);
                if (byte) as.slli_d(t1, t1, byte * 8);
                as.or_(t0, t0, t1);
            }
            context.WritePart(inst, t0, part);
        }
    }
    as.Bind(&finished);
    if (ordered) as.dbar(0);
    context.Store(zero, state, state_offset_loongarch_guest_fault_cookie);
    as.dbar(0);
    context.FlushValues(); context.DiscardValues();
    if (!memory_extents.contains(inst)) {
        as.MV(a0, state); context.HostAddress(t0, reinterpret_cast<u64>(&ReleaseMemory)); Call(t0);
    }
}
void JitTranslator::EmitLoadMemory(ir::Inst* inst) { EmitMemory(inst, false, false); }
void JitTranslator::EmitLoadMemoryTSO(ir::Inst* inst) { EmitMemory(inst, false, true); }
void JitTranslator::EmitStoreMemory(ir::Inst* inst) { EmitMemory(inst, true, false); }
void JitTranslator::EmitStoreMemoryTSO(ir::Inst* inst) { EmitMemory(inst, true, true); }
void JitTranslator::EmitMemoryBarrierTSO(ir::Inst*) { context.GetMasm().dbar(0); }
void JitTranslator::EmitCheckMemoryAlignment(ir::Inst* inst) {
    auto& as = context.GetMasm();
    const u64 mask = inst->GetArg<ir::Imm>(1).Get();
    context.Read(t0, inst->GetArg<ir::Value>(0)); as.LI(t1, mask); as.and_(t0, t0, t1);
    Label aligned; as.BEQ(t0, zero, &aligned);
    Return(HaltReason::PageFatal); as.Bind(&aligned);
}
}  // namespace swift::runtime::backend::loongarch64
