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
    address.inner.imm = imm;
    address.type = ArgType::Imm;
}

Lambda::Lambda(const Value& value) {
    address.inner.value = value;
    address.type = ArgType::Value;
}

Imm& Lambda::GetImm() {
    ASSERT(address.type == ArgType::Imm);
    return address.inner.imm;
}

Value& Lambda::GetValue() {
    ASSERT(address.type == ArgType::Value);
    return address.inner.value;
}

Value& Lambda::GetValue() const {
    ASSERT(address.type == ArgType::Value);
    return address.inner.value;
}

bool Lambda::IsValue() const { return address.type == ArgType::Value; }

Operand::Operand(const Value& left, const Imm& right, Op op) : left(left), right(right), op(op) {}

Operand::Operand(const Value& left, const Value& right, Op op) : left(left), right(right), op(op) {}

Operand::Op Operand::GetOp() const { return op; }

Uniform::Uniform(u32 offset, ValueType type) : type(type), offset(offset) {}

u32 Uniform::GetOffset() { return offset; }

ValueType Uniform::GetType() { return type; }

}  // namespace swift::runtime::ir