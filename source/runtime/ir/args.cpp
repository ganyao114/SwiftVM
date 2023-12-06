//
// Created by 甘尧 on 2023/9/6.
//

#include "args.h"
#include "runtime/common/logging.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::ir {

void Value::SetType(ValueType type) const {
    ASSERT(Def());
    Def()->SetReturn(type);
}

ValueType Value::Type() const {
    ASSERT(Def());
    return Def()->ReturnType();
}

void Value::Use() const {
    ASSERT(Def());
    Def()->num_use++;
}

void Value::UnUse() const {
    ASSERT(Def());
    Def()->num_use--;
}

Lambda::Lambda() { address.type = ArgType::Void; }

Lambda::Lambda(const Imm& imm) {
    address.imm = imm;
    address.type = ArgType::Imm;
}

Lambda::Lambda(const Value& value) {
    address.value = value;
    address.type = ArgType::Value;
}

Imm& Lambda::GetImm() {
    ASSERT(address.type == ArgType::Imm);
    return address.imm;
}

Value& Lambda::GetValue() {
    ASSERT(address.type == ArgType::Value);
    return address.value;
}

Value& Lambda::GetValue() const {
    ASSERT(address.type == ArgType::Value);
    return address.value;
}

bool Lambda::IsValue() const { return address.type == ArgType::Value; }

void Params::Push(const Value& data) {
    Push(new Param(data));
}

void Params::Push(const Imm& data) {
    Push(new Param(data));
}

void Params::Push(Param* param) {
    if (first_param) {
        auto insert_point = first_param;
        while (insert_point->next_node) {
            insert_point = insert_point->next_node;
        }
        insert_point->next_node = param;
    } else {
        first_param = param;
    }
}

void Params::Destroy() {
    if (!first_param) return;
    auto param = first_param;
    do {
        auto deleted = param;
        param = param->next_node;
        delete deleted;
    } while (param);
}

Operand::Operand(const Value& left, const Imm& right, Op op) : left(left), right(right), op(op) {}

Operand::Operand(const Value& left, const Value& right, Op op) : left(left), right(right), op(op) {}

Operand::Op Operand::GetOp() const { return op; }

Uniform::Uniform(u32 offset, ValueType type) : type(type), offset(offset) {}

u32 Uniform::GetOffset() { return offset; }

ValueType Uniform::GetType() { return type; }

Params::Param::Param(const Value& value) {
    data.value = value;
    data.type = ArgType::Value;
}

Params::Param::Param(const Imm& imm) {
    data.imm = imm;
    data.type = ArgType::Imm;
}

}  // namespace swift::runtime::ir