//
// Created by 甘尧 on 2023/9/6.
//

#include "instr.h"

#include <cstdlib>

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
// (next_pseudo_inst, the intrusive list node) that the rest of the runtime
// reads without any packed-access annotation.
constexpr size_t kInstSlot = (sizeof(Inst) + 15u) & ~size_t(15u);
constexpr size_t kChunkBytes = 64u * 1024u;
}  // namespace

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

Inst::Inst(OpCode code) : op_code(code) {}

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

void Inst::Use(const Value& value) {
    auto def = value.Def();
    ASSERT_MSG(def, "Value used by {} is null!", op_code);
    def->num_use++;

    if (IsPseudoOperation()) {
        auto insert_point = value.Def();
        while (insert_point->next_pseudo_inst) {
            insert_point = insert_point->next_pseudo_inst;
            assert(insert_point->GetArg<Value>(0).Def() == value.Def());
        }
        insert_point->next_pseudo_inst = this;
    }
}

void Inst::UnUse(const Value& value) {
    auto def = value.Def();
    ASSERT_MSG(def, "Value used by {} is null!", op_code);
    def->num_use--;

    if (IsPseudoOperation()) {
        auto insert_point = value.Def();
        while (insert_point->next_pseudo_inst != this) {
            insert_point = insert_point->next_pseudo_inst;
            assert(insert_point->GetArg<Value>(0).Def() == value.Def());
        }
        insert_point->next_pseudo_inst = next_pseudo_inst;
        next_pseudo_inst = {};
    }
}

u8 Inst::GetUses(bool exclude_pseudo) {
    if (!exclude_pseudo) {
        return num_use;
    }
    u8 pseudo_count{0};
    auto pseudo_inst = next_pseudo_inst;
    while (pseudo_inst) {
        ASSERT(pseudo_inst->GetArg<Value>(0).Def() == this);
        pseudo_count++;
        pseudo_inst = pseudo_inst->next_pseudo_inst;
    }
    return num_use - pseudo_count;
}

Inst::Values Inst::GetValues() {
    Values values{};
    for (auto &arg : arguments) {
        if (arg.IsValue()) {
            values.push_back(arg.Get<Value>());
        } else if (arg.IsLambda() && arg.Get<Lambda>().IsValue()) {
            values.push_back(arg.Get<Lambda>().GetValue());
        } else if (arg.IsParams()) {
            auto& params = arg.Get<Params>();
            for (auto param : params) {
                if (auto data = param.data; data.IsValue()) {
                    values.push_back(data.value);
                }
            }
        }
    }
    return std::move(values);
}

OpCode Inst::GetOp() const {
    return op_code;
}

void Inst::SetId(u16 id_) { this->id = id_; }

void Inst::SetReturn(ValueType type) { this->ret_type = type; }

u16 Inst::Id() const { return id; }

ValueType Inst::ReturnType() const {
    return ret_type;
}

bool Inst::HasValue() { return meta::HasValue(GetIRMetaInfo(op_code).return_type); }

bool Inst::IsPseudoOperation() {
    return op_code == OpCode::GetFlags || op_code == OpCode::SaveFlags || op_code == OpCode::GetResult;
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

bool Inst::HasSideEffects() {
    if (num_use) {
        return true;
    }
    // Value-returning atomics still mutate guest memory when their old value
    // is dead (LOCK NOT is the common case). They must not be removed by DCE.
    //
    // Host calls belong in the same bucket: CallLambda/CallLocation/CallDynamic
    // are opaque to this IR (UniformEliminationPass already treats them as
    // full barriers), and the x86 frontend uses them for helpers that write
    // guest state through the state pointer rather than through their return
    // value -- FNINIT, the SoftFloat x87 helpers, xsave, and so on. Their U64
    // return is frequently unused, and deleting them silently drops the guest
    // side effect: with these three missing from this list, function-mode
    // compilation dropped the x87 control-word / TOP updates and
    // "x87 directed edge semantics" read TOP=0 after nine FLD1s instead of 7.
    if (op_code == OpCode::CompareAndSwap || op_code == OpCode::CompareAndSwap128 ||
        op_code == OpCode::AtomicExchange || op_code == OpCode::AtomicFetchAdd ||
        op_code == OpCode::AtomicRMW || op_code == OpCode::X87Op ||
        op_code == OpCode::CallLambda || op_code == OpCode::CallLocation ||
        op_code == OpCode::CallDynamic) {
        return true;
    }
    auto &ir_info = GetIRMetaInfo(op_code);
    return ir_info.return_type == ArgType::Void;
}

Inst::Pseudos Inst::GetPseudoOperations(OpCode code) {
    Pseudos pseudos{};
    auto pseudo_inst = next_pseudo_inst;
    while (pseudo_inst) {
        ASSERT(pseudo_inst->GetArg<Value>(0).Def() == this);
        if (pseudo_inst->op_code == code) {
            pseudos.push_back(pseudo_inst);
        }
        pseudo_inst = pseudo_inst->next_pseudo_inst;
    }
    return std::move(pseudos);
}

Inst::Pseudos Inst::GetPseudoOperations() {
    Pseudos pseudos{};
    auto pseudo_inst = next_pseudo_inst;
    while (pseudo_inst) {
        ASSERT(pseudo_inst->GetArg<Value>(0).Def() == this);
        pseudos.push_back(pseudo_inst);
        pseudo_inst = pseudo_inst->next_pseudo_inst;
    }
    return std::move(pseudos);
}

bool Inst::HasFlagsSavePseudo() {
    auto pseudo_inst = next_pseudo_inst;
    while (pseudo_inst) {
        ASSERT(pseudo_inst->GetArg<Value>(0).Def() == this);
        if (pseudo_inst->GetOp() == OpCode::SaveFlags) {
            return true;
        }
        pseudo_inst = pseudo_inst->next_pseudo_inst;
    }
    return false;
}

void Inst::DestroyArg(u8 arg_idx) {
    auto &arg = ArgAt(arg_idx);
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

void Inst::Reset() {
    DestroyArgs();
    op_code = OpCode::Void;
}

void Inst::SetVirReg(u16 slot) {
    vir_reg = slot;
}

void Inst::Validate(Inst* inst) {
    ASSERT(inst);
    ASSERT(inst->op_code >= OpCode::Void && inst->op_code < OpCode::COUNT);
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

int Inst::PublicIndex(int logical_index) const {
    auto& info = GetIRMetaInfo(op_code);
    ASSERT(logical_index >= 0 && logical_index < (int)info.arg_types.size());
    int physical = logical_index;
    for (int i = 0; i < logical_index; i++) {
        physical += PhysicalSlots(info.arg_types[i]) - 1;
    }
    ASSERT(physical < max_args);
    return physical;
}

Inst::~Inst() {
    DestroyArgs();
}

}  // namespace swift::runtime::ir
