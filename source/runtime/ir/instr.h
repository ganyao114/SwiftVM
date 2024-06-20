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
    static constexpr auto invalid_id = UINT16_MAX;
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

    template <typename T>
    [[nodiscard]] T GetArg(int index) const {
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

    [[nodiscard]] OpCode GetOp() const;
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

    void SetVirReg(u16 slot);

    IntrusiveListNode list_node{};

private:
    friend class Value;
    friend class Block;

    Inst* next_pseudo_inst{};
    mutable std::array<Arg, max_args> arguments{};
    OpCode op_code{OpCode::Void};
    u8 num_use{};
    u16 id{invalid_id};
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