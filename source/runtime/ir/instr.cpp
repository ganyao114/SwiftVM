//
// Created by 甘尧 on 2023/9/6.
//

#include "instr.h"

namespace swift::runtime::ir {

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