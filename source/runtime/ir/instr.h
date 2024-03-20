//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <array>
#include "args.h"
#include "fmt/format.h"
#include "runtime/common/logging.h"
#include "runtime/common/slab_alloc.h"
#include "runtime/ir/opcodes.h"

namespace swift::runtime::ir {

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
    static constexpr auto max_args = 4;
    using Values = StackVector<Value, max_args>;
    using Pseudos = StackVector<Inst*, 8>;

    ~Inst();

    template <typename... Args> void SetArgs(const Args&... args) {
        constexpr auto arg_count = sizeof...(args);
        static_assert(arg_count <= max_args);
        int index{};
        auto arg_index = [&](const Arg& arg) -> int {
            auto res = index;
            if (arg.IsOperand()) {
                index += 3;
            } else {
                index++;
            }
            return res;
        };
        (SetArg(arg_index(args), args), ...);
    }

    template <InstAllocator Allocator, typename... Args>
    static Inst* Create(Allocator& allocator, OpCode op, const Args&... args) {
        auto inst = allocator.New(op);
        inst->SetArgs(args...);
        return inst;
    }

    template <typename... Args> static Inst* Create(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        inst->SetArgs(args...);
        return inst;
    }
    static void Validate(Inst* inst);

    explicit Inst() = default;

    explicit Inst(OpCode code);

    Arg& ArgAt(int index);
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

    template <typename T>
    [[nodiscard]] T GetArg(int index) {
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

    void Use(const Value& value);
    void UnUse(const Value& value);
    Values GetValues();

    [[nodiscard]] OpCode GetOp();
    void SetId(u16 id);
    void SetReturn(ValueType type);
    [[nodiscard]] u16 Id() const;
    [[nodiscard]] ValueType ReturnType() const;
    bool HasValue();
    bool IsPseudoOperation();

    [[nodiscard]] Inst* GetPseudoOperation(OpCode code);
    [[nodiscard]] Inst::Pseudos GetPseudoOperations();
    void DestroyArg(u8 arg_idx);

    [[nodiscard]] u16 VirRegID() const {
        return vir_reg;
    }

    IntrusiveListNode list_node{};

private:
    friend class Value;
    friend class Block;

    Inst* next_pseudo_inst{};
    std::array<Arg, max_args> arguments{};
    OpCode op_code{OpCode::Void};
    u8 num_use{};
    u16 id{};
    u16 vir_reg{};
    ValueType ret_type{};
};
#pragma pack(pop)

using InstList = IntrusiveList<&Inst::list_node>;

}  // namespace swift::runtime::ir

template <> struct fmt::formatter<swift::runtime::ir::OpCode> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(swift::runtime::ir::OpCode op, FormatContext& ctx) const {
        return formatter<std::string>::format(swift::runtime::ir::GetIRMetaInfo(op).name, ctx);
    }
};