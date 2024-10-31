//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <mutex>
#include <shared_mutex>
#include "runtime/backend/jit_code.h"
#include "runtime/common/cast_utils.h"
#include "runtime/common/spin_lock.h"
#include "runtime/common/variant_util.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/terminal.h"

namespace swift::runtime::ir {

struct AddressNode {
    enum NodeType : u32 { None = 0u, Block = 1u << 0, Function = 1u << 1 };

    union {
        NonTriviallyDummy dummy_node{};
        IntrusiveMapNode map_node;
        IntrusiveListNode list_node;
    };
    Location location{0};
    u32 node_size{0};
    NodeType node_type{None};

    explicit AddressNode() {};

    explicit AddressNode(const Location& location, NodeType type = None)
            : location(location), node_type(type) {}

    bool operator<(const AddressNode& rhs) const { return location < rhs.location; }

    bool operator>(const AddressNode& rhs) const { return location > rhs.location; }

    bool operator==(const AddressNode& rhs) const { return location == rhs.location; }

    // for rbtree compare
    static NOINLINE int Compare(const AddressNode& lhs, const AddressNode& rhs) {
        if (rhs.location > lhs.location) {
            return 1;
        } else if (rhs.location < lhs.location) {
            return -1;
        } else {
            return 0;
        }
    }

    void SetEndLocation(Location end) { node_size = end.Value() - location.Value(); }

    [[nodiscard]] Location GetStartLocation() const { return location; }

    [[nodiscard]] Location GetEndLocation() const { return location + node_size; }

    [[nodiscard]] u32 GetNodeSize() const { return node_size; }

    [[nodiscard]] bool Overlap(LocationDescriptor start, LocationDescriptor end) const {
        return (GetStartLocation() <= start && GetEndLocation() >= start) ||
               (GetStartLocation() <= end && GetEndLocation() >= end);
    }
};

class Block final : public SlabObject<Block, true>,
                    public IntrusiveRefCounter<Block>,
                    public AddressNode {
public:
    using ReadLock = std::shared_lock<RwSpinLock>;
    using WriteLock = std::unique_lock<RwSpinLock>;

    explicit Block() = default;

    explicit Block(const Location& location) : AddressNode(location, AddressNode::Block) {}

    explicit Block(u32 id, Location location) : id(id), AddressNode(location, AddressNode::Block) {}

    [[nodiscard]] Terminal GetTerminal() const;
    void SetTerminal(Terminal term);
    [[nodiscard]] bool HasTerminal() const;

    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>
    Inst* AppendInst(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        inst->SetArgs(std::forward<const Args&>(args)...);
        inst->SetReturn(RetType::TYPE);
        AppendInst(inst);
        return inst;
    }

    void AppendInst(Inst* inst);
    void InsertBefore(Inst* inst, Inst* before);
    void InsertAfter(Inst* inst, Inst* after);
    void RemoveInst(Inst* inst);
    void DestroyInst(Inst* inst);
    void DestroyInstrs();
    void ReIdInstr();

    [[nodiscard]] InstList& GetInstList();
    [[nodiscard]] InstList& GetInstList() const;
    [[nodiscard]] InstList::iterator GetBeginInst();
    [[nodiscard]] bool IsEmptyBlock();
    [[nodiscard]] bool IsJitCached();

    [[nodiscard]] ReadLock LockRead() { return std::shared_lock{block_lock}; }

    [[nodiscard]] WriteLock LockWrite() { return std::unique_lock{block_lock}; }

    [[nodiscard]] u32 GetVStackSize() const { return v_stack * 8; }

    [[nodiscard]] backend::JitCache& GetJitCache() { return jit_cache; }

    [[nodiscard]] u32 GetId() const { return id; }

    [[nodiscard]] u32 GetDispatchIndex() const { return dispatch_index; }

    [[nodiscard]] u32 MaxInstrId() const { return max_instr_id; }

#define INST(name, ret, ...)                                                                       \
    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>                    \
    ret name(const Args&... args) {                                                                \
        auto inst = AppendInst(OpCode::name, std::forward<const Args&>(args)...);                  \
        inst->SetReturn(RetType::TYPE);                                                            \
        return ret{inst};                                                                          \
    }
#include "ir.inc"
#undef INST

    template <typename Lambda, typename... Args> Value CallHost(Lambda l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(FptrCast(l), std::forward<const Args&>(args)...);
    }

    [[nodiscard]] std::string ToString() const;

    ~Block();

private:
    union {
        u32 id{};
        u32 dispatch_index;
    };
    mutable InstList inst_list{};
    Terminal block_term{};
    RwSpinLock block_lock{};
    u16 max_instr_id{};
    u16 v_stack{};
    backend::JitCache jit_cache;
};

using BlockList = IntrusiveList<&Block::list_node>;
using BlockMap = IntrusiveMap<&Block::map_node>;

}  // namespace swift::runtime::ir

template <> struct fmt::formatter<swift::runtime::ir::Terminal> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(swift::runtime::ir::Terminal term, FormatContext& ctx) const {
        using namespace swift::runtime::ir;
        auto result = swift::runtime::VisitVariant<std::string>(term, [](auto x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            std::string content{};
            if constexpr (std::is_same_v<T, terminal::LinkBlock>) {
                content.append(fmt::format("  Link Block 0x{:x}\n", x.next.Value()));
            } else if constexpr (std::is_same_v<T, terminal::LinkBlockFast>) {
                content.append(fmt::format("  LinkFast Block 0x{:x}\n", x.next.Value()));
            } else if constexpr (std::is_same_v<T, terminal::ReturnToDispatch>) {
                content.append("  ReturnToDispatch\n");
            } else if constexpr (std::is_same_v<T, terminal::ReturnToHost>) {
                content.append("  ReturnToHost\n");
            } else if constexpr (std::is_same_v<T, terminal::PopRSBHint>) {
                content.append("  PopRSBHint\n");
            } else if constexpr (std::is_same_v<T, terminal::If>) {
                content.append(fmt::format("  If True({}):\n", (const Value&)x.cond));
                content.append(fmt::format("  {}", x.then_));
                content.append("  Else:\n");
                content.append(fmt::format("  {}", x.else_));
            } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                content.append(fmt::format("  If Cond({}):\n", (const Value&)x.cond));
                content.append(fmt::format("  {}", x.then_));
                content.append("  Else:\n");
                content.append(fmt::format("  {}", x.else_));
            } else {
                return {};
            }
            return content;
        });
        return formatter<std::string>::format(result, ctx);
    }
};

template <> struct fmt::formatter<swift::runtime::ir::Block> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(const swift::runtime::ir::Block& block, FormatContext& ctx) const {
        std::string block_content{fmt::format("Basic Block ${}, Location: 0x{:x}: \n",
                                              block.GetId(),
                                              block.GetStartLocation().Value())};
        for (auto& instr : block.GetInstList()) {
            block_content.append(fmt::format("  {}\n", instr));
        }
        block_content.append(fmt::format("{}", block.GetTerminal()));
        return formatter<std::string>::format(block_content, ctx);
    }
};