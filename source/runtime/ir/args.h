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
#include "fmt/format.h"

namespace swift::runtime::ir {

class Inst;
class Arg;
struct ArgClass;

#define COND_ENUM(X) \
    X(EQ) \
    X(NE) \
    X(CS) \
    X(CC) \
    X(MI) \
    X(PL) \
    X(VS) \
    X(VC) \
    X(HI) \
    X(LS) \
    X(GE) \
    X(LT) \
    X(GT) \
    X(LE) \
    X(AL) \
    X(NV)

enum class Cond : u8 { COND_ENUM(ENUM_DEFINE) };

const char *CondString(Cond cond);

struct Void {
    explicit Void(Inst*) {}
};

#pragma pack(push, 1)
class Imm {
public:
    explicit Imm(bool value) : type{ValueType::BOOL}, imm_u8{value} {}
    explicit Imm(u8 value) : type{ValueType::U8}, imm_u8{value} {}
    explicit Imm(u16 value) : type{ValueType::U16}, imm_u16{value} {}
    explicit Imm(u32 value) : type{ValueType::U32}, imm_u32{value} {}
    explicit Imm(u64 value) : type{ValueType::U64}, imm_u64{value} {}
    explicit Imm(u64 value, ValueType size) : type{size}, imm_u64{value} {}

    [[nodiscard]] ValueType GetType() const;
    [[nodiscard]] u64 GetValue() const;

private:
    ValueType type{ValueType::VOID};

    union {
        u8 imm_u8;
        u16 imm_u16;
        u32 imm_u32;
        u64 imm_u64;
    };
};

class Value {
public:
    constexpr Value(Inst* in = {}) : inst(in) {}

    [[nodiscard]] Inst* Def() const { return inst; }

    [[nodiscard]] bool Defined() const { return inst; }

    [[nodiscard]] Value SetType(ValueType type) const;

    [[nodiscard]] ValueType Type() const;

    void Use() const;
    void UnUse() const;
    [[nodiscard]] u16 Id() const;

protected:
    Inst* inst;
    ValueType cast_type{ValueType::VOID};
};

template <ValueType type_> class TypedValue final : public Value {
public:
    TypedValue() = default;

    template <ValueType other_type> constexpr TypedValue(const TypedValue<other_type>& value)
            : Value(value) {
        ASSERT(value.Type() != type_);
    }

    constexpr TypedValue(const Value& value) : Value(value) { cast_type = type_; }

    constexpr TypedValue(Inst* inst) : TypedValue(Value(inst)) { cast_type = type_; }
};

using BOOL = TypedValue<ValueType::BOOL>;
using U1 = TypedValue<ValueType::BOOL>;
using U8 = TypedValue<ValueType::U8>;
using U16 = TypedValue<ValueType::U16>;
using U32 = TypedValue<ValueType::U32>;
using U64 = TypedValue<ValueType::U64>;

// Uniform buffer
class Uniform {
public:
    explicit Uniform(u32 offset, ValueType type);

    [[nodiscard]] u32 GetOffset() const;
    [[nodiscard]] ValueType GetType() const;

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

    constexpr DataClass() {}

    constexpr DataClass(const Value& v) : value(v), type(ArgType::Value) {}

    constexpr DataClass(const Imm& v) : imm(v), type(ArgType::Imm) {}

    [[nodiscard]] bool IsValue() const {
        return type == ArgType::Value;
    }

    [[nodiscard]] ArgClass ToArgClass() const;
};

class Lambda {
public:
    using FuncAddr = DataClass;

    constexpr Lambda() : address() {}

    constexpr Lambda(const Value& value) : address(value) {}

    constexpr Lambda(const Imm& imm) : address(imm) {}

    bool IsValue() const;

    Value& GetValue();
    Value& GetValue() const;
    Imm& GetImm();
    Imm& GetImm() const;

private:
    mutable FuncAddr address;
};

struct FlagsBit {
    constexpr static u8 Carry = 0;
    constexpr static u8 Overflow = 1;
    constexpr static u8 Zero = 2;
    constexpr static u8 Negate = 3;
    constexpr static u8 Parity = 4;
    constexpr static u8 Positive = 4;
};

enum class Flags : u16 {
    Carry = 1 << FlagsBit::Carry,
    Overflow = 1 << FlagsBit::Overflow,
    Zero = 1 << FlagsBit::Zero,
    Negate = 1 << FlagsBit::Negate,
    Parity = 1 << FlagsBit::Parity,
    Positive = 1 << FlagsBit::Positive,
    NegZero = Zero | Negate,
    NZCV = Carry | Overflow | Zero | Negate,
    All = Carry | Overflow | Zero | Negate | Parity
};

DECLARE_ENUM_FLAG_OPERATORS(Flags)

struct OperandOp {
    enum Type : u8 {
        None = 0,
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
    using Type = DataClass;
    using Op = OperandOp;

    constexpr Operand() = default;

    explicit Operand(const Value& left, const Imm& right, Op op = {});

    explicit Operand(const Value& left, const Value& right, Op op = {});

    explicit Operand(const Imm& left);

    explicit Operand(const Value& left);

    [[nodiscard]] Op GetOp() const;

    [[nodiscard]] Type GetLeft() const { return left; }

    [[nodiscard]] Type GetRight() const { return right; }

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
            ASSERT(value.type == ArgType::Value);
            return value.value;
        } else if constexpr (std::is_same<T, Imm>::value) {
            ASSERT(value.type == ArgType::Imm);
            return value.imm;
        } else if constexpr (std::is_same<T, Local>::value) {
            ASSERT(value.type == ArgType::Local);
            return value.local;
        } else if constexpr (std::is_same<T, Uniform>::value) {
            ASSERT(value.type == ArgType::Uniform);
            return value.uniform;
        } else if constexpr (std::is_same<T, Cond>::value) {
            ASSERT(value.type == ArgType::Cond);
            return value.cond;
        } else if constexpr (std::is_same<T, OperandOp>::value) {
            ASSERT(value.type == ArgType::Operand);
            return value.operand;
        } else if constexpr (std::is_same<T, Lambda>::value) {
            ASSERT(value.type == ArgType::Lambda);
            return value.lambda;
        } else if constexpr (std::is_same<T, Flags>::value) {
            ASSERT(value.type == ArgType::Flags);
            return value.flags;
        } else if constexpr (std::is_same<T, Params>::value) {
            ASSERT(value.type == ArgType::Params);
            return value.params;
        } else {
            PANIC();
        }
    }

    [[maybe_unused]] DataClass ToDataClass() const {
        if (value.type == ArgType::Value) {
            return value.value;
        } else if (value.type == ArgType::Imm) {
            return value.imm;
        } else {
            PANIC();
        }
    }

private:
    ArgClass value;
};

}  // namespace swift::runtime::ir

// formatters
template <> struct fmt::formatter<swift::runtime::ir::Cond> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(swift::runtime::ir::Cond cond, FormatContext& ctx) const {
        return formatter<std::string>::format(swift::runtime::ir::CondString(cond), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Value> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Value &value, FormatContext& ctx) const {
        return formatter<std::string>::format(fmt::format("({}) @{}", value.Type(), value.Id()), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Imm> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Imm &imm, FormatContext& ctx) const {
        return formatter<std::string>::format(fmt::format("({}) #{}", imm.GetType(), imm.GetValue()), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Uniform> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Uniform &uni, FormatContext& ctx) const {
        return formatter<std::string>::format(fmt::format("({}) u[{}]", uni.GetType(), uni.GetOffset()), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Local> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Local &local, FormatContext& ctx) const {
        return formatter<std::string>::format(fmt::format("({}) a[{}]", local.type, local.id), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Flags> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(swift::runtime::ir::Flags flags, FormatContext& ctx) const {
        return formatter<std::string>::format(fmt::format("f{:b}", (uint16_t) flags), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::DataClass> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::DataClass &data, FormatContext& ctx) const {
        if (data.IsValue()) {
            return formatter<std::string>::format(fmt::format("{}", data.value), ctx);
        } else {
            return formatter<std::string>::format(fmt::format("{}", data.imm), ctx);
        }
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Lambda> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Lambda &lambda, FormatContext& ctx) const {
        if (lambda.IsValue()) {
            return formatter<std::string>::format(fmt::format("{}", lambda.GetValue()), ctx);
        } else {
            return formatter<std::string>::format(fmt::format("{}", lambda.GetImm()), ctx);
        }
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Operand> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Operand &operand, FormatContext& ctx) const {
        std::string result{};
        switch (operand.GetOp().type) {
            case swift::runtime::ir::OperandOp::Plus:
                result.append(fmt::format("{} + {}", operand.GetLeft(), operand.GetRight()));
                break;
            case swift::runtime::ir::OperandOp::Minus:
                result.append(fmt::format("{} - {}", operand.GetLeft(), operand.GetRight()));
                break;
            case swift::runtime::ir::OperandOp::LSL:
                result.append(fmt::format("{} << {}", operand.GetLeft(), operand.GetRight()));
                break;
            case swift::runtime::ir::OperandOp::LSR:
                result.append(fmt::format("{} >> {}", operand.GetLeft(), operand.GetRight()));
                break;
            case swift::runtime::ir::OperandOp::EXT:
                result.append(fmt::format("{} + {}", operand.GetLeft(), operand.GetRight()));
                break;
            default:
                result.append(fmt::format("{}", operand.GetLeft()));
                break;
        }
        return formatter<std::string>::format(fmt::format("[{}]", result), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Params> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Params &params, FormatContext& ctx) const {
        std::string result{};
        bool first{true};
        for (auto param : params) {
            if (!first) {
                result.append(", ");
            }
            result.append(fmt::format("{}", param.data));
            first = false;
        }
        return formatter<std::string>::format(fmt::format("[{}]", result), ctx);
    }
};