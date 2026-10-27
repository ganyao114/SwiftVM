//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-pack"
#endif

#include <utility>

#include "base/common_funcs.h"
#include "runtime/common/logging.h"
#include "runtime/common/slab_alloc.h"
#include "runtime/common/types.h"
#include "runtime/ir/ir_types.h"
#include "fmt/format.h"

namespace swift::runtime::ir {
class Inst;
class Arg;
class HIRFunction;
class HIRTestSupport;
struct ArgClass;
namespace terminal {
struct If;
struct Switch;
}  // namespace terminal

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

constexpr bool IsValidCondition(Cond condition) {
    return static_cast<u8>(condition) <= static_cast<u8>(Cond::NV);
}

constexpr bool IsAlwaysCondition(Cond condition) {
    return condition == Cond::AL || condition == Cond::NV;
}

const char *CondString(Cond cond);

struct Void {
    explicit Void(const Inst*) {}
};

#pragma pack(push, 1)
class Imm {
public:
    constexpr Imm(bool value) : type{ValueType::U8}, imm_u8{value} {}
    constexpr Imm(u8 value) : type{ValueType::U8}, imm_u8{value} {}
    constexpr Imm(u16 value) : type{ValueType::U16}, imm_u16{value} {}
    constexpr Imm(u32 value) : type{ValueType::U32}, imm_u32{value} {}
    constexpr Imm(u64 value) : type{ValueType::U64}, imm_u64{value} {}
    constexpr Imm(s8 value) : type{ValueType::S8}, imm_s8{value} {}
    constexpr Imm(s16 value) : type{ValueType::S16}, imm_s16{value} {}
    constexpr Imm(s32 value) : type{ValueType::S32}, imm_s32{value} {}
    constexpr Imm(s64 value) : type{ValueType::S64}, imm_s64{value} {}
    constexpr Imm(u64 value, ValueType size) : type{size}, imm_u64{value} {}

    [[nodiscard]] ValueType GetType() const { return type; }
    [[nodiscard]] bool IsSigned() const {
        return type >= ValueType::S8 && type <= ValueType::S64;
    }
    [[nodiscard]] bool IsNegate() const {
        return IsSigned() && GetSigned() < 0;
    }
    [[nodiscard]] u64 Get() const {
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
    [[nodiscard]] s64 GetSigned() const {
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

private:
    ValueType type{ValueType::VOID};

    union {
        u8 imm_u8;
        u16 imm_u16;
        u32 imm_u32;
        u64 imm_u64;
        s8 imm_s8;
        s16 imm_s16;
        s32 imm_s32;
        s64 imm_s64;
    };
};

const inline Imm HostRegIndex(u16 index) {
    return Imm(index);
}

class Value {
public:
    constexpr Value(const Inst* in = {}) : inst(in) {}

    [[nodiscard]] const Inst* Def() const { return inst; }

    [[nodiscard]] bool Defined() const { return inst; }

    [[nodiscard]] Value SetCastType(ValueType type) {
        ASSERT(Def());
        cast_type = type;
        return *this;
    }

    // A type view belongs to a use edge, not to its canonical definition.
    [[nodiscard]] Value WithDefinition(const Inst* definition) const;

    [[nodiscard]] ValueType Type() const;
    [[nodiscard]] bool HasValidTypeView() const;
    [[nodiscard]] bool HasValidDataView() const;

    bool operator==(const Value& rhs) const { return inst == rhs.inst; }
    bool operator!=(const Value& rhs) const { return !(rhs == *this); }

    [[nodiscard]] u16 Id() const;

protected:
    const Inst* inst;
    ValueType cast_type{ValueType::VOID};

private:
    friend class Inst;
    friend class HIRFunction;
    friend struct terminal::If;
    friend struct terminal::Switch;
    void Use() const;
    void UnUse() const;
};

template <ValueType type_> class TypedValue final : public Value {
public:
    constexpr static ValueType TYPE = type_;

    TypedValue() = default;

    template <ValueType other_type> constexpr TypedValue(const TypedValue<other_type>& value)
            : Value(value) {
        ASSERT(value.Type() != type_);
    }

    constexpr TypedValue(const Value& value) : Value(value) { cast_type = type_; }

    constexpr TypedValue(const Inst* inst) : TypedValue(Value(inst)) { cast_type = type_; }
};

using VOID = TypedValue<ValueType::VOID>;
using BOOL = TypedValue<ValueType::U8>;
using U8 = TypedValue<ValueType::U8>;
using U16 = TypedValue<ValueType::U16>;
using U32 = TypedValue<ValueType::U32>;
using U64 = TypedValue<ValueType::U64>;

// Uniform buffer
class Uniform {
public:
    explicit Uniform() = default;

    explicit Uniform(u32 offset, ValueType type);

    [[nodiscard]] u32 GetOffset() const;
    [[nodiscard]] ValueType GetType() const;

    bool operator==(const Uniform& rhs) const;
    bool operator!=(const Uniform& rhs) const;

private:
    u32 offset{};
    ValueType type{ValueType::VOID};
};

struct Local {
    u16 id{};
    ValueType type{};
};

struct DataClass {
    ArgType type;
    union {
        Value value;
        Imm imm;
    };

    constexpr DataClass() : type(ArgType::Void) {}

    constexpr DataClass(const Value& v) : value(v), type(ArgType::Value) {}

    constexpr DataClass(const Imm& v) : imm(v), type(ArgType::Imm) {}

    [[nodiscard]] bool Null() const {
        return type == ArgType::Void;
    }

    [[nodiscard]] bool IsValue() const {
        return type == ArgType::Value;
    }

    [[nodiscard]] bool IsImm() const {
        return type == ArgType::Imm;
    }

    [[nodiscard]] ArgClass ToArgClass() const;
};

struct UniformEffectRange {
    u32 offset{};
    u32 size{};
};

// Uniform bytes a helper may write. This metadata is intentionally not used
// for dead-store elimination because it does not describe helper reads.
struct UniformEffectSet {
    const UniformEffectRange* ranges{};
    u8 count{};
};

enum class UniformEffectId : u8 {
    Unknown = 0,
    None = 1,
};

// ABI metadata for a direct host helper call. Every existing Lambda keeps the
// conservative normal AAPCS default, including all indirect calls.
enum class HelperABI : u8 {
    NormalAAPCS,
    PreserveAllLeaf,
};

// Host floating-point environment effects for a direct helper call. The zero
// value is deliberately conservative so every existing/new Lambda remains a
// full FPCR boundary until its call site explicitly opts in.
enum class HostFpEffect : u8 {
    MayTouch = 0,
    FPCRTransparent,
};

enum class HostRegisterEffect : u8 {
    MayTouchSIMD = 0,
    GeneralOnly,
    PreservesPinnedState,
};

enum class HelperGuestStateEffect : u8 {
    MayReadWrite = 0,
    ReadOnly,
    WriteOnly,
    None,
};

enum class HelperFaultEffect : u8 {
    MayFault = 0,
    NoDirectFault,
};

enum class HelperReentryEffect : u8 {
    MayReenter = 0,
    NoReentry,
};

enum class HostFlagsEffect : u8 {
    MayTouch = 0,
    PreservesNZCV,
};

struct HelperCallTraits {
    UniformEffectId uniform{UniformEffectId::Unknown};
    HelperABI abi{HelperABI::NormalAAPCS};
    HostFpEffect host_fp{HostFpEffect::MayTouch};
    HostRegisterEffect host_registers{HostRegisterEffect::MayTouchSIMD};
    HelperGuestStateEffect guest_state{HelperGuestStateEffect::MayReadWrite};
    HelperFaultEffect fault{HelperFaultEffect::MayFault};
    HelperReentryEffect reentry{HelperReentryEffect::MayReenter};
    HostFlagsEffect host_flags{HostFlagsEffect::MayTouch};
};

static_assert(static_cast<u8>(HostFpEffect::MayTouch) == 0);
static_assert(static_cast<u8>(HostRegisterEffect::MayTouchSIMD) == 0);
static_assert(static_cast<u8>(HelperGuestStateEffect::MayReadWrite) == 0);
static_assert(static_cast<u8>(HelperFaultEffect::MayFault) == 0);
static_assert(static_cast<u8>(HelperReentryEffect::MayReenter) == 0);
static_assert(static_cast<u8>(HostFlagsEffect::MayTouch) == 0);

inline constexpr UniformEffectSet kNoUniformEffects{};

UniformEffectId RegisterUniformEffectSet(const UniformEffectSet* effects);
const UniformEffectSet* LookupUniformEffectSet(UniformEffectId id);

class Lambda {
public:
    using FuncAddr = DataClass;

    constexpr Lambda() : address() {}

    constexpr Lambda(const DataClass& value) : address(value) {}

    constexpr Lambda(const Value& value) : address(value) {}

    constexpr Lambda(const Imm& imm) : address(imm) {}

    Lambda(const DataClass& value, UniformEffectId effects);

    Lambda(const DataClass& value, HelperABI abi);

    Lambda(const DataClass& value, HelperCallTraits traits);

    bool IsValue() const;
    bool IsImm() const;

    Value& GetValue();
    const Value& GetValue() const;
    Imm& GetImm();
    const Imm& GetImm() const;
    UniformEffectId GetUniformEffectId() const;
    HelperABI GetHelperABI() const;
    HostFpEffect GetHostFpEffect() const;
    HostRegisterEffect GetHostRegisterEffect() const;
    HelperGuestStateEffect GetHelperGuestStateEffect() const;
    HelperFaultEffect GetHelperFaultEffect() const;
    HelperReentryEffect GetHelperReentryEffect() const;
    HostFlagsEffect GetHostFlagsEffect() const;

private:
    [[nodiscard]] bool IsTaggedImm() const;

    FuncAddr address;
    u8 helper_effects{};
};

// Parsed direct-helper semantics, independent of host ABI/register policy.
// The body traits do not describe reentry: consumers must account for it
// separately. Uniform write ranges likewise do not prove absence of reads,
// faults or reentry. Indirect helpers retain the conservative defaults.
struct HelperStateEffects final {
    bool direct{};
    HelperGuestStateEffect access{HelperGuestStateEffect::MayReadWrite};
    HelperFaultEffect fault{HelperFaultEffect::MayFault};
    HelperReentryEffect reentry{HelperReentryEffect::MayReenter};
    UniformEffectId uniform{UniformEffectId::Unknown};

    [[nodiscard]] static HelperStateEffects Resolve(const Lambda& helper);
    [[nodiscard]] bool ReadsGuestState() const {
        return access == HelperGuestStateEffect::MayReadWrite ||
               access == HelperGuestStateEffect::ReadOnly;
    }
    [[nodiscard]] bool WritesGuestState() const {
        return access == HelperGuestStateEffect::MayReadWrite ||
               access == HelperGuestStateEffect::WriteOnly;
    }
    [[nodiscard]] bool MayFault() const { return fault == HelperFaultEffect::MayFault; }
    [[nodiscard]] bool MayReenter() const { return reentry == HelperReentryEffect::MayReenter; }
    [[nodiscard]] bool RequiresGuestStatePublication() const {
        return ReadsGuestState() || WritesGuestState() || MayFault() || MayReenter();
    }
};

struct FlagsBit {
    constexpr static u8 Carry = 0;
    constexpr static u8 Overflow = 1;
    constexpr static u8 Zero = 2;
    constexpr static u8 Negate = 3;
    constexpr static u8 Parity = 4;
    constexpr static u8 Positive = 5;
    // hack for x86 AF flag
    constexpr static u8 AuxiliaryCarry = 6;
};

enum class Flags : u64 {
    None = 0,
    Carry = 1 << FlagsBit::Carry,
    Overflow = 1 << FlagsBit::Overflow,
    Zero = 1 << FlagsBit::Zero,
    Negate = 1 << FlagsBit::Negate,
    Parity = 1 << FlagsBit::Parity,
    Positive = 1 << FlagsBit::Positive,
    AuxiliaryCarry = 1 << FlagsBit::AuxiliaryCarry,
    NegZero = Zero | Negate,
    NZ = NegZero,
    CV = Carry | Overflow,
    NZCV = Carry | Overflow | Zero | Negate,
    All = Carry | Overflow | Zero | Negate | Parity | AuxiliaryCarry
};

DECLARE_ENUM_FLAG_OPERATORS(Flags)

std::string FlagsString(Flags flags);

struct OperandOp {
    enum Type : u8 {
        None = 0,
        Plus = 1 << 0,
        LSL = 1 << 1,
        LSR = 1 << 2,
        PlusExt = 1 << 3,
    };

    constexpr OperandOp() = default;

    constexpr OperandOp(Type type, u8 shift_ext = 0) : type(type), shift_ext(shift_ext) {}

    bool operator==(const OperandOp& rhs) const {
        return type == rhs.type;
    }

    bool operator!=(const OperandOp& rhs) const { return !(rhs == *this); }

    Type type{Plus};
    u8 shift_ext{};
};

constexpr OperandOp OperandNone{OperandOp::None};
constexpr OperandOp OperandPlus{OperandOp::Plus};
constexpr OperandOp OperandLsl{OperandOp::LSL};
constexpr OperandOp OperandLsr{OperandOp::LSR};

class Params {
    friend class Arg;
    friend struct ArgClass;
    friend class HIRTestSupport;

    struct Node : SlabObject<Node, true> {
        explicit Node(const Value& value);
        explicit Node(const Imm& imm);

        DataClass data;
        Node* next_node{};
    };

    // Installed lists belong to Inst, whose teardown decides whether to
    // unregister uses. Keep this payload trivial inside the packed ArgClass.
    struct Storage {
        Node* first;
    };

public:
    // A detached data snapshot: iteration never lends a node or write alias,
    // including while a Params owner is still waiting to be installed.
    struct Param {
        DataClass data;
    };

    class Iterator {
    public:
        using value_type = Param;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Param;
        using iterator_category = std::forward_iterator_tag;

        Iterator() = default;
        explicit Iterator(const Node* ptr) : ptr_(ptr) {}
        reference operator*() const { return Param{ptr_->data}; }
        Iterator& operator++() { ptr_ = ptr_->next_node; return *this; }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator& other) const { return ptr_ == other.ptr_; }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
    private:
        const Node* ptr_{};
    };
    using ConstIterator = Iterator;

    class View {
    public:
        ConstIterator begin() const { return ConstIterator(first); }
        ConstIterator end() const { return ConstIterator(nullptr); }
    private:
        friend class Params;
        explicit View(const Node* first) : first(first) {}
        const Node* first;
    };

    Params() = default;
    ~Params();
    Params(const Params&) = delete;
    Params& operator=(const Params&) = delete;
    Params(Params&& other) noexcept;
    Params& operator=(Params&& other) noexcept;

    void Push(const Imm& data);
    void Push(const Value& data);

    [[nodiscard]] ConstIterator begin() const { return ConstIterator(first_param); }
    [[nodiscard]] ConstIterator end() const { return ConstIterator(nullptr); }
    [[nodiscard]] View Read() const { return View(first_param); }

    void Destroy() noexcept;

private:
    [[nodiscard]] Storage ReleaseStorage() noexcept { return {std::exchange(first_param, nullptr)}; }
    [[nodiscard]] static View ReadStorage(const Storage& storage) { return View(storage.first); }
    static void DestroyStorage(Storage& storage) noexcept;
    void Push(Node* param);
    Node* first_param{};
};

struct ArgClass {
private:
    friend class Arg;

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
        Params::Storage params;
    };

public:
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
static_assert(std::is_trivially_copyable_v<ArgClass>);
static_assert(std::is_trivially_destructible_v<ArgClass>);
#pragma pack(pop)

class Operand {
    friend class Inst;

public:
    using Type = DataClass;
    using Op = OperandOp;

    constexpr Operand() = default;

    explicit Operand(const Type& left, const Type& right = {}, Op op = {});

    explicit Operand(const Value& left, const Imm& right, Op op = {});

    explicit Operand(const Value& left, const Value& right, Op op = {});

    explicit Operand(const Imm& left);

    explicit Operand(const Value& left);

    [[nodiscard]] Op GetOp() const { return op; }

    [[nodiscard]] Type GetLeft() const { return left; }

    [[nodiscard]] Type GetRight() const { return right; }

    [[nodiscard]] bool IsImm() const { return left.IsImm() && right.Null(); }

private:
    Op op{};
    Type left{};
    Type right{};
};

class Arg {
    friend class Inst;

public:
    Arg(const Arg&) = delete;
    Arg& operator=(const Arg&) = delete;
    Arg(Arg&& other) noexcept : value(other.value) {
        if (other.IsParams()) other.value.params = {};
        other.value.type = ArgType::Void;
    }
    Arg& operator=(Arg&& other) noexcept {
        if (this == &other) return *this;
        // Only Inst can install Params. It must unregister its old uses and
        // destroy that list before replacing the storage.
        ASSERT(!IsParams());
        value = other.value;
        if (other.IsParams()) other.value.params = {};
        other.value.type = ArgType::Void;
        return *this;
    }
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

    [[nodiscard]] constexpr bool IsImm() const { return value.type == ArgType::Imm; }

    [[nodiscard]] constexpr bool IsValue() const { return value.type == ArgType::Value; }

    [[nodiscard]] constexpr bool IsOperand() const { return value.type == ArgType::Operand; }

    [[nodiscard]] constexpr bool IsLambda() const { return value.type == ArgType::Lambda; }

    [[nodiscard]] constexpr bool IsVoid() const { return value.type == ArgType::Void; }

    [[nodiscard]] constexpr bool IsParams() const { return value.type == ArgType::Params; }

    [[nodiscard]] ArgType GetType() const { return value.type; }

    template <typename T> constexpr decltype(auto) Get() const {
        if constexpr (std::is_same_v<T, Params>) {
            ASSERT(IsParams());
            return Params::ReadStorage(value.params);
        } else return Storage<T>(*this);
    }
    template <typename T> constexpr decltype(auto) Get() {
        if constexpr (std::is_same_v<T, Params>) return std::as_const(*this).Get<T>();
        else return Storage<T>(*this);
    }

private:
    void AdoptParams(Params&& params) noexcept {
        ASSERT(IsVoid());
        value.params = params.ReleaseStorage();
        value.type = ArgType::Params;
    }
    void DestroyParams() noexcept {
        ASSERT(IsParams());
        Params::DestroyStorage(value.params);
        value.type = ArgType::Void;
    }

    template <typename T, typename Self> static constexpr auto& Storage(Self& self) {
        if constexpr (std::is_same<T, Value>::value) {
            ASSERT(self.value.type == ArgType::Value);
            return self.value.value;
        } else if constexpr (std::is_same<T, Imm>::value) {
            ASSERT(self.value.type == ArgType::Imm);
            return self.value.imm;
        } else if constexpr (std::is_same<T, Local>::value) {
            ASSERT(self.value.type == ArgType::Local);
            return self.value.local;
        } else if constexpr (std::is_same<T, Uniform>::value) {
            ASSERT(self.value.type == ArgType::Uniform);
            return self.value.uniform;
        } else if constexpr (std::is_same<T, Cond>::value) {
            ASSERT(self.value.type == ArgType::Cond);
            return self.value.cond;
        } else if constexpr (std::is_same<T, OperandOp>::value) {
            ASSERT(self.value.type == ArgType::Operand);
            return self.value.operand;
        } else if constexpr (std::is_same<T, Lambda>::value) {
            ASSERT(self.value.type == ArgType::Lambda);
            return self.value.lambda;
        } else if constexpr (std::is_same<T, Flags>::value) {
            ASSERT(self.value.type == ArgType::Flags);
            return self.value.flags;
        } else {
            PANIC();
        }
    }

public:
    [[maybe_unused]] DataClass ToDataClass() const {
        if (value.type == ArgType::Value) {
            return value.value;
        } else if (value.type == ArgType::Imm) {
            return value.imm;
        } else if (value.type == ArgType::Void) {
            // Single-sided Operand: the empty side maps back to a null DataClass.
            return {};
        } else {
            PANIC("Invalid arg type!");
        }
        return {};
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
        if (swift::runtime::ir::IsSignValueType(imm.GetType())) {
            return formatter<std::string>::format(fmt::format("({}) #{}", imm.GetType(), imm.GetSigned()), ctx);
        } else {
            return formatter<std::string>::format(fmt::format("({}) #{}", imm.GetType(), imm.Get()), ctx);
        }
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
        return formatter<std::string>::format(fmt::format("Flags<{}>", swift::runtime::ir::FlagsString(flags)), ctx);
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
            case swift::runtime::ir::OperandOp::LSL:
                result.append(fmt::format("{} << {}", operand.GetLeft(), operand.GetRight()));
                break;
            case swift::runtime::ir::OperandOp::LSR:
                result.append(fmt::format("{} >> {}", operand.GetLeft(), operand.GetRight()));
                break;
            case swift::runtime::ir::OperandOp::PlusExt:
                result.append(fmt::format("{} + ({} << {})", operand.GetLeft(), operand.GetRight(), operand.GetOp().shift_ext));
                break;
            default:
                result.append(fmt::format("{}", operand.GetLeft()));
                break;
        }
        return formatter<std::string>::format(fmt::format("[{}]", result), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Params> : fmt::formatter<std::string> {
    template <typename ParamsType, typename FormatContext>
    auto format(const ParamsType& params, FormatContext& ctx) const {
        std::string result{};
        bool first{true};
        for (auto param : params) {
            if (!first) {
                result.append(", ");
            }
            result.append(fmt::format("{}", param.data));
            first = false;
        }
        return formatter<std::string>::format(fmt::format("Params[{}]", result), ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Params::View>
        : fmt::formatter<swift::runtime::ir::Params> {};
