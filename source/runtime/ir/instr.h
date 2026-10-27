//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <array>
#include "args.h"
#include "fmt/format.h"
#include "runtime/common/logging.h"
#include "runtime/common/slab_alloc.h"
#include "runtime/ir/ir_meta.h"
#include "runtime/ir/opcodes.h"

namespace swift::runtime::ir {
namespace meta {
// Project C++ construction arguments into the same physical kinds used by
// ir.inc. Operand::Op is a physical header, never a complete logical Operand.
template<class T> inline constexpr ArgType ArgumentKind = [] {
    using A = std::remove_cvref_t<T>;
    if constexpr (std::is_base_of_v<ir::Value, A>) return ArgType::Value;
    else if constexpr (std::is_same_v<A, ir::Imm>) return ArgType::Imm;
    else if constexpr (std::is_same_v<A, ir::Uniform>) return ArgType::Uniform;
    else if constexpr (std::is_same_v<A, ir::Local>) return ArgType::Local;
    else if constexpr (std::is_same_v<A, ir::Cond>) return ArgType::Cond;
    else if constexpr (std::is_same_v<A, ir::Flags>) return ArgType::Flags;
    else if constexpr (std::is_same_v<A, ir::Operand>) return ArgType::Operand;
    else if constexpr (std::is_same_v<A, ir::Lambda>) return ArgType::Lambda;
    else if constexpr (std::is_same_v<A, ir::Params>) return ArgType::Params;
    else return ArgType::Void;
}();

template<class... Args> inline constexpr std::size_t ArgumentSlots =
        (std::size_t{0} + ... + PhysicalSlots(ArgumentKind<Args>));

// Void is allowed by raw setters for incomplete/imported IR. No complete
// instruction signature accepts it as a logical construction argument.
template<class... Args> concept ArgumentStorage =
        ((ArgumentKind<Args> != ArgType::Void ||
          std::is_same_v<std::remove_cvref_t<Args>, ir::Void>) && ...) &&
        ArgumentSlots<Args...> <= kMaxPhysSlots;

// Signature kinds do not convey ownership. A Params construction argument
// transfers its sole chain; lvalues and const owners cannot be consumed.
template<class... Args> concept OwnershipConstraint =
        ((!std::is_same_v<std::remove_cvref_t<Args>, ir::Params> ||
          (!std::is_lvalue_reference_v<Args> &&
           !std::is_const_v<std::remove_reference_t<Args>> &&
           !std::is_volatile_v<std::remove_reference_t<Args>>)) && ...);

template<class... Args> concept ConstructionArguments =
        ArgumentStorage<Args...> && OwnershipConstraint<Args...>;

// Normalize ordinary borrows before instantiating the large creation bodies.
// Params retains its consuming rvalue category across the same boundary.
template<class T> constexpr decltype(auto) ForwardConstructionArgument(T&& argument) {
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, ir::Params>)
        return std::forward<T>(argument);
    else
        return std::as_const(argument);
}

template<class... Args>
constexpr bool ValidArgumentPack(std::span<const ArgSignature> signature) {
    if constexpr (!ArgumentStorage<Args...>) return false;
    else {
        constexpr std::array<ArgType, sizeof...(Args)> kinds{ArgumentKind<Args>...};
        return AcceptsArguments(signature, kinds);
    }
}

// A construction opcode proves only the logical C++ argument kinds/count and
// physical capacity. It carries no value/type/SSA proof and is never stored in
// the IR. Binding it to normalized kinds lets named and dynamic operations
// share the same allocation body without checking a known signature again.
template<ArgType... Kinds> class ConstructionOp;
template<class... Args> using OperationFor = ConstructionOp<ArgumentKind<Args>...>;

template<ArgType... Kinds> class ConstructionOp {
public:
    [[nodiscard]] constexpr OpCode Code() const { return code; }

private:
    constexpr explicit ConstructionOp(OpCode code) : code(code) {}

    template<class... Args> requires ArgumentStorage<Args...>
    friend OperationFor<Args...> CheckArgumentPack(OpCode);

    template<OpCode op, class... Args>
        requires(ValidArgumentPack<Args...>(InstructionSignature<op>::arguments))
    friend constexpr OperationFor<Args...> KnownOperation();

    OpCode code;
};

template<class... Args> requires ArgumentStorage<Args...>
OperationFor<Args...> CheckArgumentPack(OpCode op) {
    const auto& info = GetIRMetaInfo(op);
    if (op == OpCode::Void || op >= OpCode::BASE_COUNT ||
        !ValidArgumentPack<Args...>(info.arguments)) detail::InvalidIRArguments();
    return OperationFor<Args...>{op};
}

template<OpCode op, class... Args>
    requires(ValidArgumentPack<Args...>(InstructionSignature<op>::arguments))
constexpr OperationFor<Args...> KnownOperation() {
    return OperationFor<Args...>{op};
}

inline ValueType Arg0ResultType() { detail::MissingIRResultType(); }

template <typename First, typename... Rest>
ValueType Arg0ResultType(const First& first, const Rest&...) {
    ValueType type = ValueType::VOID;
    if constexpr (std::is_base_of_v<ir::Value, First>) {
        if (first.Defined()) type = first.Type();
    } else if constexpr (std::is_same_v<First, ir::Imm> || std::is_same_v<First, ir::Uniform>) {
        type = first.GetType();
    } else if constexpr (std::is_same_v<First, ir::Local>) {
        type = first.type;
    }
    if (type == ValueType::VOID) detail::MissingIRResultType();
    return type;
}

// The C++ void marker means no hint. TypedValue<VOID> and ValueType::VOID
// instead declare a real no-result instruction; neither triggers inference.
template <ResultTypePolicy policy, ValueType fixed, typename Hint = void, typename... Args>
ValueType ResolveResultType(const Args&... args) {
    if constexpr (!std::is_void_v<Hint>) {
        return Hint::TYPE;
    } else if constexpr (policy == ResultTypePolicy::Fixed || policy == ResultTypePolicy::Void) {
        return fixed;
    } else if constexpr (policy == ResultTypePolicy::Arg0) {
        return Arg0ResultType(args...);
    } else {
        static_assert(HasDefaultResultType<policy>, "this instruction requires an explicit result type");
    }
}

template <typename Hint = void, typename... Args>
ValueType ResolveResultType(OpCode op, const Args&... args) {
    if constexpr (!std::is_void_v<Hint>) {
        return Hint::TYPE;
    } else {
        const auto& info = GetIRMetaInfo(op);
        switch (info.result_policy) {
            case ResultTypePolicy::Void:
            case ResultTypePolicy::Fixed: return info.result_type;
            case ResultTypePolicy::Arg0: return Arg0ResultType(args...);
            case ResultTypePolicy::Explicit:
            case ResultTypePolicy::Call: detail::MissingIRResultType();
        }
        detail::MissingIRResultType();
    }
}
}  // namespace meta

class Block;

#pragma pack(push, 1)
class Inst final : public SlabObject<Inst, true> {
public:
    static constexpr auto max_args = 5;
    static_assert(max_args == meta::kMaxPhysSlots, "keep in sync with ir_meta.h");
    static constexpr u8 params_arg_index = max_args;
    static constexpr auto invalid_id = UINT16_MAX;
    using Values = StackVector<Value, max_args>;
    using Pseudos = StackVector<const Inst*, 4>;

    ~Inst();

    template <typename... Args> requires meta::ConstructionArguments<Args...>
    void SetArgs(Args&&... args) {
        int index{};
        const auto set = [&]<class T>(T&& arg) {
            const auto slot = index;
            index += PhysicalSlots(meta::ArgumentKind<T>);
            SetArg(slot, std::forward<T>(arg));
        };
        (set(std::forward<Args>(args)), ...);
    }

    // Initial construction only: the constructor has already made every slot
    // Void, so SetArg's DestroyArg walk cannot own or unregister anything.
    // The slow mode exercises the mutation-capable registration route. Both
    // routes receive a result type that was fixed before argument writes.
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    void SetArgsFresh(Args&&... args) {
        int index{};
        const auto set = [&]<class T>(T&& arg) {
            const auto slot = index;
            index += PhysicalSlots(meta::ArgumentKind<T>);
            SetArgFresh(slot, std::forward<T>(arg));
        };
        (set(std::forward<Args>(args)), ...);
    }

    static void Validate(const Inst* inst);
    // Basic presence/range validity; opcode-specific widths and the explicit
    // low-level result contract are separate from construction defaults.
    [[nodiscard]] bool HasValidResultType() const;
    [[nodiscard]] bool HasValidArguments() const;
    [[nodiscard]] IntegerPredicateKind GetIntegerPredicateKind() const;
    // A materialized primitive or literal is a complete scalar 0 or 1. Aliases,
    // guest homes and deferred native conditions need their own contracts.
    [[nodiscard]] bool HasBooleanResult() const;

    explicit Inst() = default;

    explicit Inst(OpCode code);
    Inst(OpCode code, ValueType type);

    // Queries never lend a mutable operand slot. Construction and rewrites
    // use SetArg so use counts and rewrite notifications remain authoritative.
    const Arg& ArgAt(int index) const;
    void SetArg(int index, const Void& arg);
    void SetArg(int index, const Value& arg);
    void SetArg(int index, const Imm& arg);
    void SetArg(int index, const Cond& arg);
    void SetArg(int index, const Flags& arg);
    void SetArg(int index, const Operand::Op& arg);
    void SetArg(int index, const Local& arg);
    void SetArg(int index, const Uniform& arg);
    void SetArg(int index, const Lambda& arg);
    void SetArg(int index, const Operand& arg);
    void SetArg(int index, Params&& arg);
    void SetArgFresh(int index, const Void& arg);
    void SetArgFresh(int index, const Value& arg);
    void SetArgFresh(int index, const Imm& arg);
    void SetArgFresh(int index, const Cond& arg);
    void SetArgFresh(int index, const Flags& arg);
    void SetArgFresh(int index, const Operand::Op& arg);
    void SetArgFresh(int index, const Local& arg);
    void SetArgFresh(int index, const Uniform& arg);
    void SetArgFresh(int index, const Lambda& arg);
    void SetArgFresh(int index, const Operand& arg);
    void SetArgFresh(int index, Params&& arg);

    template <typename T>
    [[nodiscard]] auto GetArg(int index) const {
        index = PublicIndex(index);
        if constexpr (std::is_same<T, Operand>::value) {
            ASSERT(arguments[index].IsOperand());
            Operand operand{};
            operand.op = arguments[index++].Get<Operand::Op>();
            operand.left = arguments[index++].ToDataClass();
            operand.right = arguments[index++].ToDataClass();
            return operand;
        } else {
            return arguments[index].Get<T>();
        }
    }

    template <typename... Args>
        requires meta::ConstructionArguments<Args...>
    void SetInst(OpCode op, Args&&... args) {
        CheckMutable();
        SetInst(meta::CheckArgumentPack<Args...>(op), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires meta::ConstructionArguments<Args...>
    void SetInst(OpCode op, ValueType type, Args&&... args) {
        CheckMutable();
        SetInst(meta::CheckArgumentPack<Args...>(op), type, std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires meta::ConstructionArguments<Args...>
    void SetInst(meta::OperationFor<Args...> operation, Args&&... args) {
        CheckMutable();
        // Retain value views and typed label identities. Ordinary void
        // operations cannot inherit the old definition's result type.
        const auto type =
                ret_type == ValueType::VOID ||
                                (GetIRMetaInfo(operation.Code()).return_type == ArgType::Void &&
                                 !meta::IsControlToken(operation.Code()))
                        ? meta::ResolveResultType(operation.Code(), args...)
                        : ret_type;
        SetInst(operation, type, std::forward<Args>(args)...);
    }

    template <typename... Args> requires meta::ConstructionArguments<Args...>
    void SetInst(meta::OperationFor<Args...> operation, ValueType type, Args&&... args) {
        CheckMutable();
        this->op_code = operation.Code();
        ret_type = type;
        SetArgs(std::forward<Args>(args)...);
    }

#define INST(name, ret, policy, ...)                                                                    \
    template <typename... Args> requires(meta::ConstructionArguments<Args...> &&                        \
             meta::ValidArgumentPack<Args...>(meta::kArgs_##name))                                      \
    Inst &name(Args&&... args) {                                                                        \
        this->SetInst(meta::KnownOperation<OpCode::name, Args...>(), std::forward<Args>(args)...);      \
        return *this;                                                                                   \
    }
#include "ir.inc"
#undef INST

    [[nodiscard]] u32 GetUses(bool exclude_pseudo = true) const;
    [[nodiscard]] Values GetValues() const {
        Values values{};
        ForEachValue([&](Value value) { values.push_back(value); });
        return values;
    }
    // Visit every occurrence in physical argument order, including Lambda and
    // Params values. Copies preserve each use's type view without exposing IR
    // operands to mutation or building a temporary Values collection.
    template<class Visitor> void ForEachValue(Visitor&& visit) const {
        VisitValueArguments<false>([&](Value value, u8) { visit(value); });
    }

    // Like ForEachValue, with the physical slot needed by HIR use records.
    // Params entries use params_arg_index rather than a direct operand slot.
    template<class Visitor> void ForEachValueUse(Visitor&& visit) const {
        VisitValueArguments<false>(std::forward<Visitor>(visit));
    }

    // Check the complete schema and type views during the visit used by final
    // facts. Invalid data views do not reach the callback; no snapshot or
    // separate signature traversal is needed.
    template<class Visitor> bool CheckArguments(Visitor&& visit) const {
        return VisitValueArguments<true>([&](Value value, u8) { visit(value); });
    }

private:
    // Called only after the signature and nested value views are valid.
    // Reads a fixed set of operands; it never enumerates the uses again.
    [[nodiscard]] bool HasValidScalarArguments() const;

    template<bool check, class Visitor> bool VisitValueArguments(Visitor&& visit) const {
        bool valid = true;
        const IRMeta* info{};
        u8 logical{}, operand_slots{};
        bool ended = false;
        if constexpr (check) {
            if (op_code == OpCode::Void || op_code >= OpCode::BASE_COUNT) return false;
            info = &GetIRMetaInfo(op_code);
        }
        const auto value = [&](Value value, u8 index) {
            if constexpr (check) {
                const bool readable = op_code == OpCode::BindLabel
                        ? value.HasValidTypeView() : value.HasValidDataView();
                if (!readable) {
                    valid = false;
                    return;
                }
            }
            visit(value, index);
        };
        for (u8 index = 0; index < max_args; ++index) {
            const auto& arg = arguments[index];
            if constexpr (check) {
                if (operand_slots) {
                    const bool right = operand_slots == 1;
                    --operand_slots;
                    if (!arg.IsValue() && !arg.IsImm() && !(right && arg.IsVoid())) return false;
                } else if (logical < info->arguments.size()) {
                    const auto signature = info->arguments[logical];
                    if (arg.IsVoid() && IsOptional(signature)) {
                        ended = true;
                    } else {
                        if (ended || !AcceptsArgument(signature, arg.GetType())) return false;
                        ++logical;
                        operand_slots = PhysicalSlots(signature) - 1;
                    }
                } else if (!arg.IsVoid()) {
                    return false;
                }
                if (arg.IsImm() && !IsValidValueType(arg.Get<Imm>().GetType())) valid = false;
                if (arg.GetType() == ArgType::Uniform && !IsValidValueType(arg.Get<Uniform>().GetType())) valid = false;
                if (arg.GetType() == ArgType::Local && !IsValidValueType(arg.Get<Local>().type)) valid = false;
                if (arg.GetType() == ArgType::Cond && !IsValidCondition(arg.Get<Cond>())) valid = false;
                if (arg.GetType() == ArgType::Flags &&
                    (static_cast<u64>(arg.Get<Flags>()) &
                     ~static_cast<u64>(Flags::All | Flags::Positive))) valid = false;
                if (arg.IsOperand()) {
                    const auto op = arg.Get<Operand::Op>();
                    switch (op.type) {
                        case OperandOp::None:
                        case OperandOp::Plus:
                        case OperandOp::LSL:
                        case OperandOp::LSR: break;
                        case OperandOp::PlusExt: valid &= op.shift_ext < 64; break;
                        default: valid = false;
                    }
                }
            }
            if (arg.IsValue()) {
                value(arg.Get<Value>(), index);
            } else if (arg.IsLambda()) {
                const auto& lambda = arg.Get<Lambda>();
                if (lambda.IsValue()) value(lambda.GetValue(), index);
                else if constexpr (check) {
                    if (!lambda.IsImm() || !IsValidValueType(lambda.GetImm().GetType())) valid = false;
                }
            } else if (arg.IsParams()) {
                for (auto param : arg.Get<Params>()) {
                    const auto data = param.data;
                    if (data.IsValue()) value(data.value, params_arg_index);
                    else if constexpr (check) {
                        if (!data.IsImm() || !IsValidValueType(data.imm.GetType())) valid = false;
                    }
                }
            }
        }
        if constexpr (check) {
            if (operand_slots || (logical < info->arguments.size() &&
                                  !IsOptional(info->arguments[logical]))) return false;
            // A state store is a byte range. Signedness may differ, but its
            // source view must have the width declared by that range.
            if (op_code == OpCode::StoreUniform) {
                if (!valid || arguments[0].GetType() != ArgType::Uniform || !arguments[1].IsValue()) return false;
                return GetValueSizeByte(arguments[0].Get<Uniform>().GetType()) ==
                       GetValueSizeByte(arguments[1].Get<Value>().Type());
            }
            return valid && HasValidScalarArguments();
        }
        return valid;
    }

public:
    // Trivial accessors are defined inline: they dominate host compile
    // profiles when emitted out-of-line (simpleperf, 2026-10-20).
    [[nodiscard]] OpCode GetOp() const { return op_code; }
    void SetId(u16 id_) {
        CheckMutable();
        id = id_;
    }
    void SetReturn(ValueType type) {
        CheckMutable();
        ret_type = type;
    }
    [[nodiscard]] const Block* GetBlock() const { return owner ? owner->block : nullptr; }
    [[nodiscard]] u16 Id() const { return id; }
    [[nodiscard]] ValueType ReturnType() const { return ret_type; }
    [[nodiscard]] bool HasValue() const {
        return meta::HasValue(GetIRMetaInfo(op_code).return_type);
    }
    [[nodiscard]] bool IsPseudoOperation() const;
    [[nodiscard]] bool IsGetHostRegOperation() const;
    [[nodiscard]] bool IsSetHostRegOperation() const;
    [[nodiscard]] bool IsBitCastOperation() const;
    [[nodiscard]] IREffect GetEffects() const;
    [[nodiscard]] bool HasSideEffects() const;

    // Pseudos are ordinary instructions in the owner's block whose arg 0 is
    // the owner's value; there is no intrusive owner->pseudo chain. Find
    // them with CollectPseudoOperations (block.h) or a use index.
    [[nodiscard]] bool HasPseudoOperations() const { return pseudo_use_count != 0 && !IsPseudoOperation(); }
    void DestroyArg(u8 arg_idx);
    void DestroyArgs();
    // Teardown for "everything that could read this instruction is going away
    // too" -- Block::DestroyInstrs and nothing else.
    //
    // DestroyArgs keeps the IR's *bookkeeping* consistent: each argument value
    // is unregistered from its defining instruction (`def->num_use--`). That
    // is a dependent load and
    // store into a different instruction for every operand, and it is the
    // expensive half of teardown: freeing a compiled function's IR costs
    // 18.1 ns per instruction with DestroyArgs and 10.4 ns with this
    // (func_tests, 32 411 instructions, median of 13 runs).
    //
    // It is also pure waste when the whole block or function is being freed:
    // nothing will read a use count again, and the only thing an argument
    // actually *owns* is a Params list. So this frees that and blanks the
    // slots (which is what makes the later ~Inst -> DestroyArgs a no-op) and
    // touches no other instruction at all. Not touching another instruction is
    // also what makes bulk teardown order-independent: the block-at-a-time
    // walk in ~Function used to run UnUse against instructions in blocks it
    // had already freed.
    void ReleaseArgs();
    void Reset();

    // Instruction storage: a per-thread bump arena with a free list.
    //
    // `SlabObject<Inst, true>` promises a slab, but nothing outside
    // source/tests/main_case.cpp ever calls Inst::InitializeSlabHeap, so in
    // svm_translator_linux (and in every embedder) the slab head is always
    // null and SlabObject::TryAllocate/TryFree degrade to basic malloc/free,
    // once per IR instruction -- the single largest line item in the decode
    // phase. Measured with a marginal-cost check (N extra allocations per
    // appended instruction, median of 11 runs): `new Inst` + `delete` costs
    // 25 ns, of which raw malloc+free is 15.8 ns, against 0.73 ns for the
    // bump-allocated HIRValue pool next to it. On func_tests that is ~0.5 ms
    // of a 2.84 ms decode phase.
    //
    // Turning the slab on instead is not the fix: SlabHeap::Initialize builds
    // its free list with one seq_cst compare-exchange per object (a million of
    // them at main_case.cpp's size) and SlabAllocator::Allocate/Free are a
    // seq_cst CAS loop each, i.e. the same order of magnitude as malloc.
    //
    // A recycling cache alone is not enough, and measuring that is what
    // produced this shape: at the time, the default (function) mode never
    // called Block::DestroyInstrs, so IR instructions were retained for the
    // life of the compiled unit and a pure freelist stayed empty forever -- it
    // measured +0.5..+1.0% on translate_ns, i.e. a small loss. Bump allocation
    // is what makes the *first* allocation cheap, and the freelist on top is
    // what makes a mode that does destroy its instructions reuse them: -5.9%
    // translate_ns in block mode. TranslateIR(HIRFunction*) now releases its
    // IR too, so the freelist is live on the default path as well and the
    // arena stops growing after the first unit.
    //
    // A freelist entry stores its successor in the object's own storage, so
    // the arena costs nothing beyond the chunks themselves, and chunks are
    // never released: an Inst can be freed by a thread other than the one that
    // allocated it, so no chunk has a single owning thread.
    void* operator new(size_t sz);
    void operator delete(void* p);

private:
    IntrusiveListNode list_node{};

public:
    using List = IntrusiveList<&Inst::list_node>;

private:
    friend class Value;
    friend class Block;

    // One state per owning Block: sealing does not walk or copy its instructions.
    struct Owner {
        Block* block;
        bool sealed{};
        bool compiling{};
        u16 hir_order{UINT16_MAX};
    };
    void CheckMutable() const { ASSERT(!owner || !owner->sealed); }
    static void CheckUse(const Value& value) {
        ASSERT(value.Def());
        value.Def()->CheckMutable();
    }
    void Use(const Value& value);
    void UnUse(const Value& value);

    [[nodiscard]] int PublicIndex(int logical_index) const {
        auto& info = GetIRMetaInfo(op_code);
        ASSERT(logical_index >= 0 && logical_index < (int)info.arguments.size());
        int physical = logical_index;
        for (int i = 0; i < logical_index; i++) {
            physical += PhysicalSlots(info.arguments[i]) - 1;
        }
        ASSERT(physical < max_args);
        return physical;
    }

    std::array<Arg, max_args> arguments{};
    OpCode op_code{OpCode::Void};
    mutable u32 num_use{};
    // Uses of this value by pseudo operations, maintained alongside
    // num_use in Use/UnUse so GetUses can exclude them in O(1)
    // instead of walking the pseudo chain (un-chaining step 3.1).
    mutable u16 pseudo_use_count{};
    u16 id{invalid_id};
    ValueType ret_type{};
    mutable const Owner* owner{};
};
#pragma pack(pop)

using InstList = Inst::List;

// Process-constant registration switch. The slow mode uses mutation-capable
// argument/link bookkeeping; both modes share type and sealed-IR contracts.
bool IRBuildFastEnabled();

// Value accessors that dereference the definition live here, where Inst
// is complete; args.h can only forward-declare it. Inlined for the same
// host-compile reason as the Inst accessors above.
inline ValueType Value::Type() const {
    ASSERT(Def());
    if (cast_type == ValueType::VOID) {
        return Def()->ReturnType();
    } else {
        return cast_type;
    }
}

inline bool Value::HasValidTypeView() const {
    return Defined() && IsValidValueType(Def()->ReturnType()) && IsValidValueType(Type());
}

inline bool Value::HasValidDataView() const {
    if (!HasValidTypeView() || !Def()->HasValue()) return false;
    // These values name branch bindings, not computed data. BindLabel uses
    // the type-view predicate directly and validates its control relation.
    return !meta::IsControlToken(Def()->GetOp());
}

inline u16 Value::Id() const { return Def()->Id(); }

}  // namespace swift::runtime::ir

template <> struct fmt::formatter<swift::runtime::ir::OpCode> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(swift::runtime::ir::OpCode op, FormatContext& ctx) const {
        return formatter<std::string>::format(swift::runtime::ir::GetIRMetaInfo(op).name, ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Inst> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Inst &inst, FormatContext& ctx) const {
        std::string instr_args{};
        bool continues = true;
        for (int i = 0; continues && i < swift::runtime::ir::Inst::max_args; i++) {
            std::string arg_str{};
            if (i > 0) {
                arg_str.append(", ");
            }
            switch (inst.ArgAt(i).GetType()) {
            case swift::runtime::ir::ArgType::Value: {
                auto value = inst.GetArg<swift::runtime::ir::Value>(i);
                arg_str.append(fmt::format("{}", value));
                break;
            }
            case swift::runtime::ir::ArgType::Imm: {
                auto imm = inst.GetArg<swift::runtime::ir::Imm>(i);
                arg_str.append(fmt::format("{}", imm));
                break;
            }
            case swift::runtime::ir::ArgType::Cond: {
                auto cond = inst.GetArg<swift::runtime::ir::Cond>(i);
                arg_str.append(fmt::format("{}", cond));
                break;
            }
            case swift::runtime::ir::ArgType::Flags: {
                auto flags = inst.GetArg<swift::runtime::ir::Flags>(i);
                arg_str.append(fmt::format("{}", flags));
                break;
            }
            case swift::runtime::ir::ArgType::Lambda: {
                auto lambda = inst.GetArg<swift::runtime::ir::Lambda>(i);
                arg_str.append(fmt::format("{}", lambda));
                break;
            }
            case swift::runtime::ir::ArgType::Uniform: {
                auto uni = inst.GetArg<swift::runtime::ir::Uniform>(i);
                arg_str.append(fmt::format("{}", uni));
                break;
            }
            case swift::runtime::ir::ArgType::Local: {
                auto local = inst.GetArg<swift::runtime::ir::Local>(i);
                arg_str.append(fmt::format("{}", local));
                break;
            }
            case swift::runtime::ir::ArgType::Params: {
                auto params = inst.GetArg<swift::runtime::ir::Params>(i);
                arg_str.append(fmt::format("{}", params));
                break;
            }
            default:
                continues = false;
                break;
            }
            if (continues) {
                instr_args.append(arg_str);
            }
        }
        auto result{fmt::format("@{:<{}} {:<{}} = {} {}", inst.Id(), 2, inst.ReturnType(), 4, inst.GetOp(), instr_args)};
        return formatter<std::string>::format(result, ctx);
    }
};
