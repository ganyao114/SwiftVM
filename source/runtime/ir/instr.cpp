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

Inst::Inst(OpCode code) : op_code(code) {
    // These predicates only take Flags, so argument-based inference cannot
    // supply their scalar result type. Canonical interpreters need it too.
    if (code == OpCode::TestFlags || code == OpCode::TestNotFlags)
        ret_type = ValueType::U8;
}

Arg& Inst::ArgAt(int index) { return arguments[index]; }

Arg& Inst::ArgAt(int index) const { return arguments[index]; }

void Inst::SetArg(int index, const Void& arg) {
    DestroyArg(index);
    arguments[index] = arg;
}

void Inst::SetArg(int index, const Value& arg) {
    DestroyArg(index);
    arguments[index] = arg;
    Use(arg);

    // default ret type
    if (HasValue() && ret_type == ValueType::VOID) {
        ret_type = arg.Type();
    }
}

void Inst::SetArg(int index, const Imm& arg) {
    DestroyArg(index);
    arguments[index] = arg;

    // default ret type
    if (HasValue() && ret_type == ValueType::VOID) {
        ret_type = arg.GetType();
    }
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

    // default ret type
    if (ret_type == ValueType::VOID) {
        if (GetIRMetaInfo(op_code).return_type != ArgType::Void) {
            ret_type = arg.type;
        }
    }
}

void Inst::SetArg(int index, const Uniform& arg) {
    DestroyArg(index);
    arguments[index] = arg;

    // default ret type
    if (HasValue() && ret_type == ValueType::VOID) {
        ret_type = arg.GetType();
    }
}

void Inst::SetArg(int index, const Lambda& arg) {
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

void Inst::SetArg(int index, const Params& params) {
    DestroyArg(index);
    arguments[index] = params;
    for (auto param : params) {
        if (auto data = param.data; data.IsValue()) {
            Use(data.value);
        }
    }
}

void Inst::SetArgFresh(int index, const Void& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Value& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
    Use(arg);
    if (HasValue() && ret_type == ValueType::VOID) {
        ret_type = arg.Type();
    }
}

void Inst::SetArgFresh(int index, const Imm& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
    if (HasValue() && ret_type == ValueType::VOID) {
        ret_type = arg.GetType();
    }
}

void Inst::SetArgFresh(int index, const Cond& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Flags& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Local& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
    if (ret_type == ValueType::VOID && GetIRMetaInfo(op_code).return_type != ArgType::Void) {
        ret_type = arg.type;
    }
}

void Inst::SetArgFresh(int index, const Uniform& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
    if (HasValue() && ret_type == ValueType::VOID) {
        ret_type = arg.GetType();
    }
}

void Inst::SetArgFresh(int index, const Lambda& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
    if (arg.IsValue()) {
        Use(arg.GetValue());
    }
}

void Inst::SetArgFresh(int index, const Operand::Op& arg) {
    assert(arguments[index].IsVoid());
    arguments[index] = arg;
}

void Inst::SetArgFresh(int index, const Operand& arg) {
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

void Inst::SetArgFresh(int index, const Params& params) {
    assert(arguments[index].IsVoid());
    arguments[index] = params;
    for (auto param : params) {
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
    auto def = value.Def();
    ASSERT_MSG(def, "Value used by {} is null!", op_code);
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

bool Inst::IsGetHostRegOperation() {
    if (op_code == OpCode::GetHostGPR || op_code == OpCode::GetHostFPR) {
        return GetArg<Imm>(1).Get() == 0;
    }
    return false;
}

bool Inst::IsSetHostRegOperation() {
    if (op_code == OpCode::SetHostGPR || op_code == OpCode::SetHostFPR) {
        return GetArg<Imm>(2).Get() == 0;
    }
    return false;
}

bool Inst::IsBitCastOperation() {
    return op_code == OpCode::BitCast;
}

IREffect Inst::GetEffects() const {
    auto effects = GetIRMetaInfo(op_code).effects;
    if (op_code == OpCode::CallLambda && ArgAt(0).IsLambda()) {
        const auto helper = GetArg<Lambda>(0);
        // Only direct helpers can carry audited traits. Memory effects remain
        // conservative: guest-state traits say nothing about pointed-to memory.
        if (!helper.IsValue() && helper.GetHelperReentryEffect() == HelperReentryEffect::NoReentry) {
            const auto state = helper.GetHelperGuestStateEffect();
            if (state == HelperGuestStateEffect::None || state == HelperGuestStateEffect::WriteOnly)
                effects &= ~IREffect::ReadGuestState;
            if (state == HelperGuestStateEffect::None || state == HelperGuestStateEffect::ReadOnly)
                effects &= ~IREffect::WriteGuestState;
            if (helper.GetHelperFaultEffect() == HelperFaultEffect::NoDirectFault)
                effects &= ~IREffect::MayFault;
        }
    }
    return effects;
}

bool Inst::HasSideEffects() const {
    return HasObservableEffects(GetEffects());
}

void Inst::DestroyArg(u8 arg_idx) {
    auto &arg = ArgAt(arg_idx);
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
        auto &params = arg.Get<Params>();
        for (auto param : params) {
            if (auto data = param.data; data.IsValue()) {
                UnUse(data.value);
            }
        }
        params.Destroy();
    }
    arg = {};
}

void Inst::DestroyArgs() {
    for (u8 i = 0; i < max_args; ++i) {
        DestroyArg(i);
    }
}

void Inst::ReleaseArgs() {
    for (auto& arg : arguments) {
        if (arg.IsVoid()) {
            continue;
        }
        if (arg.IsParams()) {
            // The only argument that owns heap memory. Params::Destroy does
            // not null first_param, so blanking the slot below is what keeps
            // ~Inst from walking a freed chain.
            arg.Get<Params>().Destroy();
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

void Inst::SetVirReg(u16 slot) {
    vir_reg = slot;
}

bool Inst::HasValidTypeViews() const {
    for (auto value : const_cast<Inst*>(this)->GetValues()) {
        if (!value.Defined() || value.Def()->ReturnType() == ValueType::VOID ||
            value.Type() <= ValueType::VOID || value.Type() > ValueType::V256)
            return false;
    }
    // Uniform ranges are byte ranges. Signed/unsigned aliases are legal, but
    // a store view must agree with the range width consumed by state analyses.
    if (op_code == OpCode::StoreUniform && ArgAt(0).GetType() == ArgType::Uniform &&
        ArgAt(1).IsValue()) {
        const auto uniform = GetArg<Uniform>(0);
        const auto value = GetArg<Value>(1);
        if (uniform.GetType() == ValueType::VOID ||
            GetValueSizeByte(uniform.GetType()) != GetValueSizeByte(value.Type()))
            return false;
    }
    return true;
}

void Inst::Validate(Inst* inst) {
    ASSERT(inst);
    ASSERT(inst->op_code >= OpCode::Void && inst->op_code < OpCode::COUNT);
    ASSERT_MSG(inst->HasValidTypeViews(), "{} has an invalid operand type view", inst->op_code);
    if (inst->op_code == OpCode::CallLambda) {
        ASSERT_MSG(inst->ArgAt(0).IsLambda(), "CallLambda arg 0 must be Lambda type!");
        return;
    }
    if (inst->op_code == OpCode::CallLocation) {
        ASSERT_MSG(inst->ArgAt(0).IsLambda(), "CallLocation arg 0 must be Lambda type!");
        return;
    }
    if (inst->op_code == OpCode::CallDynamic) {
        ASSERT_MSG(inst->ArgAt(0).IsLambda(), "CallDynamic arg 0 must be Lambda type!");
        return;
    }
    if (inst->op_code > OpCode::Void && inst->op_code < OpCode::BASE_COUNT) {
        auto& ir_info = GetIRMetaInfo(inst->op_code);
        int inner_arg_index{};
        int arg_index{};
        while (inner_arg_index < Inst::max_args && arg_index < ir_info.arg_types.size()) {
            auto& inst_arg = inst->ArgAt(inner_arg_index);
            auto arg_type = ir_info.arg_types[arg_index];
            ASSERT_MSG(inst_arg.GetType() == arg_type, "{} has invalid arg!", inst->op_code);
            arg_index++;
            inner_arg_index += PhysicalSlots(arg_type);
        }
    } else {
        // SetLocation and every other base opcode are handled above (they are all
        // < BASE_COUNT); anything reaching here is not a real instruction.
        ASSERT_MSG(false, "Unk Instr {}!", inst->op_code);
    }
}

Inst::~Inst() {
    DestroyArgs();
}

}  // namespace swift::runtime::ir
