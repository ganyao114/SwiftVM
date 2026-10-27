//
// Created by 甘尧 on 2023/9/6.
//

#include "instr.h"

#include <cstdlib>

#include "runtime/common/perf_stats.h"

namespace swift::runtime::ir {

namespace {
// See the comment on Inst::operator new. All three are trivially constructible
// and trivially destructible on purpose: a thread_local with a non-trivial
// destructor costs a guard check on every access, and there is nothing to run
// at thread exit -- chunks are deliberately never handed back (below).
thread_local void* tls_inst_free_head{};
thread_local u8* tls_inst_bump{};
thread_local size_t tls_inst_bump_left{};

// Inst is `#pragma pack(1)`, so its size need not be a multiple of anything.
// Round the slot up to 16 bytes so every instruction keeps exactly the
// alignment malloc used to give it -- an Inst holds raw pointers
// (the intrusive list node) that the rest of the runtime
// reads without any packed-access annotation.
constexpr size_t kInstSlot = (sizeof(Inst) + 15u) & ~size_t(15u);
constexpr size_t kChunkBytes = 64u * 1024u;
}  // namespace

bool IRBuildFastEnabled() {
    return GetSvmConfig().ir_fast;
}

void* Inst::operator new(size_t sz) {
    ASSERT(sz == sizeof(Inst));
    if (auto* head = tls_inst_free_head) {
        tls_inst_free_head = *static_cast<void**>(head);
        return head;
    }
    if (tls_inst_bump_left < kInstSlot) {
        // The tail of the old chunk (< one slot) is abandoned; at 64 KiB per
        // chunk that is under 0.2% of it.
        auto* chunk = static_cast<u8*>(std::malloc(kChunkBytes));
        ASSERT_MSG(chunk != nullptr, "out of memory allocating an IR instruction chunk");
        tls_inst_bump = chunk;
        tls_inst_bump_left = kChunkBytes;
        // Once per 64 KiB, so unconditional (no PerfEnabled() gate) costs
        // nothing measurable and keeps the counter meaningful in any build.
        PerfAdd(GetPerfStats().ir_arena_bytes, kChunkBytes);
    }
    auto* mem = tls_inst_bump;
    tls_inst_bump += kInstSlot;
    tls_inst_bump_left -= kInstSlot;
    return mem;
}

void Inst::operator delete(void* p) {
    if (!p) {
        return;
    }
    // Unbounded by construction: the storage belongs to a chunk this allocator
    // owns forever, so "retained on the free list" and "free space in the
    // arena" are the same thing. A block freed by a thread other than the one
    // that allocated it joins the freeing thread's list, which is why chunks
    // must never be released -- an Inst outlives the thread that created it
    // whenever a compiled unit is reclaimed from another thread.
    *static_cast<void**>(p) = tls_inst_free_head;
    tls_inst_free_head = p;
}

Inst::Inst(OpCode code) : Inst(code, meta::ResolveResultType(code)) {}

Inst::Inst(OpCode code, ValueType type) : op_code(code), ret_type(type) {
    (void)GetIRMetaInfo(code);
}

const Arg& Inst::ArgAt(int index) const { return arguments[index]; }

void Inst::SetArg(int index, const Void& arg) {
    DestroyArg(index);
    arguments[index] = arg;
}

void Inst::SetArg(int index, const Value& arg) {
    CheckUse(arg);
    DestroyArg(index);
    arguments[index] = arg;
    Use(arg);

}

void Inst::SetArg(int index, const Imm& arg) {
    DestroyArg(index);
    arguments[index] = arg;

}

void Inst::SetArg(int index, const Cond& arg) {
    DestroyArg(index);
    arguments[index] = arg;
}

void Inst::SetArg(int index, const Flags& arg) {
    DestroyArg(index);
    arguments[index] = arg;
}

void Inst::SetArg(int index, const Local& arg) {
    DestroyArg(index);
    arguments[index] = arg;

}

void Inst::SetArg(int index, const Uniform& arg) {
    DestroyArg(index);
    arguments[index] = arg;

}

void Inst::SetArg(int index, const Lambda& arg) {
    if (arg.IsValue()) CheckUse(arg.GetValue());
    DestroyArg(index);
    arguments[index] = arg;
    if (arg.IsValue()) {
        Use(arg.GetValue());
    }
}

void Inst::SetArg(int index, const Operand::Op& arg) {
    DestroyArg(index);
    arguments[index] = arg;
}

void Inst::SetArg(int index, const Operand& arg) {
    if (arg.left.IsValue()) CheckUse(arg.left.value);
    if (arg.right.IsValue()) CheckUse(arg.right.value);
    ASSERT(index + PhysicalSlots(ArgType::Operand) - 1 < max_args);
    DestroyArg(index);
    arguments[index++] = arg;
    DestroyArg(index);
    arguments[index++] = arg.left.ToArgClass();
    DestroyArg(index);
    arguments[index++] = arg.right.ToArgClass();
    if (arg.left.type == ArgType::Value) {
        Use(arg.left.value);
    }
    if (arg.right.type == ArgType::Value) {
        Use(arg.right.value);
    }
}

void Inst::SetArg(int index, Params&& params) {
    CheckMutable();
    ASSERT(index >= 0 && index < max_args);
    for (auto param : params)
        if (param.data.IsValue()) CheckUse(param.data.value);
    DestroyArg(index);
    arguments[index].AdoptParams(std::move(params));
    for (auto param : arguments[index].Get<Params>()) {
        if (auto data = param.data; data.IsValue()) {
            Use(data.value);
        }
    }
}

void Inst::SetArgFresh(int index, const Void& arg) {
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Value& arg) {
    CheckUse(arg);
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
    Use(arg);
}

void Inst::SetArgFresh(int index, const Imm& arg) {
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Cond& arg) {
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Flags& arg) {
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Local& arg) {
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Uniform& arg) {
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Lambda& arg) {
    if (arg.IsValue()) CheckUse(arg.GetValue());
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
    if (arg.IsValue()) {
        Use(arg.GetValue());
    }
}

void Inst::SetArgFresh(int index, const Operand::Op& arg) {
    CheckMutable();
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Operand& arg) {
    if (arg.left.IsValue()) CheckUse(arg.left.value);
    if (arg.right.IsValue()) CheckUse(arg.right.value);
    CheckMutable();
    ASSERT(index + PhysicalSlots(ArgType::Operand) - 1 < max_args);
    assert(arguments[index].IsVoid());
    arguments[index++] = arg;
    assert(arguments[index].IsVoid());
    arguments[index++] = arg.left.ToArgClass();
    assert(arguments[index].IsVoid());
    arguments[index++] = arg.right.ToArgClass();
    if (arg.left.type == ArgType::Value) {
        Use(arg.left.value);
    }
    if (arg.right.type == ArgType::Value) {
        Use(arg.right.value);
    }
}

void Inst::SetArgFresh(int index, Params&& params) {
    CheckMutable();
    ASSERT(index >= 0 && index < max_args);
    ASSERT(arguments[index].IsVoid());
    for (auto param : params)
        if (param.data.IsValue()) CheckUse(param.data.value);
    arguments[index].AdoptParams(std::move(params));
    for (auto param : arguments[index].Get<Params>()) {
        if (auto data = param.data; data.IsValue()) {
            Use(data.value);
        }
    }
}

void Inst::Use(const Value& value) {
    auto def = value.Def();
    ASSERT_MSG(def, "Value used by {} is null!", op_code);
    ASSERT(def->num_use != UINT32_MAX);
    def->num_use++;

    if (IsPseudoOperation()) {
        def->pseudo_use_count++;
    }
}

void Inst::UnUse(const Value& value) {
    CheckMutable();
    auto def = value.Def();
    ASSERT_MSG(def, "Value used by {} is null!", op_code);
    def->CheckMutable();
    ASSERT(def->num_use != 0);
    def->num_use--;

    if (IsPseudoOperation()) {
        ASSERT(def->pseudo_use_count != 0);
        def->pseudo_use_count--;
    }
}

u32 Inst::GetUses(bool exclude_pseudo) const {
    ASSERT(pseudo_use_count <= num_use);
    return exclude_pseudo ? num_use - pseudo_use_count : num_use;
}

bool Inst::IsPseudoOperation() const {
    return op_code == OpCode::GetFlags || op_code == OpCode::SaveFlags ||
           op_code == OpCode::BranchOnlyFlags || op_code == OpCode::GetResult ||
           op_code == OpCode::Div128Remainder || op_code == OpCode::CpuidUpper;
}

bool Inst::IsGetHostRegOperation() const {
    if (op_code == OpCode::GetHostGPR || op_code == OpCode::GetHostFPR) {
        return GetArg<Imm>(1).Get() == 0;
    }
    return false;
}

bool Inst::IsSetHostRegOperation() const {
    if (op_code == OpCode::SetHostGPR || op_code == OpCode::SetHostFPR) {
        return GetArg<Imm>(2).Get() == 0;
    }
    return false;
}

bool Inst::IsBitCastOperation() const {
    return op_code == OpCode::BitCast;
}

IREffect Inst::GetEffects() const {
    auto effects = GetIRMetaInfo(op_code).effects;
    if (op_code == OpCode::CallLambda && ArgAt(0).IsLambda()) {
        const auto helper = HelperStateEffects::Resolve(GetArg<Lambda>(0));
        // Only direct helpers can carry audited traits. Memory effects remain
        // conservative: guest-state traits say nothing about pointed-to memory.
        if (helper.direct && !helper.MayReenter()) {
            if (!helper.ReadsGuestState()) effects &= ~IREffect::ReadGuestState;
            if (!helper.WritesGuestState()) effects &= ~IREffect::WriteGuestState;
            if (!helper.MayFault()) effects &= ~IREffect::MayFault;
        }
    }
    return effects;
}

bool Inst::HasSideEffects() const {
    return HasObservableEffects(GetEffects());
}

void Inst::DestroyArg(u8 arg_idx) {
    CheckMutable();
    auto& arg = arguments[arg_idx];
    // An empty slot has nothing to unregister and is already blank. Most
    // instructions use one or two of the five, and ~Inst runs this over all
    // five again after ReleaseArgs has blanked them.
    if (arg.IsVoid()) {
        return;
    }
    if (arg.IsValue()) {
        UnUse(arg.Get<Value>());
    } else if (arg.IsLambda() && arg.Get<Lambda>().IsValue()) {
        UnUse(arg.Get<Lambda>().GetValue());
    } else if (arg.IsParams()) {
        const auto params = arg.Get<Params>();
        for (auto param : params) {
            if (auto data = param.data; data.IsValue()) {
                UnUse(data.value);
            }
        }
        arg.DestroyParams();
    }
    arg = {};
}

void Inst::DestroyArgs() {
    for (u8 i = 0; i < max_args; ++i) {
        DestroyArg(i);
    }
}

void Inst::ReleaseArgs() {
    CheckMutable();
    for (auto& arg : arguments) {
        if (arg.IsVoid()) {
            continue;
        }
        if (arg.IsParams()) {
            // Release only owned storage: every definition may already have
            // been freed by this whole-IR teardown.
            arg.DestroyParams();
        }
        arg = {};
    }
    num_use = 0;
    pseudo_use_count = 0;
}

void Inst::Reset() {
    DestroyArgs();
    op_code = OpCode::Void;
}

bool Inst::HasValidResultType() const {
    if (op_code == OpCode::Void || op_code >= OpCode::BASE_COUNT) return false;
    // Function lowering can use an untyped branch or a typed label identity;
    // neither representation materializes a data result.
    if (meta::IsControlToken(op_code))
        return ret_type == ValueType::VOID || IsValidValueType(ret_type);
    const auto& info = GetIRMetaInfo(op_code);
    if (info.return_type == ArgType::Void) return ret_type == ValueType::VOID;
    if (ret_type != ValueType::VOID) return IsValidValueType(ret_type);
    return info.result_policy == ResultTypePolicy::Call;
}

bool Inst::HasValidArguments() const {
    return CheckArguments([](Value) {});
}

IntegerPredicateKind Inst::GetIntegerPredicateKind() const {
    switch (op_code) {
        case OpCode::TestZero: return IntegerPredicateKind::Zero;
        case OpCode::TestNotZero: return IntegerPredicateKind::Nonzero;
        case OpCode::TestBit: return IntegerPredicateKind::Bit;
        case OpCode::Not:
            return arguments[1].IsVoid() ? IntegerPredicateKind::Zero : IntegerPredicateKind::None;
        default: return IntegerPredicateKind::None;
    }
}

bool Inst::HasBooleanResult() const {
    if (!IsScalarIntegerType(ret_type)) return false;
    if (GetIntegerPredicateKind() != IntegerPredicateKind::None) return true;
    switch (op_code) {
        case OpCode::LoadImm:
            return arguments[0].IsImm() && arguments[0].Get<Imm>().Get() <= 1;
        case OpCode::TestFlags: case OpCode::TestNotFlags:
        case OpCode::CondSet: case OpCode::LocalCondSet:
        case OpCode::LocalParitySet: case OpCode::FCmpCondSet:
            return true;
        default: return false;
    }
}

bool Inst::HasValidScalarArguments() const {
    const auto scalar = [&](u8 index) {
        if (!arguments[index].IsValue()) return false;
        const auto& value = arguments[index].Get<Value>();
        return value.Defined() && IsScalarIntegerType(value.Type());
    };
    const auto unary = [&] { return IsScalarIntegerType(ret_type) && scalar(0); };
    const auto integer_imm = [&](u8 index) {
        return arguments[index].IsImm() &&
               IsScalarIntegerType(arguments[index].Get<Imm>().GetType());
    };
    const auto scalar_data = [&](u8 index) {
        return arguments[index].IsImm() ? integer_imm(index) : scalar(index);
    };
    if (const auto kind = GetIntegerPredicateKind(); kind != IntegerPredicateKind::None) {
        return unary() && (kind != IntegerPredicateKind::Bit ||
                (integer_imm(1) && arguments[1].Get<Imm>().Get() < 64));
    }
    if (ClassifyIntegerShift(op_code) != IntegerShiftKind::None) {
        return unary() && (arguments[1].IsImm() ? integer_imm(1) : scalar(1));
    }
    const auto field = [&](u8 index) {
        if (!arguments[index].IsImm() || !arguments[index + 1].IsImm()) return false;
        const auto& position = arguments[index].Get<Imm>();
        const auto& length = arguments[index + 1].Get<Imm>();
        if (!IsScalarIntegerType(position.GetType()) || !IsScalarIntegerType(length.GetType()))
            return false;
        const u64 lsb = position.Get();
        const u64 width = length.Get();
        // Keep the subtraction guarded: large/overflowing immediates must
        // not wrap into a valid field. A zero-width field is well-defined.
        return lsb < 64 && width <= 64 - lsb;
    };
    switch (op_code) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::Mul:
        case OpCode::Div:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
        case OpCode::Not:
            // The Operand header occupies slot1; both present data slots
            // belong to the integer domain. Unary Not returned above.
            return unary() && scalar_data(2) &&
                   (arguments[3].IsVoid() || scalar_data(3));
        case OpCode::MulSub: return unary() && scalar(1) && scalar(2);
        case OpCode::SignedDiv64: return unary() && scalar(1);
        case OpCode::MulHigh: return unary() && scalar(1) && integer_imm(2);
        case OpCode::Div128: return unary() && scalar(1) && scalar(2) && integer_imm(3);
        case OpCode::Select:
        case OpCode::SelectZero:
        case OpCode::Goto:
        case OpCode::NotGoto: return scalar(0);
        case OpCode::BitExtract:
        case OpCode::BitClear: return unary() && field(1);
        case OpCode::BitInsert: return unary() && scalar(1) && field(2);
        case OpCode::ByteSwap: {
            if (!arguments[1].IsImm()) return false;
            const auto bits = arguments[1].Get<Imm>().Get();
            return unary() && bits >= 16 && IsScalarBitWidth(bits);
        }
        case OpCode::Crc32c:
            return unary() && scalar(1) && arguments[2].IsImm() &&
                   IsScalarBitWidth(arguments[2].Get<Imm>().Get());
        case OpCode::CountLeadingZeros32:
        case OpCode::CountTrailingZeros32: {
            if (!arguments[0].IsValue() || !arguments[0].Get<Value>().Defined()) return false;
            const auto type = arguments[0].Get<Value>().Type();
            return IsScalarIntegerType(ret_type) && IsScalarIntegerType(type, 32);
        }
        case OpCode::CountLeadingZeros64:
        case OpCode::CountTrailingZeros64: {
            if (!arguments[0].IsValue() || !arguments[0].Get<Value>().Defined()) return false;
            const auto type = arguments[0].Get<Value>().Type();
            return IsScalarIntegerType(ret_type) && IsScalarIntegerType(type, 64);
        }
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
        case OpCode::ZeroExtend64:
        case OpCode::SignExtend:
        case OpCode::Neg:
        case OpCode::PopCount: return unary();
        default: return true;
    }
}

void Inst::Validate(const Inst* inst) {
    ASSERT(inst);
    ASSERT(inst->op_code >= OpCode::Void && inst->op_code < OpCode::COUNT);
    ASSERT_MSG(inst->HasValidResultType(), "{} has an invalid result type", inst->op_code);
    ASSERT_MSG(inst->HasValidArguments(), "{} has invalid arguments", inst->op_code);
}

Inst::~Inst() {
    ASSERT(!owner);
    DestroyArgs();
}

}  // namespace swift::runtime::ir
