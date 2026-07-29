//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <span>
#include "runtime/common/mem_arena.h"
#include "runtime/common/object_pool.h"
#include "runtime/ir/function.h"
#include "runtime/ir/host_reg.h"
#include "runtime/ir/module.h"

namespace swift::runtime::ir {

class HIRBlock;
class HIRFunction;
class HIRBuilder;
struct HIRPools;
struct HIRValue;

using HIRBlockVector = std::span<HIRBlock*>;
using HIRBlockSet = Set<HIRBlock*>;

class DataContext {
public:
    virtual u16 MaxBlockCount() = 0;
    virtual u16 MaxInstrCount() = 0;
    virtual u16 MaxLocalCount() = 0;
};

struct Edge {
    enum Flags : u8 {
        CONDITIONAL = 1 << 0,
        DOMINATES = 1 << 1,
    };

    explicit Edge(HIRBlock* src, HIRBlock* dest);

    IntrusiveListNode outgoing_edges;
    IntrusiveListNode incoming_edges;

    HIRBlock* src_block;
    HIRBlock* dest_block;

    u8 flags{};
};

struct Dominance {
    IntrusiveListNode node;
    HIRBlock* block;

    explicit Dominance(HIRBlock* block) : block(block), node(){};
};

using DomFrontier = IntrusiveList<&Dominance::node>;

struct BackEdge {
    HIRBlock* target;
    IntrusiveListNode list_node{};

    explicit BackEdge(HIRBlock* block) : target(block) {}
};

using BackEdgeList = IntrusiveList<&BackEdge::list_node>;

#pragma pack(push, 1)
struct ValueAllocated {
    enum Type : u8 { NONE, GPR, FPR, MEM };
    Type type;
    union {
        HostGPR host_gpr;
        HostFPR host_fpr;
        SpillSlot spill_slot;
    };

    [[nodiscard]] bool Allocated() const { return type != NONE; }

    explicit ValueAllocated() : type(NONE) {}
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HIRUse {
    constexpr static auto USE_NIL = 255;
    constexpr static auto USE_FUNC_CALL = 253;
    constexpr static auto USE_PHI = 254;
    Inst* inst;
    u8 arg_idx;
    IntrusiveListNode list_node{};

    explicit HIRUse(Inst* inst, u8 arg_idx);

    [[nodiscard]] bool IsFuncCall() const { return arg_idx == USE_FUNC_CALL; }

    [[nodiscard]] bool IsPhi() const { return arg_idx == USE_PHI; }
};
#pragma pack(pop)

using HIRUseList = IntrusiveList<&HIRUse::list_node>;

#pragma pack(push, 4)
struct HIRValue final {
    Value value;
    HIRBlock* block;
    ValueAllocated allocated{};
    HIRUseList uses{};

    HIRValue() : value(), block(nullptr){};
    HIRValue(const Value& value) : value(value), block(nullptr){};
    explicit HIRValue(const Value& value, HIRBlock* block);

    void Use(Inst* inst, u8 idx);
    void UnUse(Inst* inst, u8 idx);

    [[nodiscard]] u16 GetOrderId() const;
};

struct HIRLocal {
    Local local;
    HIRValue* current_value{};
};

// HIRLoop / HIRLoopList lived here. Removed, not repaired: the constructor
// took a single `HIRBlock* header` and memcpy'd `length * sizeof(HIRBlock*)`
// bytes OUT OF THAT ONE OBJECT, so every loop it built was the header block's
// own fields reinterpreted as block pointers, reading past the object once
// length exceeded sizeof(HIRBlock)/8. Its only producer (ComputeLoopInformation
// in ir/opts/cfg_analysis_pass.cpp) was deleted in the previous round, and the
// only accessor of the resulting list, HIRFunction::GetHIRLoop, never had a
// caller anywhere in the tree -- so the type had no way left to be either
// exercised or validated. A "fixed" constructor with no producer and no
// consumer is an unverifiable claim; whoever next needs loop information should
// add the type back together with the analysis that fills it and the code that
// reads it.
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

class HIRBlock final : public DataContext {
    friend class HIRFunction;
    friend class HIRValue;

public:
    explicit HIRBlock(Block* block, HIRValueMap& values, HIRPools& pools);

    template <typename... Args> Inst* CreateInst(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        if (IRBuildFastEnabled()) {
            inst->SetArgsFresh(std::forward<const Args&>(args)...);
        } else {
            inst->SetArgs(std::forward<const Args&>(args)...);
        }
        return inst;
    }

    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>
    HIRValue* AppendInst(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        if (IRBuildFastEnabled()) {
            inst->SetArgsFresh(std::forward<const Args&>(args)...);
        } else {
            inst->SetArgs(std::forward<const Args&>(args)...);
        }
        if constexpr (RetType::TYPE != ValueType::VOID) {
            inst->SetReturn(RetType::TYPE);
        }
        return AppendInst(inst);
    }

#define INST(name, ret, ...)                                                                       \
    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>                    \
    ret name(const Args&... args) {                                                                \
        static_assert(meta::ValidArgCount(OpCode::name, meta::kArgs_##name.size(),                 \
                                          sizeof...(Args)),                                        \
                    #name ": argument count mismatch (see ir.inc)");                               \
        auto hir_value = AppendInst<RetType>(OpCode::name, std::forward<const Args&>(args)...);    \
        return ret{hir_value ? hir_value->value.Def() : nullptr};                                  \
    }
#include "ir.inc"
#undef INST

    HIRValue* AppendInst(Inst* inst);
    HIRValue* InsertFront(Inst* inst);
    HIRValueMap& GetHIRValues();
    [[nodiscard]] u16 GetOrderId() const;

    void AddOutgoingEdge(Edge* edge);
    void AddIncomingEdge(Edge* edge);
    void AddBackEdge(HIRBlock* back_edge);
    bool HasIncomingEdges();
    bool HasOutgoingEdges();

    auto& GetIncomingEdges() { return incoming_edges; }
    auto& GetOutgoingEdges() { return outgoing_edges; }

    auto& GetPredecessors() { return predecessors; }
    auto& GetSuccessors() { return successors; }

    auto& GetBackEdges() { return back_edges; }

    auto& GetDomFrontier() { return dom_frontier; }

    void PushDominance(HIRBlock* block);
    void SetDominator(HIRBlock* block_) { dominator = block_; };
    auto GetDominator() { return dominator; };

    Block* GetBlock();
    InstList& GetInstList();

    u16 MaxBlockCount() override;
    u16 MaxInstrCount() override;
    u16 MaxLocalCount() override;

    IntrusiveListNode list_node{};

private:
    u16 order_id{};
    Block* block;
    HIRFunction* function{};
    HIRPools& pools;
    HIRValueMap& value_map;
    IntrusiveList<&Edge::outgoing_edges> outgoing_edges{};
    IntrusiveList<&Edge::incoming_edges> incoming_edges{};
    HIRBlockVector predecessors;
    HIRBlockVector successors;
    BackEdgeList back_edges{};
    HIRBlock* dominator{};
    DomFrontier dom_frontier{};
};

using HIRBlockList = IntrusiveList<&HIRBlock::list_node>;

class HIRFunction final : public DataContext {
public:
    explicit HIRFunction(Function* function,
                         const Location& begin,
                         const Location& end,
                         HIRPools& pools,
                         bool ir_fast);
    ~HIRFunction();

    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>
    Inst* AppendInst(OpCode op, const Args&... args) {
        PerfScope2 perf_ir_append{GetPerfStats2().ir_append};
        ASSERT(current_block);
        if (PerfIRDetailEnabled()) {
            auto begin = std::chrono::steady_clock::now();
            auto inst = new Inst(op);
            auto end = std::chrono::steady_clock::now();
            PerfIRDetailRecord(GetPerfStats2().ir_alloc, begin, end);

            begin = std::chrono::steady_clock::now();
            if (ir_fast) {
                inst->SetArgsFresh(std::forward<const Args&>(args)...);
            } else {
                inst->SetArgs(std::forward<const Args&>(args)...);
            }
            if constexpr (RetType::TYPE != ValueType::VOID) {
                inst->SetReturn(RetType::TYPE);
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

            if (ir_fast) {
                AppendValueFast(current_block, inst, args...);
            } else {
                AppendValue(current_block, inst);
            }
            return inst;
        }
        auto inst = new Inst(op);
        if (ir_fast) {
            inst->SetArgsFresh(std::forward<const Args&>(args)...);
        } else {
            inst->SetArgs(std::forward<const Args&>(args)...);
        }
        inst->SetId(inst_order_id++);
        if constexpr (RetType::TYPE != ValueType::VOID) {
            inst->SetReturn(RetType::TYPE);
        }
        if (ir_fast) {
            current_block->block->AppendInstUnchecked(inst);
            AppendValueFast(current_block, inst, args...);
        } else {
            current_block->block->AppendInst(inst);
            AppendValue(current_block, inst);
        }
        return inst;
    }

#define INST(name, ret, ...)                                                                       \
    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>                    \
    ret name(const Args&... args) {                                                                \
        static_assert(meta::ValidArgCount(OpCode::name, meta::kArgs_##name.size(),                 \
                                          sizeof...(Args)),                                        \
                    #name ": argument count mismatch (see ir.inc)");                               \
        auto inst = AppendInst(OpCode::name, std::forward<const Args&>(args)...);                  \
        if constexpr (RetType::TYPE != ValueType::VOID) {                                          \
            if (!ir_fast) {                                                                         \
                inst->SetReturn(RetType::TYPE);                                                    \
            }                                                                                       \
        }                                                                                          \
        return ret{inst};                                                                          \
    }
#include "ir.inc"
#undef INST

    HIRBlock* AppendBlock(Location start, Location end = {});
    HIRBlock* CreateOrGetBlock(Location location);
    void SetCurBlock(HIRBlock* block);
    HIRValue* AppendValue(HIRBlock* block, Inst* inst);
    void DestroyHIRValue(HIRValue* value);
    // Removes `inst` from its block and frees it, keeping the function-level
    // HIRValue bookkeeping consistent: every HIRUse this instruction holds on
    // another value is unregistered first, and the instruction's own HIRValue
    // (if it defines one) is erased from `values`. A plain `delete` would leave
    // dangling HIRUse nodes behind, which the function-level register allocator
    // dereferences when it computes live-interval ends.
    void EraseInst(Block* block, Inst* inst);
    HIRBlock* GetEntryBlock();
    HIRBlock* GetCurrentBlock();
    HIRBlockVector& GetHIRBlocks();
    HIRBlockList& GetHIRBlockList();
    HIRBlockList& GetHIRBlocksRPO();
    HIRValueMap& GetHIRValues();
    HIRValue* GetHIRValue(const Value& value);
    HIRPools& GetMemPool();
    Function* GetFunction();
    void ReleaseFunctionOwnership();
    void AddEdge(HIRBlock* src, HIRBlock* dest, bool conditional = false);
    void RemoveEdge(Edge* edge);
    void MergeAdjacentBlocks(HIRBlock* left, HIRBlock* right);
    bool SplitBlock(HIRBlock* new_block, HIRBlock* old_block);
    // Populates blocks_rpo with the reverse-post-order of the CFG reachable
    // from the entry block (the synthetic entry itself is excluded — it holds
    // no guest instructions, only a LinkBlock to the first real block). Must be
    // called after EndFunction (predecessors/successors are built there) and
    // before IdByRPO / function-level register allocation / emission, all of
    // which rely on a consistent RPO instruction numbering.
    void ComputeRPO();
    void IdByRPO();

    void EndBlock(Terminal terminal);
    void EndFunction();

    u16 MaxBlockCount() override;
    u16 MaxInstrCount() override;
    u16 MaxLocalCount() override;

    IntrusiveListNode list_node;

private:
    friend class HIRBuilder;
    friend class HIRBlock;

    struct {
        u32 current_slot{0};
    } spill_stack{};

    void UseInst(Inst* inst);
    void UnUseInst(Inst* inst);
    HIRValue* AppendValueRecord(HIRBlock* block, Inst* inst);
    void TrackLocal(Inst* inst);

    template <typename ArgTypeT>
    void UseFreshArg(Inst* inst, u8 physical, const ArgTypeT& arg) {
        using T = std::decay_t<ArgTypeT>;
        if constexpr (std::is_base_of_v<Value, T>) {
            if (auto* hir_value = GetHIRValue(static_cast<const Value&>(arg)); hir_value) {
                hir_value->Use(inst, physical);
            }
        } else if constexpr (std::is_same_v<T, Lambda>) {
            if (arg.IsValue()) {
                if (auto* hir_value = GetHIRValue(arg.GetValue()); hir_value) {
                    hir_value->Use(inst, physical);
                }
            }
        } else if constexpr (std::is_same_v<T, Params>) {
            for (auto param : arg) {
                if (auto data = param.data; data.IsValue()) {
                    if (auto* hir_value = GetHIRValue(data.value); hir_value) {
                        hir_value->Use(inst, HIRUse::USE_FUNC_CALL);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, Operand>) {
            const auto left = arg.GetLeft();
            const auto right = arg.GetRight();
            if (left.IsValue()) {
                if (auto* hir_value = GetHIRValue(left.value); hir_value) {
                    hir_value->Use(inst, physical + 1);
                }
            }
            if (right.IsValue()) {
                if (auto* hir_value = GetHIRValue(right.value); hir_value) {
                    hir_value->Use(inst, physical + 2);
                }
            }
        }
    }

    template <typename... Args>
    HIRValue* AppendValueFast(HIRBlock* block, Inst* inst, const Args&... args) {
        const bool detail = PerfIRDetailEnabled();
        auto begin = detail ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
        auto* hir_value = AppendValueRecord(block, inst);
        if (detail) {
            const auto end = std::chrono::steady_clock::now();
            PerfIRDetailRecord(GetPerfStats2().ir_value, begin, end);
            begin = end;
        }
        u8 physical{};
        auto use_arg = [&](const auto& arg) {
            UseFreshArg(inst, physical, arg);
            using T = std::decay_t<decltype(arg)>;
            physical += std::is_same_v<T, Operand> ? PhysicalSlots(ArgType::Operand) : 1;
        };
        (use_arg(args), ...);
        if (detail) {
            PerfIRDetailRecord(
                    GetPerfStats2().ir_use, begin, std::chrono::steady_clock::now());
        }
        TrackLocal(inst);
        return hir_value;
    }

    u16 max_local_id{};
    Function* function;
    bool owns_function{true};
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
    HIRValueMap values{};
    // Scratch target for IdByRPO's rebuild of `values`; kept as a member so the
    // second (post-pass) renumbering reuses the first one's buffer instead of
    // allocating again.
    HIRValueMap reid_scratch{};
    HIRBlock* current_block{};
    HIRBlock* entry_block{};
};

using HIRFunctionList = IntrusiveList<&HIRFunction::list_node>;

struct HIRPools {
    void ReleaseContents() {
        functions.ReleaseContents();
        blocks.ReleaseContents();
        values.ReleaseContents();
        edges.ReleaseContents();
        uses.ReleaseContents();
    }

    // Returns the pools to their construction state, ready for another
    // compilation unit, without freeing the backing chunks. Destructors run
    // exactly as they would if the pools were destroyed: ObjectPool's
    // destruct=true instantiations (functions and blocks) release their
    // objects, so an HIRFunction that still owns its ir::Function still deletes
    // it here.
    void Reset() {
        ReleaseContents();
        mem_arena.Reset();
    }

    explicit HIRPools(u32 func_cap = 1);

    HIRBlockVector CreateBlockVector(size_t size) {
        return {mem_arena.CreateArray<HIRBlock*>(size), size};
    }

    MemArena mem_arena;
    ObjectPool<HIRFunction, true> functions;
    ObjectPool<HIRBlock, true> blocks;
    ObjectPool<HIRValue> values;
    ObjectPool<Edge> edges;
    ObjectPool<HIRUse> uses;
};

// Borrows a per-thread HIRPools instead of building one per compiled unit.
//
// Constructing HIRPools means six allocator round trips and destroying it means
// six more; under lazy function compilation that is once per decoded guest
// block, which made it pure fixed overhead of the pipeline rather than work on
// behalf of the unit. The lease hands back a pool set that has been Reset() to
// the same state a freshly constructed one would be in.
//
// A second, nested lease on the same thread (a builder constructed while
// another is alive) gets its own privately owned HIRPools, so nesting stays
// correct rather than sharing one arena between two builders.
class HIRPoolLease {
public:
    explicit HIRPoolLease(u32 func_cap);
    ~HIRPoolLease();

    HIRPoolLease(const HIRPoolLease&) = delete;
    HIRPoolLease& operator=(const HIRPoolLease&) = delete;

    HIRPools& Get() const { return *pools; }

private:
    HIRPools* pools{};
    std::unique_ptr<HIRPools> owned{};
};

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
                        bool ir_fast = IRBuildFastEnabled());

    HIRFunction* AppendFunction(Location start, Location end = {});

    // False once a function-ending terminal (Return/ReturnToHost/...) has been
    // emitted — those call EndFunction and clear the current function. The
    // function-level decode loop uses this to stop decoding and avoid a second
    // (corrupting) EndFunction call.
    [[nodiscard]] bool HasCurrentFunction() const { return current_function != nullptr; }

    HIRFunctionList& GetHIRFunctions();

    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>
    Inst* AppendInst(OpCode op, const Args&... args) {
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
        if (LeavesPendingFlags(op)) {
            flags_since_advance = true;
        }
        auto* inst =
                current_function->AppendInst<RetType>(op, std::forward<const Args&>(args)...);
        if (op == OpCode::AdvancePC) {
            last_advance = inst;
            last_advance_block = current_function->GetCurrentBlock();
            flags_since_advance = false;
        }
        return inst;
    }

#define INST(name, ret, ...)                                                                       \
    template <typename RetType = TypedValue<ValueType::VOID>, typename... Args>                    \
    ret name(const Args&... args) {                                                                \
        static_assert(meta::ValidArgCount(OpCode::name, meta::kArgs_##name.size(),                 \
                                          sizeof...(Args)),                                        \
                    #name ": argument count mismatch (see ir.inc)");                               \
        return ret{AppendInst<RetType>(OpCode::name, std::forward<const Args&>(args)...)};         \
    }
#include "ir.inc"
#undef INST

    template <typename Lambda, typename... Args> Value CallHost(Lambda l, const Args&... args) {
        constexpr static auto MAX_ARG = 3;
        auto arg_count = sizeof...(args);
        ASSERT(arg_count <= MAX_ARG);
        return CallLambda(FptrCast(l), std::forward<const Args&>(args)...);
    }

    void SetLocation(Location location);

    void SetCurBlock(HIRBlock* block);

    void SetCurBlock(Location location);

    ElseThen If(const terminal::If& if_);

    Vector<CaseBlock> Switch(const terminal::Switch& switch_);

    HIRBlock* LinkBlock(const terminal::LinkBlock& switch_);

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
    // object, a HIRValue slot, and one visit in every pass, in RegAlloc and in
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
    static bool AdvancePCCoalesceEnabled();

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
                return true;
            default:
                return false;
        }
    }

    // Declared first so it outlives every member that allocates from it: the
    // lease's destructor is what returns the pools to a clean state, and it
    // must run after hir_functions has released its (intrusively linked)
    // HIRFunction objects.
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
    HIRBlock* last_advance_block{};
    bool flags_since_advance{false};
    const bool advpc_coalesce{AdvancePCCoalesceEnabled()};
};

void DfsHIRBlock(HIRBlock* start, HIRBlock* end, HIRBlockSet& visited);

}  // namespace swift::runtime::ir
