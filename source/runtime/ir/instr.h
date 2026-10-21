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
// Compile-time argument-count check for the builder INST macros. Exact match
// against the ir.inc signature, with two documented exceptions:
//  - CallLambda: trailing host-call argument Values are optional (Lambda +
//    0..3 Values, see Block::CallHost and the SIMD helpers in the x86 decoder).
//  - Not: unary logical-not (Value) or bitwise-not (Value, Operand); both
//    backends dispatch on ArgAt(1).IsVoid() (EmitNot / RunNot).
constexpr bool ValidArgCount(OpCode op, std::size_t declared, std::size_t actual) {
    if (op == OpCode::CallLambda || op == OpCode::Not) {
        return actual >= 1 && actual <= declared;
    }
    return actual == declared;
}
}  // namespace meta

class Block;
class Inst;

template <typename T>
concept InstAllocator = requires(T allocator, Inst* inst, OpCode code) {
    { allocator.New(code) } -> std::convertible_to<Inst*>;
    { allocator.Delete(inst) };
};

#pragma pack(push, 1)
class Inst final : public SlabObject<Inst, true> {
public:
    static constexpr auto max_args = 5;
    static_assert(max_args == meta::kMaxPhysSlots, "keep in sync with ir_meta.h");
    static constexpr auto invalid_id = UINT16_MAX;
    using Values = StackVector<Value, max_args>;
    using Pseudos = StackVector<Inst*, 4>;

    ~Inst();

    template <typename... Args> void SetArgs(const Args&... args) {
        constexpr auto arg_count = sizeof...(args);
        static_assert(arg_count <= max_args);
        int index{};
        auto arg_index = [&](const Arg& arg) -> int {
            auto res = index;
            // Operand spans op+left+right; slot width comes from PhysicalSlots (ir_types.h).
            index += PhysicalSlots(arg.IsOperand() ? ArgType::Operand : ArgType::Void);
            return res;
        };
        (SetArg(arg_index(args), args), ...);
    }

    // Initial construction only: the constructor has already made every slot
    // Void, so SetArg's DestroyArg walk cannot own or unregister anything.
    // Keeping this separate from SetArgs makes the SVM_IR_FAST=0 path an exact
    // copy of the historical mutation-capable route.
    template <typename... Args> void SetArgsFresh(const Args&... args) {
        constexpr auto arg_count = sizeof...(args);
        static_assert(arg_count <= max_args);
        int index{};
        auto arg_index = [&](const Arg& arg) -> int {
            auto res = index;
            index += PhysicalSlots(arg.IsOperand() ? ArgType::Operand : ArgType::Void);
            return res;
        };
        (SetArgFresh(arg_index(args), args), ...);
    }

    template <InstAllocator Allocator, typename... Args>
    static Inst* Create(Allocator& allocator, OpCode op, const Args&... args) {
        auto inst = allocator.New(op);
        inst->SetArgs(args...);
        return inst;
    }

    template <typename... Args> static Inst* Create(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        inst->SetArgs(std::forward<const Args&>(args)...);
        return inst;
    }
    static void Validate(Inst* inst);
    [[nodiscard]] bool HasValidTypeViews() const;

    explicit Inst() = default;

    explicit Inst(OpCode code);

    Arg& ArgAt(int index);
    Arg& ArgAt(int index) const;
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
    void SetArg(int index, const Params& arg);
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
    void SetArgFresh(int index, const Params& arg);

    template <typename T>
    [[nodiscard]] T GetArg(int index) const {
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

    template <typename... Args> void SetInst(OpCode op, const Args&... args) {
        this->op_code = op;
        this->SetArgs(std::forward<const Args&>(args)...);
    }

#define INST(name, ret, ...)                                                                        \
    template <typename... Args> Inst &name(const Args&... args) {                                   \
        static_assert(meta::ValidArgCount(OpCode::name, meta::kArgs_##name.size(),                  \
                                          sizeof...(Args)),                                         \
                    #name ": argument count mismatch (see ir.inc)");                                \
        this->SetInst(OpCode::name, std::forward<const Args&>(args)...);                            \
        return *this;                                                                               \
    }
#include "ir.inc"
#undef INST

    void Use(const Value& value);
    void UnUse(const Value& value);
    [[nodiscard]] u32 GetUses(bool exclude_pseudo = true) const;
    [[nodiscard]] Values GetValues() {
        Values values{};
        ForEachValue([&](Value value) { values.push_back(value); });
        return values;
    }
    // Visit every occurrence in physical argument order, including Lambda and
    // Params values. Copies preserve each use's type view without exposing IR
    // operands to mutation or building a temporary Values collection.
    template<class Visitor> void ForEachValue(Visitor&& visit) {
        for (auto& arg : arguments) {
            if (arg.IsValue()) {
                visit(Value{arg.Get<Value>()});
            } else if (arg.IsLambda() && arg.Get<Lambda>().IsValue()) {
                visit(Value{arg.Get<Lambda>().GetValue()});
            } else if (arg.IsParams()) {
                for (auto param : arg.Get<Params>())
                    if (auto data = param.data; data.IsValue()) visit(data.value);
            }
        }
    }

    // Trivial accessors are defined inline: they dominate host compile
    // profiles when emitted out-of-line (simpleperf, 2026-10-20).
    [[nodiscard]] OpCode GetOp() const { return op_code; }
    void SetId(u16 id_) { id = id_; }
    void SetReturn(ValueType type) { ret_type = type; }
    [[nodiscard]] u16 Id() const { return id; }
    [[nodiscard]] ValueType ReturnType() const { return ret_type; }
    [[nodiscard]] bool HasValue() {
        return meta::HasValue(GetIRMetaInfo(op_code).return_type);
    }
    [[nodiscard]] bool IsPseudoOperation() const;
    [[nodiscard]] bool IsGetHostRegOperation();
    [[nodiscard]] bool IsSetHostRegOperation();
    [[nodiscard]] bool IsBitCastOperation();
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

    [[nodiscard]] u16 VirRegID() const {
        return vir_reg;
    }

    void SetVirReg(u16 slot);

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

    IntrusiveListNode list_node{};

private:
    friend class Value;
    friend class Block;

    [[nodiscard]] int PublicIndex(int logical_index) const {
        auto& info = GetIRMetaInfo(op_code);
        ASSERT(logical_index >= 0 && logical_index < (int)info.arg_types.size());
        int physical = logical_index;
        for (int i = 0; i < logical_index; i++) {
            physical += PhysicalSlots(info.arg_types[i]) - 1;
        }
        ASSERT(physical < max_args);
        return physical;
    }

    mutable std::array<Arg, max_args> arguments{};
    OpCode op_code{OpCode::Void};
    u32 num_use{};
    // Uses of this value by pseudo operations, maintained alongside
    // num_use in Use/UnUse so GetUses can exclude them in O(1)
    // instead of walking the pseudo chain (un-chaining step 3.1).
    u16 pseudo_use_count{};
    u16 id{invalid_id};
    u16 vir_reg{};
    ValueType ret_type{};
};
#pragma pack(pop)

using InstList = IntrusiveList<&Inst::list_node>;

// Process-constant bisect switch. Default on; SVM_IR_FAST=0 restores every
// construction/append/use-registration path changed by W6.
bool IRBuildFastEnabled();

// Value accessors that dereference the definition live here, where Inst
// is complete; args.h can only forward-declare it. Inlined for the same
// host-compile reason as the Inst accessors above.
inline Value Value::SetType(ValueType type) {
    ASSERT(Def());
    Def()->SetReturn(type);
    return *this;
}

inline ValueType Value::Type() const {
    ASSERT(Def());
    if (cast_type == ValueType::VOID) {
        return Def()->ReturnType();
    } else {
        return cast_type;
    }
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
