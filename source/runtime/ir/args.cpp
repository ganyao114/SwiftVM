//
// Created by 甘尧 on 2023/9/6.
//

#include "args.h"
#include <array>
#include <atomic>
#include <mutex>
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
        case ValueType::S8:
            return static_cast<u8>(imm_s8);
        case ValueType::S16:
            return static_cast<u16>(imm_s16);
        case ValueType::S32:
            return static_cast<u32>(imm_s32);
        case ValueType::S64:
            return static_cast<u64>(imm_s64);
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
    } else if (type == ArgType::Void) {
        // Single-sided Operand: the empty right side maps to a Void arg.
        return ArgClass{};
    } else {
        PANIC();
    }
}

namespace {

constexpr u8 kUniformEffectTag = 0x80;
constexpr u8 kHelperABITag = 0x40;
constexpr u8 kUniformEffectMask = 0x3f;
constexpr size_t kMaxUniformEffectSets = kUniformEffectMask + 1;

std::array<const UniformEffectSet*, kMaxUniformEffectSets> uniform_effect_sets{
        nullptr, &kNoUniformEffects};
std::mutex uniform_effect_sets_mutex;
std::atomic<u8> uniform_effect_set_count{2};

}  // namespace

UniformEffectId RegisterUniformEffectSet(const UniformEffectSet* effects) {
    ASSERT(effects);
    std::scoped_lock lock{uniform_effect_sets_mutex};
    const auto count = uniform_effect_set_count.load(std::memory_order_relaxed);
    for (u8 id = 1; id < count; ++id) {
        if (uniform_effect_sets[id] == effects) {
            return static_cast<UniformEffectId>(id);
        }
    }
    ASSERT(count < kMaxUniformEffectSets);
    if (count >= kMaxUniformEffectSets) {
        return UniformEffectId::Unknown;
    }
    const auto id = count;
    uniform_effect_sets[id] = effects;
    uniform_effect_set_count.store(id + 1, std::memory_order_release);
    return static_cast<UniformEffectId>(id);
}

const UniformEffectSet* LookupUniformEffectSet(UniformEffectId id) {
    const auto index = static_cast<u8>(id);
    if (index >= uniform_effect_set_count.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return uniform_effect_sets[index];
}

Lambda::Lambda(const DataClass& value, UniformEffectId effects) : address(value) {
    ASSERT(address.type == ArgType::Imm);
    const auto id = static_cast<u8>(effects);
    ASSERT(id > 0 && id <= kUniformEffectMask);
    if (id == 0 || id > kUniformEffectMask) {
        return;
    }
    address.type = static_cast<ArgType>(kUniformEffectTag | id);
}

Lambda::Lambda(const DataClass& value, HelperABI abi) : address(value) {
    ASSERT(address.type == ArgType::Imm);
    if (abi == HelperABI::PreserveAllLeaf) {
        address.type = static_cast<ArgType>(kHelperABITag);
    }
}

bool Lambda::IsTaggedImm() const {
    return (static_cast<u8>(address.type) & (kUniformEffectTag | kHelperABITag)) != 0;
}

Imm& Lambda::GetImm() {
    ASSERT(address.type == ArgType::Imm || IsTaggedImm());
    return address.imm;
}

Imm& Lambda::GetImm() const {
    ASSERT(address.type == ArgType::Imm || IsTaggedImm());
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

UniformEffectId Lambda::GetUniformEffectId() const {
    if ((static_cast<u8>(address.type) & kUniformEffectTag) == 0) {
        return UniformEffectId::Unknown;
    }
    return static_cast<UniformEffectId>(static_cast<u8>(address.type) & kUniformEffectMask);
}

HelperABI Lambda::GetHelperABI() const {
    return (static_cast<u8>(address.type) & kHelperABITag) != 0
            ? HelperABI::PreserveAllLeaf
            : HelperABI::NormalAAPCS;
}

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

bool Uniform::operator==(const Uniform& rhs) const { return offset == rhs.offset && type == rhs.type; }

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
