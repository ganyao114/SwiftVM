//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <memory>
#include <span>
#include <utility>
#include "runtime/common/svm_config.h"
#include "runtime/ir/function.h"
#include "runtime/ir/function_entry_provenance.h"
#include "runtime/ir/hir_storage.h"
#include "runtime/ir/host_reg.h"
#include "runtime/ir/module.h"

namespace swift::runtime::compile { class RewriteContext; }

namespace swift::runtime::ir {
class HIRBlock;
class HIRFunction;
class HIRBuilder;
class HIRTestSupport;
struct HIRPools;
struct HIRValue;

using HIRBlockVector = std::span<HIRBlock*>;
using HIRBlockView = std::span<const HIRBlock* const>;

struct Edge {
    explicit Edge(const HIRBlock* src, const HIRBlock* dest);
    const HIRBlock* GetSourceBlock() const { return src_block; }
    const HIRBlock* GetTargetBlock() const { return dest_block; }

private:
    const HIRBlock* src_block;
    const HIRBlock* dest_block;
    IntrusiveListNode outgoing_edges;
    IntrusiveListNode incoming_edges;

public:
    using OutgoingList = IntrusiveList<&Edge::outgoing_edges>;
    using IncomingList = IntrusiveList<&Edge::incoming_edges>;
};

#pragma pack(push, 1)
struct HIRUse {
    constexpr static auto USE_NIL = 255;
    constexpr static auto USE_FUNC_CALL = 253;
    constexpr static auto USE_PHI = 254;
    const Inst* inst;
    u8 arg_idx;
    IntrusiveListNode list_node{};

    explicit HIRUse(const Inst* inst, u8 arg_idx);

    [[nodiscard]] bool IsFuncCall() const { return arg_idx == USE_FUNC_CALL; }

    [[nodiscard]] bool IsPhi() const { return arg_idx == USE_PHI; }
};
#pragma pack(pop)

using HIRUseList = IntrusiveList<&HIRUse::list_node>;

#pragma pack(push, 4)
struct HIRValue final {
    const Value value;

private:
    friend class HIRBlock;
    friend class HIRFunction;
    HIRBlock* block;

public:
    HIRBlock* GetBlock() { return block; }
    const HIRBlock* GetBlock() const { return block; }
    [[nodiscard]] const HIRUseList& GetUses() const { return uses; }

    HIRValue() : value(), block(nullptr){};
    HIRValue(const Value& value) : value(value), block(nullptr){};
    explicit HIRValue(const Value& value, HIRBlock* block);

    [[nodiscard]] u16 GetOrderId() const;

private:
    HIRUseList uses{};
    void Use(const Inst* inst, u8 idx);
    void UnUse(const Inst* inst, u8 idx);
};

struct HIRLocal {
    Local local;
    HIRValue* current_value{};
};

#pragma pack(pop)

// Instruction id -> the HIRValue that instruction defines (null for the
// void-typed ones), dense over 0..MaxInstrCount()-1.
//
// This was an intrusive red-black tree keyed on the defining instruction's id,
// which made every use-chain lookup a tree walk through a NOINLINE comparator
// and forced IdByRPO to erase and re-insert every value just to re-key it.
// Instruction ids are already assigned densely and monotonically as
// instructions are appended, so the tree was an index over a sequence that is
// its own index. Iterating the vector in index order visits values in
// ascending id, exactly as the tree's in-order walk did -- which is what the
// linear-scan allocator relies on.
using HIRValueMap = Vector<HIRValue*>;

class HIRBlock final {
    friend class HIRFunction;
    friend class HIRValue;
    friend class HIRTestSupport;
    friend class compile::CompileUnit;

public:
    explicit HIRBlock(std::unique_ptr<Block> block, HIRValueMap& values, HIRPools& pools);

    template <typename RetType = void, typename... Args>
        requires meta::ConstructionArguments<Args...>
    HIRValue* AppendInst(OpCode op, Args&&... args) {
        return AppendInst(op, meta::ResolveResultType<RetType>(op, args...), std::forward<Args>(args)...);
    }
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    HIRValue* AppendInst(OpCode op, ValueType type, Args&&... args) {
        block->CheckMutable();
        return AppendInst(meta::CheckArgumentPack<Args...>(op), type, std::forward<Args>(args)...);
    }
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    HIRValue* AppendInst(meta::OperationFor<Args...> operation, ValueType type, Args&&... args) {
        return AppendConstructedInst(operation, type,
                meta::ForwardConstructionArgument(std::forward<Args>(args))...);
    }

private:
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    HIRValue* AppendConstructedInst(meta::OperationFor<Args...> operation, ValueType type, Args&&... args) {
        block->CheckMutable();
        const auto op = operation.Code();
        auto pending = std::unique_ptr<Inst>{new Inst(op, type)};
        auto* inst = pending.get();
        if (IRBuildFastEnabled()) {
            inst->SetArgsFresh(std::forward<Args>(args)...);
        } else {
            inst->SetArgs(std::forward<Args>(args)...);
        }
        return AppendInst(pending.release());
    }

public:

#define INST(name, ret, policy, ...)                                                                    \
    template <typename RetType = void, typename... Args>                                                \
        requires((!std::is_void_v<RetType> || meta::HasDefaultResultType<meta::kResultPolicy_##name>) && \
                 meta::ConstructionArguments<Args...> &&                                                \
                 meta::ValidArgumentPack<Args...>(meta::kArgs_##name))                                  \
    ret name(Args&&... args) {                                                                          \
        auto hir_value = AppendInst(meta::KnownOperation<OpCode::name, Args...>(),                      \
                    meta::ResolveResultType<meta::kResultPolicy_##name,                                 \
                    meta::kResult_##name, RetType>(args...), std::forward<Args>(args)...);              \
        return ret{hir_value ? hir_value->value.Def() : nullptr};                                       \
    }                                                                                                   \
    template <typename... Args>                                                                         \
        requires(!std::is_same_v<ret, Void> &&                                                          \
                 meta::ConstructionArguments<Args...> &&                                                \
                 meta::ValidArgumentPack<Args...>(meta::kArgs_##name))                                  \
    ret name(ValueType type, Args&&... args) {                                                          \
        auto* value = AppendInst(meta::KnownOperation<OpCode::name, Args...>(), type, std::forward<Args>(args)...); \
        return ret{value ? value->value.Def() : nullptr};                                               \
    }
#include "ir.inc"
#undef INST

    HIRValue* AppendInst(Inst* inst);
    HIRValue* InsertFront(Inst* inst);
    std::span<const HIRValue* const> GetHIRValues() const;
    [[nodiscard]] u16 GetOrderId() const;

    bool HasIncomingEdges() const;
    bool HasOutgoingEdges() const;

    const auto& GetIncomingEdges() const { return incoming_edges; }
    const auto& GetOutgoingEdges() const { return outgoing_edges; }

    HIRBlockView GetPredecessors() const { return predecessors; }
    HIRBlockView GetSuccessors() const { return successors; }

    [[nodiscard]] const HIRBlock* GetCallReturnBlock() const { return call_return_block; }

    [[nodiscard]] bool IsCallReturnBlock() const { return call_return_target; }

    Block* GetBlock();
    const Block* GetBlock() const;
    const InstList& GetInstList() const;
    bool BeginStandalonePublication();
    void AbortStandalonePublication();
    void CommitStandalonePublication();

private:
    IntrusiveListNode list_node{};

public:
    using List = IntrusiveList<&HIRBlock::list_node>;

private:
    HIRPoolLease RetainStorage() const;
    u16 order_id{};
    Block* block;
    // Synthetic and unfinished nodes stay here until canonical ownership transfers.
    std::unique_ptr<Block> unpublished_block;
    HIRFunction* function{};
    bool standalone_publication_pending{};
    HIRPools& pools;
    HIRValueMap& value_map;
    Edge::OutgoingList outgoing_edges{};
    Edge::IncomingList incoming_edges{};
    std::span<const HIRBlock*> predecessors;
    std::span<const HIRBlock*> successors;
    HIRBlock* call_return_block{};
    bool call_return_target{};
};

using HIRBlockList = HIRBlock::List;

class HIRFunction final {
public:
    struct ExternalDirectLink {
        const HIRBlock* source{};
        Location target{};
    };

    explicit HIRFunction(std::unique_ptr<Function> function,
                         const Location& begin,
                         const Location& end,
                         HIRPools& pools,
                         bool ir_fast);
    ~HIRFunction();

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
        ASSERT(current_block);
        current_block->block->CheckMutable();
        if (PerfIRDetailEnabled()) {
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
            inst->SetId(inst_order_id++);
            end = std::chrono::steady_clock::now();
            PerfIRDetailRecord(GetPerfStats2().ir_args, begin, end);

            begin = std::chrono::steady_clock::now();
            if (ir_fast) {
                current_block->block->AppendInstUnchecked(inst);
            } else {
                current_block->block->AppendInst(inst);
            }
            end = std::chrono::steady_clock::now();
            PerfIRDetailRecord(GetPerfStats2().ir_link, begin, end);

            pending.release();
            AppendValue(current_block, inst);
            return inst;
        }
        auto pending = std::unique_ptr<Inst>{new Inst(op, type)};
        auto* inst = pending.get();
        if (ir_fast) {
            inst->SetArgsFresh(std::forward<Args>(args)...);
        } else {
            inst->SetArgs(std::forward<Args>(args)...);
        }
        inst->SetId(inst_order_id++);
        if (ir_fast) {
            current_block->block->AppendInstUnchecked(inst);
        } else {
            current_block->block->AppendInst(inst);
        }
        pending.release();
        AppendValue(current_block, inst);
        return inst;
    }

public:

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

    HIRBlock* AppendBlock(Location start, Location end = {});
    HIRBlock* CreateOrGetBlock(Location location);
    void SetCurBlock(const HIRBlock* block);
    const HIRBlock* GetEntryBlock() const;
    const HIRBlock* GetCurrentBlock() const;
    HIRBlockView GetHIRBlocks() const;
    const HIRBlockList& GetHIRBlockList() const;
    const HIRBlockList& GetHIRBlocksRPO() const;
    HIRBlock& EditBlock(const HIRBlock* block);
    void RegisterExternalDirectLink(Location target);
    [[nodiscard]] const std::vector<ExternalDirectLink>& GetExternalDirectLinks() const {
        return external_direct_links;
    }
    void RegisterExternalEntryRoot(const HIRBlock* block);
    [[nodiscard]] HIRBlockView GetExternalEntryRoots() const { return external_entry_roots; }
    void SetFunctionEntryProvenance(std::vector<FunctionEntryProvenance> provenance) {
        CheckMutable();
        function_entry_provenance = std::move(provenance);
    }
    [[nodiscard]] const std::vector<FunctionEntryProvenance>& GetFunctionEntryProvenance() const {
        return function_entry_provenance;
    }
    std::span<const HIRValue* const> GetHIRValues() const;
    const HIRValue* GetHIRValue(const Value& value) const;
    Function* GetFunction();
    const Function* GetFunction() const;
    void MarkFunctionPublished();
    [[nodiscard]] bool HasStandaloneBlocks() const { return standalone_blocks_published; }
    void AddEdge(const HIRBlock* src, const HIRBlock* dest);
    void RegisterCallReturn(Location location);
    bool ResetDecodedBlock(const HIRBlock* block);
    // Populates blocks_rpo with the reverse-post-order of the primary CFG and
    // each canonical external-entry root.
    void ComputeRPO();
    void IdByRPO();

    void EndBlock(Terminal terminal);
    void EndFunction();

    u16 MaxBlockCount() const;
    u16 MaxInstrCount() const;
    u16 MaxLocalCount() const;

private:
    IntrusiveListNode list_node;

public:
    using List = IntrusiveList<&HIRFunction::list_node>;

private:
    friend class HIRBuilder;
    friend class HIRBlock;
    friend class compile::RewriteContext;
    friend class HIRTestSupport;
    friend class compile::CompileUnit;

    HIRValue* AppendValue(HIRBlock* block, Inst* inst);
    void DestroyHIRValue(HIRValue* value);
    // Removes `inst` from its block and frees it, keeping the function-level
    // HIRValue bookkeeping consistent: every HIRUse this instruction holds on
    // another value is unregistered first, and the instruction's own HIRValue
    // (if it defines one) is erased from `values`. A basic `delete` would leave
    // dangling HIRUse nodes behind, which the function-level register allocator
    // dereferences when it computes live-interval ends.
    void EraseInst(Block* block, const Inst* inst);
    HIRPoolLease RetainStorage() const;
    HIRValue* FindValueRecord(const Value& value) const;
    void SetIRReadOnly(bool read_only);
    void RemoveEdge(const Edge* edge);
    void CheckMutable() const {
        function->CheckMutable();
        if (entry_block) entry_block->GetBlock()->CheckMutable();
    }
    void UseInst(const Inst* inst);
    void UnUseInst(const Inst* inst);
    static void ReleaseTerminalUses(const Terminal& terminal);
    HIRValue* AppendValueRecord(HIRBlock* block, Inst* inst);
    void TrackLocal(Inst* inst);

    u16 max_local_id{};
    // Keeps the persistent owner alive even when its publishing module ends first.
    // Function still directly owns its child Blocks; no Block refcounts are added.
    IntrusivePtr<Function> function;
    bool function_published{};
    bool standalone_blocks_published{};
    Location begin;
    Location end;
    u16 block_order_id{};
    u16 inst_order_id{};
    u16 value_count{};
    bool ir_fast{};
    HIRPools& pools;

    HIRBlockVector blocks{};
    HIRBlockList block_list{};
    // Reverse Post Order
    HIRBlockList blocks_rpo{};
    std::vector<ExternalDirectLink> external_direct_links{};
    std::vector<HIRBlock*> external_entry_roots{};
    HIRValueMap values{};
    // Scratch target for IdByRPO's rebuild of `values`; kept as a member so the
    // second (post-pass) renumbering reuses the first one's buffer instead of
    // allocating again.
    HIRValueMap reid_scratch{};
    std::vector<FunctionEntryProvenance> function_entry_provenance{};
    HIRBlock* current_block{};
    HIRBlock* entry_block{};
};

using HIRFunctionList = HIRFunction::List;

class HIRBuilder {
public:
    struct ElseThen {
        HIRBlock* else_;
        HIRBlock* then_;
    };

    struct CaseBlock {
        Imm case_{0u};
        HIRBlock* then_{};
    };

    // defer_function_end keeps the HIR function open when one decoded block
    // reaches ret/syscall/indirect control flow. Whole-function discovery
    // decodes the remaining queued CFG blocks before calling EndFunction().
    explicit HIRBuilder(u32 func_cap = 1,
                        bool defer_function_end = false,
                        bool ir_fast = IRBuildFastEnabled(),
                        const FeatureSet& features = FeatureSet{});
    HIRBuilder(u32 func_cap, bool defer_function_end,
               const FeatureSet& features)
            : HIRBuilder(func_cap, defer_function_end, IRBuildFastEnabled(), features) {}
    ~HIRBuilder() { hir_functions.clear(); }
    HIRBuilder(const HIRBuilder&) = delete;
    HIRBuilder& operator=(const HIRBuilder&) = delete;

    HIRFunction* AppendFunction(Location start, Location end = {});

    // False once a function-ending terminal (Return/ReturnToHost/...) has been
    // emitted — those call EndFunction and clear the current function. The
    // function-level decode loop uses this to stop decoding and avoid a second
    // (corrupting) EndFunction call.
    [[nodiscard]] bool HasCurrentFunction() const { return current_function != nullptr; }
    [[nodiscard]] bool HasCurrentFunctionBlock(Location location);

    template <typename RetType = void, typename... Args>
        requires meta::ConstructionArguments<Args...>
    Inst* AppendInst(OpCode op, Args&&... args) {
        return AppendInst(op, meta::ResolveResultType<RetType>(op, args...), std::forward<Args>(args)...);
    }
    template <typename... Args> requires meta::ConstructionArguments<Args...>
    Inst* AppendInst(OpCode op, ValueType type, Args&&... args) {
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
        const auto op = operation.Code();
        // A function-ending terminal (Return/ReturnToHost) clears
        // current_function mid-Decode, but the x86 decoder still emits a
        // trailing AdvancePC right after it (decoder.cc decode loop). In block
        // mode that lands harmlessly in the block's inst list; here there is no
        // current function to append to. AdvancePC is only a flags-flush marker
        // (the backend emits no pc motion for it) and the block-end FlushFlags +
        // terminal MergeNZCV already cover it, so dropping it is a safe no-op.
        if (!current_function || !current_function->GetCurrentBlock()) {
            ASSERT_MSG(op == OpCode::AdvancePC,
                       "IR {} emitted after the current block terminal",
                       op);
            return nullptr;
        }
        // AdvancePC coalescing -- see FoldAdvancePC. Guarded by the argument
        // shape so the branch folds away for every other opcode.
        if constexpr (sizeof...(Args) == 1 &&
                      (std::is_same_v<std::decay_t<Args>, Imm> && ...)) {
            if (advpc_coalesce && op == OpCode::AdvancePC && FoldAdvancePC(args...)) {
                return nullptr;
            }
        }
        auto* inst = current_function->AppendInst(operation, type, std::forward<Args>(args)...);
        if (LeavesPendingFlags(op)) flags_since_advance = true;
        if (op == OpCode::AdvancePC) {
            last_advance = inst;
            last_advance_block = current_function->GetCurrentBlock();
            flags_since_advance = false;
        }
        return inst;
    }

public:

    template <typename Edit> void RewriteInst(Inst* inst, Edit&& edit) {
        ASSERT(current_function);
        current_function->UnUseInst(inst);
        edit();
        current_function->UseInst(inst);
    }

#define INST(name, ret, policy, ...)                                                                    \
    template <typename RetType = void, typename... Args>                                                \
        requires((!std::is_void_v<RetType> || meta::HasDefaultResultType<meta::kResultPolicy_##name>) && \
                 meta::ConstructionArguments<Args...> &&                                                \
                 meta::ValidArgumentPack<Args...>(meta::kArgs_##name))                                  \
    ret name(Args&&... args) {                                                                          \
        return ret{AppendInst(meta::KnownOperation<OpCode::name, Args...>(),                            \
                    meta::ResolveResultType<meta::kResultPolicy_##name,                                 \
                    meta::kResult_##name, RetType>(args...), std::forward<Args>(args)...)};             \
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

    void SetLocation(Location location);

    void AddGuestCodeDependency(Location start, Location end) {
        ASSERT(current_function && current_function->GetCurrentBlock());
        current_function->EditBlock(current_function->GetCurrentBlock())
                .GetBlock()
                ->AddGuestCodeDependency(start, end);
    }

    void SetCurBlock(const HIRBlock* block);

    void SetCurBlock(Location location);

    bool ResetDecodedBlock(const HIRBlock* block);

    ElseThen If(const terminal::If& if_);

    Vector<CaseBlock> Switch(const terminal::Switch& switch_);

    HIRBlock* LinkBlock(const terminal::LinkBlock& switch_);

    void ExternalLinkBlock(const terminal::ExternalLinkBlock& link);

    void RegisterExternalDirectLink(Location target);

    void RegisterCallReturn(Location location);

    void ReturnToDispatcher();

    void ReturnToHost();

    void Return();

    void End();

private:
    Location GetNextLocation(const Terminal& term);

    // `AdvancePC` is not program-counter motion -- no backend emits any -- it is
    // the guest instruction boundary at which the arm64 backend commits its
    // lazily kept flag state (EmitAdvancePC = MergeNZCV + FlushFlags) and at
    // which the interpreter does nothing at all. Its immediate is read in
    // exactly one other place: the sum over a block is that block's guest byte
    // length (backend/runtime.cpp, SMC range registration).
    //
    // Measured over the 25 e2e guests: 21 562 AdvancePC reach the backend and
    // only 4 016 (18.6%) find pending NZCV, 2 543 (11.8%) a pending ClearFlags
    // -- at least 69.6% emit nothing whatsoever while still costing a pool
    // object, a HIRValue slot, and one visit in every pass, in Allocation and in
    // codegen. That is 13.4% of all IR.
    //
    // So drop the ones that cannot do anything, and fold their immediate
    // *backwards* into the last AdvancePC that was kept in the same block. That
    // direction matters: the running sum of the retained immediates equals the
    // sum of all of them after every single step, so no block-close hook is
    // needed and the guest byte length is preserved exactly -- including for
    // blocks whose trailing AdvancePC is dropped by the terminal rule above.
    //
    // "Cannot do anything" = no instruction since the last retained AdvancePC
    // can leave pending flag state (LeavesPendingFlags). The emitted host code
    // is therefore byte-identical, which is the check this is verified with
    // (SVM_PROF=2 per-unit ir/host fingerprints) rather than a claim.
    bool FoldAdvancePC(const Imm& imm);

    // Every opcode whose backend emission can leave state that EmitAdvancePC
    // would have to commit. `nzcv_dirty` is only ever set by SaveHostFlags and
    // SaveNZ, both of which are reached exclusively from emitters gated on a
    // SaveFlags pseudo-operation; `flags_clear` only by EmitClearFlags.
    // SetCarry/SetOverflow are listed because they touch the guest flags word
    // directly, so a boundary after them is kept even though they self-flush.
    static constexpr bool LeavesPendingFlags(OpCode op) {
        switch (op) {
            case OpCode::SaveFlags:
            case OpCode::ClearFlags:
            case OpCode::SetCarry:
            case OpCode::SetOverflow:
            case OpCode::InvertCarry:
            case OpCode::PublishFCmpFlags:
            case OpCode::PublishSse42StrFlags:
                return true;
            default:
                return false;
        }
    }

    // The owning lease outlives the builder roster. Prepared units may retain
    // their own lease, postponing pool reuse until their final consumer ends.
    HIRPoolLease pool_lease;
    HIRPools& pools;
    HIRFunctionList hir_functions{};
    Location current_location;
    HIRFunction* current_function{};
    bool defer_function_end{};
    bool ir_fast{};
    // AdvancePC coalescing state. last_advance_block is compared by pointer so
    // no block-transition hook is needed: If/LinkBlock/Switch/SetCurBlock all
    // move current_block, and a mismatch simply means "no fold target here".
    Inst* last_advance{};
    const HIRBlock* last_advance_block{};
    bool flags_since_advance{false};
    const bool advpc_coalesce;
};

}  // namespace swift::runtime::ir
