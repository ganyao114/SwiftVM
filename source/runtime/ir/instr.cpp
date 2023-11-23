//
// Created by 甘尧 on 2023/9/6.
//

#include "instr.h"

namespace swift::runtime::ir {

Inst::Inst(OpCode code) : op_code(code) {}

Arg& Inst::ArgAt(int index) { return arguments[index]; }

void Inst::SetArg(int index, const Void& arg) {
    DestroyArg(index);
    arguments[index] = arg;
}

void Inst::SetArg(int index, const Value& arg) {
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
    DestroyArg(index);
    arguments[index++] = arg;
    DestroyArg(index);
    arguments[index++] = arg.left;
    DestroyArg(index);
    arguments[index++] = arg.right;
    if (arg.left.type == ArgType::Value) {
        Use(arg.left.value);
    }
    if (arg.right.type == ArgType::Value) {
        Use(arg.right.value);
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

OpCode Inst::GetOp() {
    return op_code;
}

void Inst::SetId(u16 id_) { this->id = id_; }

void Inst::SetReturn(ValueType type) { this->ret_type = type; }

u16 Inst::Id() const { return id; }

ValueType Inst::ReturnType() const {
    return ret_type;
}

bool Inst::HasValue() { return GetIRMetaInfo(op_code).return_type == ArgType::Value; }

bool Inst::IsPseudoOperation() {
    switch (op_code) {
        case OpCode::GetCarry:
        case OpCode::GetOverFlow:
        case OpCode::GetNegate:
        case OpCode::GetZero:
        case OpCode::GetNZCV:
        case OpCode::GetNegZero:
        case OpCode::GetAllFlags:
            return true;
        default:
            return false;
    }
}

Inst* Inst::GetPseudoOperation(OpCode code) {
    auto pseudo_inst = next_pseudo_inst;
    while (pseudo_inst) {
        if (pseudo_inst->op_code == code) {
            ASSERT(pseudo_inst->GetArg<Value>(0).Def() == this);
            return pseudo_inst;
        }
        pseudo_inst = pseudo_inst->next_pseudo_inst;
    }
    return {};
}

void Inst::DestroyArg(u8 arg_idx) {
    auto &arg = ArgAt(arg_idx);
    if (arg.IsValue()) {
        UnUse(arg.Get<Value>());
    } else if (arg.IsLambda() && arg.Get<Lambda>().IsValue()) {
        UnUse(arg.Get<Lambda>().GetValue());
    }
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
        while (inner_arg_index < Inst::max_args) {
            auto& inst_arg = inst->ArgAt(inner_arg_index);
            auto arg_type = ir_info.arg_types[arg_index];
            ASSERT_MSG(inst_arg.GetType() == arg_type, "{} has invalid arg!", inst->op_code);
            arg_index++;
            if (arg_type == ArgType::Operand) {
                inner_arg_index += 3;
            } else {
                inner_arg_index++;
            }
        }
    } else {
        switch (inst->op_code) {
            case OpCode::SetLocation: {
                ASSERT_MSG(inst->ArgAt(0).GetType() == ArgType::Imm,
                           "SetLocation arg 0 must be Imm type!");
                break;
            }
            default:
                ASSERT_MSG(false, "Unk Instr {}!", inst->op_code);
                break;
        }
    }
}

Inst::~Inst() {
    for (int i = 0; i < max_args; ++i) {
        DestroyArg(i);
    }
}

}  // namespace swift::runtime::ir