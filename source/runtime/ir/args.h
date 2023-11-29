//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-pack"
#endif

#include "base/common_funcs.h"
#include "runtime/common/logging.h"
#include "runtime/common/slab_alloc.h"
#include "runtime/common/types.h"
#include "runtime/ir/ir_types.h"

namespace swift::runtime::ir {

class Inst;
class Arg;

enum class Cond : u8 {
    EQ = 0,
    NE,
    CS,
    CC,
    MI,
    PL,
    VS,
    VC,
    HI,
    LS,
    GE,
    LT,
    GT,
    LE,
    AL,
    NV,
    HS = CS,
    LO = CC,
};

struct Void {
    explicit Void(Inst*) {}
};

#pragma pack(1)
class Imm {
public:
    explicit Imm(bool value) : type{ValueType::BOOL}, imm_bool{value} {}
    explicit Imm(u8 value) : type{ValueType::U8}, imm_u8{value} {}
    explicit Imm(u16 value) : type{ValueType::U16}, imm_u16{value} {}
    explicit Imm(u32 value) : type{ValueType::U32}, imm_u32{value} {}
    explicit Imm(u64 value) : type{ValueType::U64}, imm_u64{value} {}

private:
    ValueType type{ValueType::VOID};

    union {
        bool imm_bool;
        u8 imm_u8;
        u16 imm_u16;
        u32 imm_u32;
        u64 imm_u64;
    };
};

#pragma pack(push, 1)
class Value {
public:
    constexpr Value(Inst* in = {}) : inst(in) {}

    [[nodiscard]] Inst* Def() const { return inst; }

    [[nodiscard]] bool Defined() const { return inst; }

    void SetType(ValueType type) const;

    [[nodiscard]] ValueType Type() const;

    void Use() const;
    void UnUse() const;

private:
    Inst* inst{};
};

template <ValueType type_> class TypedValue final : public Value {
public:
    TypedValue() = default;

    template <ValueType other_type> constexpr TypedValue(const TypedValue<other_type>& value)
            : Value(value) {
        ASSERT(value.Type() != type_);
    }

    constexpr TypedValue(const Value& value) : Value(value) { SetType(type_); }

    constexpr TypedValue(Inst* inst) : TypedValue(Value(inst)) { SetType(type_); }
};

using BOOL = TypedValue<ValueType::BOOL>;
using U1 = TypedValue<ValueType::BOOL>;
using U8 = TypedValue<ValueType::U8>;
using U16 = TypedValue<ValueType::U16>;
using U32 = TypedValue<ValueType::U32>;
using U64 = TypedValue<ValueType::U64>;
using U128 = TypedValue<ValueType::U128>;

// Uniform buffer
class Uniform {
public:
    explicit Uniform(u32 offset, ValueType type);

    u32 GetOffset();
    ValueType GetType();

private:
    u32 offset{};
    ValueType type{};
};

struct Local {
    u16 id{};
    ValueType type{};
};

struct DataClass {
    ArgType type{ArgType::Void};
    union {
        Value value;
        Imm imm;
    };

    [[nodiscard]] bool IsValue() const {
        return type == ArgType::Value;
    }
};

class Lambda {
public:
    using FuncAddr = DataClass;

    explicit Lambda();
    explicit Lambda(const Value& value);
    explicit Lambda(const Imm& imm);

    bool IsValue() const;

    Value& GetValue();
    Value& GetValue() const;
    Imm& GetImm();

private:
    mutable FuncAddr address{};
};

enum class Flags : u16 {
    Carry = 1 << 0,
    Overflow = 1 << 1,
    Zero = 1 << 2,
    Negate = 1 << 3,
    Parity = 1 << 4,
    NegZero = Zero | Negate,
    NZCV = Carry | Overflow | Zero | Negate,
    All = Carry | Overflow | Zero | Negate | Parity
};

DECLARE_ENUM_FLAG_OPERATORS(Flags)

struct OperandOp {
    enum Type : u8 {
        Plus = 1 << 0,
        Minus = 1 << 1,
        LSL = 1 << 2,
        LSR = 1 << 3,
        EXT = 1 << 4,
    };

    constexpr OperandOp() = default;

    explicit OperandOp(Type type, u8 shift_ext = 0) : type(type), shift_ext(shift_ext) {}

    Type type{Plus};
    u8 shift_ext{};
};

class Params {
public:
    class Param : public SlabObject<Param, true> {
    public:
        explicit Param() = default;
        explicit Param(const Value& value);
        explicit Param(const Imm& imm);

        DataClass data{};

    private:
        friend class Params;
        Param* next_node{};
    };

    class Iterator {
    public:
        using value_type = Param;
        using difference_type = std::ptrdiff_t;
        using pointer = Param*;
        using reference = Param&;
        using iterator_category = std::forward_iterator_tag;

        explicit Iterator(pointer ptr) : ptr_(ptr) {}

        reference operator*() const { return *ptr_; }
        pointer operator->() { return ptr_; }

        // 前置递增操作符
        Iterator& operator++() {
            ptr_ = ptr_->next_node;
            return *this;
        }

        Iterator operator++(int) {
            Iterator temp = *this;
            ptr_ = ptr_->next_node;
            return temp;
        }

        bool operator==(const Iterator& other) const {
            return ptr_ == other.ptr_;
        }

        bool operator!=(const Iterator& other) const {
            return ptr_ != other.ptr_;
        }

    private:
        pointer ptr_;
    };

    void Push(const Imm& data);
    void Push(const Value& data);

    [[nodiscard]] Iterator begin() const { return Iterator(first_param); }

    [[nodiscard]] Iterator end() const { return Iterator(nullptr); }

    void Destroy();

private:
    void Push(Param* param);
    Param* first_param{};
};

struct ArgClass {
    ArgType type;
    union {
        Value value;
        Imm imm;
        Cond cond;
        OperandOp operand;
        Local local;
        Uniform uniform;
        Lambda lambda;
        Flags flags;
        Params params;
    };

    explicit ArgClass() : type(ArgType::Void) {}

    explicit ArgClass(const Value& v) {
        value = v;
        type = ArgType::Value;
    }

    explicit ArgClass(const Imm& v) {
        imm = v;
        type = ArgType::Imm;
    }

    explicit ArgClass(const OperandOp& v) {
        operand = v;
        type = ArgType::Operand;
    }

    explicit ArgClass(const Local& v) {
        local = v;
        type = ArgType::Local;
    }

    explicit ArgClass(const Uniform& v) {
        uniform = v;
        type = ArgType::Uniform;
    }

    explicit ArgClass(const Lambda& v) {
        lambda = v;
        type = ArgType::Lambda;
    }

    explicit ArgClass(const Cond& v) {
        cond = v;
        type = ArgType::Cond;
    }

    explicit ArgClass(const Flags& v) {
        flags = v;
        type = ArgType::Flags;
    }

    explicit ArgClass(const Params& v) {
        params = v;
        type = ArgType::Params;
    }

    constexpr ArgClass& operator=(const Uniform& v) {
        uniform = v;
        type = ArgType::Uniform;
        return *this;
    }

    constexpr ArgClass& operator=(const Lambda& v) {
        lambda = v;
        type = ArgType::Lambda;
        return *this;
    }

    constexpr ArgClass& operator=(const Imm& v) {
        imm = v;
        type = ArgType::Imm;
        return *this;
    }
};
#pragma pack(pop)

class Operand {
    friend class Inst;

public:
    using Type = ArgClass;
    using Op = OperandOp;

    constexpr Operand() = default;

    explicit Operand(const Value& left, const Imm& right, Op op = {});

    explicit Operand(const Value& left, const Value& right, Op op = {});

    [[nodiscard]] Op GetOp() const;

    Type GetLeft() const { return left; }

    Type GetRight() const { return right; }

private:
    Op op{};
    Type left{};
    Type right{};
};

class Arg {
    friend class Inst;

public:
    Arg() : value() {}
    Arg(const Void& v) : value() {}
    Arg(const ArgClass& v) : value(v) {}
    Arg(const Value& v) : value(v) {}
    Arg(const Imm& v) : value(v) {}
    Arg(const Cond& v) : value(v) {}
    Arg(const Flags& v) : value(v) {}
    Arg(const Operand& v) : value(v.GetOp()) {}
    Arg(const Operand::Op v) : value(v) {}
    Arg(const Local& v) : value(v) {}
    Arg(const Uniform& v) : value(v) {}
    Arg(const Lambda& v) : value(v) {}
    Arg(const Params& v) : value(v) {}

    [[nodiscard]] constexpr bool IsImm() const { return value.type == ArgType::Imm; }

    [[nodiscard]] constexpr bool IsValue() const { return value.type == ArgType::Value; }

    [[nodiscard]] constexpr bool IsOperand() const { return value.type == ArgType::Operand; }

    [[nodiscard]] constexpr bool IsLambda() const { return value.type == ArgType::Lambda; }

    [[nodiscard]] constexpr bool IsVoid() const { return value.type == ArgType::Void; }

    [[nodiscard]] constexpr bool IsParams() const { return value.type == ArgType::Params; }

    [[nodiscard]] ArgType GetType() const { return value.type; }

    template <typename T> constexpr T& Get() {
        if constexpr (std::is_same<T, Value>::value) {
            assert(value.type == ArgType::Value);
            return value.value;
        } else if constexpr (std::is_same<T, Imm>::value) {
            assert(value.type == ArgType::Imm);
            return value.imm;
        } else if constexpr (std::is_same<T, Local>::value) {
            assert(value.type == ArgType::Local);
            return value.local;
        } else if constexpr (std::is_same<T, Uniform>::value) {
            assert(value.type == ArgType::Uniform);
            return value.uniform;
        } else if constexpr (std::is_same<T, Cond>::value) {
            assert(value.type == ArgType::Cond);
            return value.cond;
        } else if constexpr (std::is_same<T, OperandOp>::value) {
            assert(value.type == ArgType::Operand);
            return value.operand;
        } else if constexpr (std::is_same<T, Lambda>::value) {
            assert(value.type == ArgType::Lambda);
            return value.lambda;
        } else if constexpr (std::is_same<T, Flags>::value) {
            assert(value.type == ArgType::Flags);
            return value.flags;
        } else if constexpr (std::is_same<T, Params>::value) {
            assert(value.type == ArgType::Params);
            return value.params;
        } else {
            assert(0);
        }
    }

private:
    ArgClass value;
};

}  // namespace swift::runtime::ir
