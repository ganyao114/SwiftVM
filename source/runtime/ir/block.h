//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <shared_mutex>
#include "runtime/backend/code_lifetime.h"
#include "runtime/backend/jit_code.h"
#include "runtime/common/cast_utils.h"
#include "runtime/common/perf_stats.h"
#include "runtime/common/spin_lock.h"
#include "runtime/common/variant_util.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/terminal.h"

namespace swift::runtime::compile {
class CompileUnit;
class RewriteContext;
}  // namespace swift::runtime::compile

namespace swift::runtime::backend {
class Module;
}

namespace swift::runtime::ir {
class Function;
struct AddressNode {
    enum NodeType : u32 { None = 0u, Block = 1u << 0, Function = 1u << 1 };

private:
    friend class backend::Module;
    friend class HIRTestSupport;
    union {
        NonTriviallyDummy dummy_node{};
        IntrusiveMapNode map_node;
        IntrusiveListNode list_node;
    };
    Location location{0};
    u32 node_size{0};
    const NodeType node_type{None};

    void CheckRangeMutable() const;

public:
    using List = IntrusiveList<&AddressNode::list_node>;
    using Map = IntrusiveMap<&AddressNode::map_node>;
    // Assigned before installation publishes the node; immutable thereafter.
    std::shared_ptr<backend::CodeLifetime> code_lifetime;

    explicit AddressNode() {};

    explicit AddressNode(const Location& location) : location(location) {}
    AddressNode(const AddressNode&) = delete;
    AddressNode& operator=(const AddressNode&) = delete;

protected:
    explicit AddressNode(const Location& location, NodeType type)
            : location(location), node_type(type) {}

public:
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

    void SetEndLocation(Location end) {
        CheckRangeMutable();
        node_size = end.Value() - location.Value();
    }

    [[nodiscard]] NodeType GetNodeType() const { return node_type; }

    [[nodiscard]] Location GetStartLocation() const { return location; }

    [[nodiscard]] Location GetEndLocation() const { return location + node_size; }

    [[nodiscard]] u32 GetNodeSize() const { return node_size; }

    [[nodiscard]] bool Overlap(LocationDescriptor start, LocationDescriptor end) const {
        return (GetStartLocation() <= start && GetEndLocation() >= start) ||
               (GetStartLocation() <= end && GetEndLocation() >= end);
    }
};

struct GuestCodeDependency {
    Location start{};
    Location end{};
};

// 单元内单块自环的瞬态发码计划。普通入口执行到 prefix_end，自回边落到
// 其后；anchors 存储由 CompileUnit 持有，不序列化也不跨单元共享。
struct LoopHoistMetadata {
    const Inst* prefix_end{};
    std::span<const Inst* const> anchors{};
    u16 gpr_count{};
    u16 const_count{};
};

class Block final : public SlabObject<Block, true>,
                    public IntrusiveRefCounter<Block>,
                    public AddressNode {
public:
    using ReadLock = std::shared_lock<RwSpinLock>;
    using WriteLock = std::unique_lock<RwSpinLock>;

    explicit Block() : AddressNode(Location{0}, AddressNode::Block) {}

    explicit Block(const Location& location) : AddressNode(location, AddressNode::Block) {}

    explicit Block(u32 id, Location location) : id(id), AddressNode(location, AddressNode::Block) {}

    // Borrowed until SetTerminal or Block destruction. A traversal must finish
    // before replacing the terminal; callers needing a snapshot copy it.
    [[nodiscard]] const Terminal& GetTerminal() const;
    void SetTerminal(Terminal term);
    [[nodiscard]] bool HasTerminal() const;

    template <typename RetType = void, typename... Args>
        requires meta::ConstructionArguments<Args...>
    Inst* AppendInst(OpCode op, Args&&... args) {
        return AppendInst(op, meta::ResolveResultType<RetType>(op, args...), std::forward<Args>(args)...);
    }
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    Inst* AppendInst(OpCode op, ValueType type, Args&&... args) {
        CheckMutable();
        return AppendInst(meta::CheckArgumentPack<Args...>(op), type, std::forward<Args>(args)...);
    }
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    Inst* AppendInst(meta::OperationFor<Args...> operation, ValueType type, Args&&... args) {
        return AppendConstructedInst(operation, type,
                meta::ForwardConstructionArgument(std::forward<Args>(args))...);
    }

private:
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    Inst* AppendConstructedInst(meta::OperationFor<Args...> operation, ValueType type, Args&&... args) {
        CheckMutable();
        const auto op = operation.Code();
        PerfScope2 perf_ir_append{GetPerfStats2().ir_append};
        if (PerfIRDetailEnabled()) {
            const bool ir_fast = IRBuildFastEnabled();
            auto begin = std::chrono::steady_clock::now();
            auto pending = std::unique_ptr<Inst>{new Inst(op, type)};
            auto* inst = pending.get();
            auto end = std::chrono::steady_clock::now();
            PerfIRDetailRecord(GetPerfStats2().ir_alloc, begin, end);

            begin = std::chrono::steady_clock::now();
            if (ir_fast) {
                inst->SetArgsFresh(std::forward<Args>(args)...);
            } else {
                inst->SetArgs(std::forward<Args>(args)...);
            }
            end = std::chrono::steady_clock::now();
            PerfIRDetailRecord(GetPerfStats2().ir_args, begin, end);

            begin = std::chrono::steady_clock::now();
            if (ir_fast) {
                AppendInstUnchecked(inst);
            } else {
                AppendInst(inst);
            }
            end = std::chrono::steady_clock::now();
            PerfIRDetailRecord(GetPerfStats2().ir_link, begin, end);
            pending.release();
            return inst;
        }
        const bool ir_fast = IRBuildFastEnabled();
        auto pending = std::unique_ptr<Inst>{new Inst(op, type)};
        auto* inst = pending.get();
        if (ir_fast) {
            inst->SetArgsFresh(std::forward<Args>(args)...);
        } else {
            inst->SetArgs(std::forward<Args>(args)...);
        }
        if (ir_fast) {
            AppendInstUnchecked(inst);
        } else {
            AppendInst(inst);
        }
        pending.release();
        return inst;
    }

public:

    void AppendInst(const Inst* inst);
    void AppendInstUnchecked(const Inst* inst);
    void InsertBefore(const Inst* inst, const Inst* before);
    void InsertAfter(const Inst* inst, const Inst* after);
    void MoveBefore(const Inst* inst, const Inst* before);
    void RemoveInst(const Inst* inst);
    void DestroyInst(const Inst* inst);
    // Frees every instruction in the block. Bulk teardown: it runs
    // Inst::ReleaseArgs, not Inst::DestroyArgs, so no instruction is
    // dereferenced through another instruction's operand. See ReleaseArgs for
    // why that is both correct here and materially cheaper -- and why it is
    // what makes tearing a whole function down safe regardless of the order
    // its blocks are visited in.
    void DestroyInstrs();
    void ReIdInstr();

    [[nodiscard]] const InstList& GetInstList() const;
    [[nodiscard]] bool IsIRReadOnly() const { return instruction_owner.sealed; }
    [[nodiscard]] bool IsEmptyBlock() const;
    [[nodiscard]] bool IsJitCached() const;

    [[nodiscard]] ReadLock LockRead() const { return std::shared_lock{block_lock}; }

    [[nodiscard]] WriteLock LockWrite() const { return std::unique_lock{block_lock}; }

    [[nodiscard]] u32 GetVStackSize() const { return v_stack * 8; }

    [[nodiscard]] backend::JitCache& GetJitCache() { return jit_cache; }
    // Retirement changes publication state independently of the semantic IR.
    void ClearJitCache() const { jit_cache = {}; }

    [[nodiscard]] u32 GetId() const { return id; }

    [[nodiscard]] u32 GetDispatchIndex() const { return dispatch_index; }

    [[nodiscard]] u32 MaxInstrId() const { return max_instr_id; }

    void AddGuestCodeDependency(Location start, Location end) {
        CheckMutable();
        if (end.Value() <= start.Value()) {
            return;
        }
        for (const auto& dependency : guest_code_dependencies) {
            if (dependency.start == start && dependency.end == end) {
                return;
            }
        }
        guest_code_dependencies.push_back({start, end});
    }
    [[nodiscard]] const std::vector<GuestCodeDependency>&
    GetGuestCodeDependencies() const {
        return guest_code_dependencies;
    }
    void ClearGuestCodeDependencies() {
        CheckMutable();
        guest_code_dependencies.clear();
    }

    [[nodiscard]] const LoopHoistMetadata& GetLoopHoistMetadata() const {
        return loop_hoist_metadata;
    }
    void SetLoopHoistMetadata(LoopHoistMetadata metadata) {
        CheckMutable();
        loop_hoist_metadata = metadata;
    }

    void SetDeadEdgeIntegerBranchProof(bool proven) {
        CheckMutable();
        dead_edge_integer_branch_proven = proven;
    }
    [[nodiscard]] bool HasDeadEdgeIntegerBranchProof() const {
        return dead_edge_integer_branch_proven;
    }

#define INST(name, ret, policy, ...)                                                                    \
    template <typename RetType = void, typename... Args>                                                \
        requires((!std::is_void_v<RetType> || meta::HasDefaultResultType<meta::kResultPolicy_##name>) && \
                 meta::ConstructionArguments<Args...> &&                                                \
                 meta::ValidArgumentPack<Args...>(meta::kArgs_##name))                                  \
    ret name(Args&&... args) {                                                                          \
        auto inst = AppendInst(meta::KnownOperation<OpCode::name, Args...>(),                           \
                    meta::ResolveResultType<meta::kResultPolicy_##name,                                 \
                    meta::kResult_##name, RetType>(args...), std::forward<Args>(args)...);              \
        return ret{inst};                                                                               \
    }                                                                                                   \
    template <typename... Args>                                                                         \
        requires(!std::is_same_v<ret, Void> &&                                                          \
                 meta::ConstructionArguments<Args...> &&                                                \
                 meta::ValidArgumentPack<Args...>(meta::kArgs_##name))                                  \
    ret name(ValueType type, Args&&... args) {                                                          \
        return ret{AppendInst(meta::KnownOperation<OpCode::name, Args...>(), type, std::forward<Args>(args)...)}; \
    }
#include "ir.inc"
#undef INST

    [[nodiscard]] std::string ToString() const;

    ~Block();

private:
    friend struct AddressNode;
    friend class Function;
    friend class HIRBlock;
    friend class HIRFunction;
    friend class compile::CompileUnit;
    friend class compile::RewriteContext;

    void CheckMutable() const { ASSERT(!instruction_owner.sealed); }
    Inst& EditInst(const Inst* inst) {
        CheckMutable();
        ASSERT(inst && inst->GetBlock() == this);
        return *const_cast<Inst*>(inst);
    }

    union {
        u32 id{};
        u32 dispatch_index;
    };
    bool dead_edge_integer_branch_proven{};
    InstList inst_list{};
    Inst::Owner instruction_owner{this};
    Terminal block_term{};
    mutable RwSpinLock block_lock{};
    u16 max_instr_id{};
    u16 v_stack{};
    mutable backend::JitCache jit_cache{};
    std::vector<GuestCodeDependency> guest_code_dependencies{};
    LoopHoistMetadata loop_hoist_metadata{};
    // Function membership maintains this deletion owner. Prepared units retain
    // the parent instead of introducing an independent child Block reference.
    ir::Function* owning_function{};
};

using BlockList = AddressNode::List;
using BlockMap = AddressNode::Map;

// Chain-free pseudo lookup (un-chaining step 3.6): a pseudo is an ordinary
// instruction in its owner's block whose arg 0 is the owner's value. These
// scans are O(block) and suit emission/test paths; hot planning paths use
// BlockAnalysisIndex or demand tables instead.
inline Inst::Pseudos CollectPseudoOperations(const Block& block, const Inst* owner) {
    Inst::Pseudos pseudos{};
    for (auto& inst : block.GetInstList()) {
        if (inst.IsPseudoOperation() && inst.ArgAt(0).IsValue() &&
            inst.GetArg<Value>(0).Def() == owner)
            pseudos.push_back(&inst);
    }
    return pseudos;
}

inline Inst::Pseudos CollectPseudoOperations(const Block& block, const Inst* owner, OpCode code) {
    Inst::Pseudos pseudos{};
    for (auto& inst : block.GetInstList()) {
        if (inst.GetOp() == code && inst.ArgAt(0).IsValue() &&
            inst.GetArg<Value>(0).Def() == owner)
            pseudos.push_back(&inst);
    }
    return pseudos;
}

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
            } else if constexpr (std::is_same_v<T, terminal::ExternalLinkBlock>) {
                content.append(fmt::format("  ExternalLink Block 0x{:x}\n", x.next.Value()));
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
