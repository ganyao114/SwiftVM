//
// Created by 甘尧 on 2023/9/6.
//

#include "args.h"
#include "runtime/common/logging.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::ir {

ValueType Imm::GetType() const { return type; }

u64 Imm::Get() const {
    switch (type) {
        case ValueType::U8:
            return imm_u8;
        case ValueType::U16:
            return imm_u16;
        case ValueType::U32:
            return imm_u32;
        case ValueType::U64:
            return imm_u64;
        default:
            return 0;
    }
}

s64 Imm::GetSigned() const {
    if (!IsSigned()) {
        return static_cast<s64>(Get());
    }
    switch (type) {
        case ValueType::S8:
            return imm_s8;
        case ValueType::S16:
            return imm_s16;
        case ValueType::S32:
            return imm_s32;
        case ValueType::S64:
            return imm_s64;
        default:
            return 0;
    }
}

bool Imm::IsSigned() const {
    return type >= ValueType::S8 && type <= ValueType::S64;
}

bool Imm::IsNegate() const {
    if (IsSigned()) {
        return GetSigned() < 0;
    } else {
        return false;
    }
}

Value Value::SetType(ValueType type) {
    ASSERT(Def());
    Def()->SetReturn(type);
    return *this;
}

Value Value::SetCastType(ValueType type) {
    ASSERT(Def());
    cast_type = type;
    return *this;
}

ValueType Value::Type() const {
    ASSERT(Def());
    if (cast_type == ValueType::VOID) {
        return Def()->ReturnType();
    } else {
        return cast_type;
    }
}

void Value::Use() const {
    ASSERT(Def());
    Def()->num_use++;
}

void Value::UnUse() const {
    ASSERT(Def());
    Def()->num_use--;
}

u16 Value::Id() const { return Def()->Id(); }

bool Value::operator==(const Value& rhs) const { return inst == rhs.inst; }

bool Value::operator!=(const Value& rhs) const { return !(rhs == *this); }

ArgClass DataClass::ToArgClass() const {
    if (type == ArgType::Value) {
        return ArgClass{value};
    } else if (type == ArgType::Imm) {
        return ArgClass{imm};
    } else {
        PANIC();
    }
}

Imm& Lambda::GetImm() {
    ASSERT(address.type == ArgType::Imm);
    return address.imm;
}

Imm& Lambda::GetImm() const {
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

bool Lambda::IsValue() const { return address.IsValue(); }

void Params::Push(const Value& data) { Push(new Param(data)); }

void Params::Push(const Imm& data) { Push(new Param(data)); }

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

Operand::Operand(const Imm& left) : left(left) {}

Operand::Operand(const Value& left) : left(left) {}

Operand::Operand(const Type& left, const Type& right, Op op) : left(left), right(right), op(op) {}

Operand::Operand(const Value& left, const Imm& right, Op op) : left(left), right(right), op(op) {}

Operand::Operand(const Value& left, const Value& right, Op op) : left(left), right(right), op(op) {}

Operand::Op Operand::GetOp() const { return op; }

Uniform::Uniform(u32 offset, ValueType type) : type(type), offset(offset) {}

u32 Uniform::GetOffset() const { return offset; }

ValueType Uniform::GetType() const { return type; }

bool Uniform::operator==(const Uniform& rhs) const { return offset == rhs.offset; }

bool Uniform::operator!=(const Uniform& rhs) const { return !(rhs == *this); }

Params::Param::Param(const Value& value) {
    data.value = value;
    data.type = ArgType::Value;
}

Params::Param::Param(const Imm& imm) {
    data.imm = imm;
    data.type = ArgType::Imm;
}

const char* CondString(Cond cond) {
#define ENUM_CLASS Cond
    switch (cond) { COND_ENUM(ENUM_TO_STRING_CASE) }
    return "Unk";
#undef ENUM_CLASS
}

std::string FlagsString(Flags flags) {
    std::string result{};
    if (True(flags & Flags::Carry)) {
        result += "CF, ";
    }
    if (True(flags & Flags::Overflow)) {
        result += "OF, ";
    }
    if (True(flags & Flags::Zero)) {
        result += "ZF, ";
    }
    if (True(flags & Flags::Negate)) {
        result += "SF, ";
    }
    if (True(flags & Flags::AuxiliaryCarry)) {
        result += "AF, ";
    }
    if (True(flags & Flags::Parity)) {
        result += "PF, ";
    }
    size_t end = result.find_last_not_of(", ");
    result = result.substr(0, end + 1);
    return result;
}

}  // namespace swift::runtime::ir