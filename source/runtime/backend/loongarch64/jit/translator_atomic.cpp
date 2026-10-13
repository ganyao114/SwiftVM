#include "translator.h"
#include "helpers.h"
#include "fault_scope.h"

#include <stdexcept>
#include "runtime/ir/atomic_rmw.h"

namespace swift::runtime::backend::loongarch64 {
namespace {

using O = ir::OpCode;
using R = ir::AtomicRMWOp;

enum class AtomicKind {
    Exchange, Add, And, Or, Xor, Neg, Compare,
    SignedMax, SignedMin, UnsignedMax, UnsignedMin,
};

AtomicKind Kind(R operation) {
    switch (operation) {
        case R::Add: case R::Sub: case R::AddCarry: case R::SubBorrow: return AtomicKind::Add;
        case R::And: return AtomicKind::And;
        case R::Or: return AtomicKind::Or;
        case R::Xor: return AtomicKind::Xor;
        case R::Neg: return AtomicKind::Neg;
        case R::SignedMax: return AtomicKind::SignedMax;
        case R::SignedMin: return AtomicKind::SignedMin;
        case R::UnsignedMax: return AtomicKind::UnsignedMax;
        case R::UnsignedMin: return AtomicKind::UnsignedMin;
    }
    throw std::runtime_error("invalid LoongArch atomic RMW operation");
}

template<class Record> void AtomicUpdate(JitContext& context, u32 size, AtomicKind kind, const Record& record) {
    auto& as = context.GetMasm();
    // The result, replacement and address registers must all be distinct:
    // rd == rj is illegal and rd == rk has an undefined result on LoongArch.
#define AMO(word, wide) \
    { const auto before = context.CurrentBufferSize(); \
      if (size == 4) as.word(t0, t1, t4); else as.wide(t0, t1, t4); \
      record(before, GuestAccessKind::ReadWrite); }
    switch (kind) {
        case AtomicKind::Exchange: AMO(amswap_db_w, amswap_db_d); break;
        case AtomicKind::Add: AMO(amadd_db_w, amadd_db_d); break;
        case AtomicKind::And: AMO(amand_db_w, amand_db_d); break;
        case AtomicKind::Or: AMO(amor_db_w, amor_db_d); break;
        case AtomicKind::Xor: AMO(amxor_db_w, amxor_db_d); break;
        case AtomicKind::SignedMax: AMO(ammax_db_w, ammax_db_d); break;
        case AtomicKind::SignedMin: AMO(ammin_db_w, ammin_db_d); break;
        case AtomicKind::UnsignedMax: AMO(ammax_db_wu, ammax_db_du); break;
        case AtomicKind::UnsignedMin: AMO(ammin_db_wu, ammin_db_du); break;
        case AtomicKind::Compare: case AtomicKind::Neg: {
            // LL/SC has no ordering by itself. Both a successful update and
            // the failed-compare path participate in the seq_cst contract.
            as.dbar(0);
            Label retry, complete;
            as.Bind(&retry);
            auto before = context.CurrentBufferSize();
            if (size == 4) {
                as.ll_w(t0, t4, 0);
                record(before, GuestAccessKind::Read);
                context.Mask(t0, 32);  // LL.W sign-extends its observed word.
            } else { as.ll_d(t0, t4, 0); record(before, GuestAccessKind::Read); }
            if (kind == AtomicKind::Compare) {
                as.BNE(t0, t1, &complete);
                as.MV(t3, t2);
            } else as.sub_d(t3, zero, t0);
            before = context.CurrentBufferSize();
            if (size == 4) as.sc_w(t3, t4, 0);
            else as.sc_d(t3, t4, 0);
            record(before, GuestAccessKind::Write);
            // Unlike RV64 SC, LoongArch SC reports one on success.
            as.BEQ(t3, zero, &retry);
            as.Bind(&complete); as.dbar(0);
            break;
        }
    }
#undef AMO
}

}  // namespace

void JitTranslator::EmitAtomic(ir::Inst* inst) {
    const auto op = inst->GetOp();
    const bool rmw = op == O::AtomicRMW;
    const auto input = inst->GetArg<ir::Value>(rmw ? 2 : 1);
    const auto size = ir::GetValueSizeByte(input.Type());
    if (ir::IsFloatValueType(input.Type()) || (size != 4 && size != 8)) {
        EmitHelper(inst);
        return;
    }
    const auto address = inst->GetArg<ir::Value>(rmw ? 1 : 0);
    const auto raw_operation = rmw ? inst->GetArg<ir::Imm>(0).Get() : u64{0};
    if (rmw && raw_operation > static_cast<u64>(R::UnsignedMin))
        throw std::runtime_error("invalid LoongArch atomic RMW operation");
    const auto operation = static_cast<R>(raw_operation);
    const auto kind = rmw ? Kind(operation) : op == O::CompareAndSwap ? AtomicKind::Compare :
            op == O::AtomicExchange ? AtomicKind::Exchange : AtomicKind::Add;
    auto& as = context.GetMasm();
    context.FlushValues(); context.DiscardValues(); context.PublishFlags();
    // The shared reader lease prevents the unaligned writer kernel from
    // racing native AMO/LLSC. EmitHelper releases it before taking that gate.
    as.MV(a0, state); context.HostAddress(t0, reinterpret_cast<u64>(&AcquireMemory)); Call(t0);
    as.MV(a0, state); context.Read(a1, address); as.LI(a2, size);
    context.HostAddress(t0, reinterpret_cast<u64>(&GuestAddress)); Call(t0);
    as.MV(t4, a0); context.ReloadFlags(); CheckHalt();

    Label slow, done;
    context.Read(t3, address);
    context.Load(t2, state, offsetof(State, guest_addr_mask));
    as.and_(t3, t3, t2); as.andi(t3, t3, size - 1); as.BNE(t3, zero, &slow);
    as.andi(t3, t4, size - 1); as.BNE(t3, zero, &slow);
    context.Read(t1, input); context.Mask(t1, size * 8);
    if (kind == AtomicKind::Compare) {
        context.Read(t2, inst->GetArg<ir::Value>(2)); context.Mask(t2, size * 8);
    } else if (rmw) {
        // Carry/borrow is an explicit immutable SSA input to the atomic IR;
        // the retry loop never reads or updates the resident flags register.
        if (operation == R::AddCarry || operation == R::SubBorrow) {
            context.Read(t3, inst->GetArg<ir::Value>(3)); as.andi(t3, t3, 1);
            as.add_d(t1, t1, t3);
        }
        if (operation == R::Sub || operation == R::SubBorrow) as.sub_d(t1, zero, t1);
    }
    AtomicUpdate(context, size, kind, [&](u32 before, GuestAccessKind access) {
        RecordGuestAccess(before, size, access, t4);
    });
    context.Store(zero, state, state_offset_loongarch_guest_fault_cookie);
    as.dbar(0);
    context.Mask(t0, size * 8);
    context.Write(inst, t0); context.FlushValues(); context.DiscardValues();
    as.J(&done);
    as.Bind(&slow);
    EmitHelper(inst);
    as.Bind(&done);
    // Both paths write canonical homes and leave the compile-time cache
    // empty, so subsequent reads cannot depend on a branch-only cache fill.
    context.DiscardValues(); context.ReloadFlags();
}

void JitTranslator::EmitAtomicExchange(ir::Inst* inst) { EmitAtomic(inst); }
void JitTranslator::EmitAtomicFetchAdd(ir::Inst* inst) { EmitAtomic(inst); }
void JitTranslator::EmitAtomicRMW(ir::Inst* inst) { EmitAtomic(inst); }
void JitTranslator::EmitCompareAndSwap(ir::Inst* inst) { EmitAtomic(inst); }

}  // namespace swift::runtime::backend::loongarch64
