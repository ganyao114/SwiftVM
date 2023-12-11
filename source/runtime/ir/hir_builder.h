//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <span>
#include "runtime/common/mem_arena.h"
#include "runtime/common/object_pool.h"
#include "runtime/ir/function.h"
#include "runtime/ir/module.h"
#include "runtime/ir/host_reg.h"

namespace swift::runtime::ir {

class HIRBlock;
class HIRFunction;
class HIRBuilder;
struct HIRPools;
struct HIRValue;

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
    HIRBlock *block;

    explicit Dominance(HIRBlock* block) : block(block), node() {};
};

using DomFrontier = IntrusiveList<&Dominance::node>;

struct BackEdge {
    HIRBlock *target;
    IntrusiveListNode list_node{};

    explicit BackEdge(HIRBlock *block) : target(block) {}
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

    [[nodiscard]] bool IsFuncCall() const {
        return arg_idx == USE_FUNC_CALL;
    }

    [[nodiscard]] bool IsPhi() const {
        return arg_idx == USE_PHI;
    }
};
#pragma pack(pop)

using HIRUseList = IntrusiveList<&HIRUse::list_node>;

#pragma pack(push, 4)
struct HIRValue final {
    Value value;
    HIRBlock* block;
    ValueAllocated allocated{};
    HIRUseList uses{};

    IntrusiveMapNode map_node{};

    HIRValue() : value(), block(nullptr) {};
    HIRValue(const Value& value) : value(value), block(nullptr){};
    explicit HIRValue(const Value& value, HIRBlock* block);

    void Use(Inst* inst, u8 idx);
    void UnUse(Inst* inst, u8 idx);

    [[nodiscard]] u16 GetOrderId() const;

    // for rbtree compare
    static NOINLINE int Compare(const HIRValue& lhs, const HIRValue& rhs) {
        if (rhs.GetOrderId() > lhs.GetOrderId()) {
            return 1;
        } else if (rhs.GetOrderId() < lhs.GetOrderId()) {
            return -1;
        } else {
            return 0;
        }
    }
};

struct HIRLocal {
    Local local;
    HIRValue* current_value{};
};

#pragma pack(pop)

using HIRValueMap = IntrusiveMap<&HIRValue::map_node>;

using HIRBlockVector = std::span<HIRBlock*>;

class HIRBlock : public DataContext {
    friend class HIRFunction;
    friend class HIRValue;

public:
    explicit HIRBlock(Block* block, HIRValueMap& values, HIRPools& pools);

    template <typename... Args> Inst* CreateInst(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        inst->SetArgs(args...);
        return inst;
    }

    template <typename... Args> HIRValue *AppendInst(OpCode op, const Args&... args) {
        auto inst = new Inst(op);
        inst->SetArgs(args...);
        return AppendInst(inst);
    }

#define INST(name, ret, ...)                                                                       \
    template <typename... Args> ret name(const Args&... args) {                                    \
        return ret{AppendInst(OpCode::name, args...)};                                             \
    }
#include "ir.inc"
#undef INST

    HIRValue *AppendInst(Inst* inst);
    HIRValue *InsertFront(Inst* inst);
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

    void PushDominance(HIRBlock *block);
    void SetDominator(HIRBlock *block_) { dominator = block_; };
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
    HIRFunction *function{};
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
                         HIRPools& pools);

    template <typename... Args> Inst* AppendInst(OpCode op, const Args&... args) {
        ASSERT(current_block);
        auto inst = new Inst(op);
        inst->SetArgs(args...);
        inst->SetId(inst_order_id++);
        current_block->block->AppendInst(inst);
        AppendValue(current_block, inst);
        return inst;
    }

#define INST(name, ret, ...)                                                                       \
    template <typename... Args> ret name(const Args&... args) {                                    \
        return ret{AppendInst(OpCode::name, args...)};                                             \
    }
#include "ir.inc"
#undef INST

    HIRBlock* AppendBlock(Location start, Location end = {});
    HIRBlock *CreateOrGetBlock(Location location);
    void SetCurBlock(HIRBlock *block);
    HIRValue *AppendValue(HIRBlock *block, Inst* inst);
    void DestroyHIRValue(HIRValue* value);
    HIRBlock *GetEntryBlock();
    HIRBlock *GetCurrentBlock();
    HIRBlockVector& GetHIRBlocks();
    HIRBlockList& GetHIRBlockList();
    HIRBlockList& GetHIRBlocksRPO();
    HIRValueMap& GetHIRValues();
    HIRValue* GetHIRValue(const Value& value);
    HIRPools& GetMemPool();
    void AddEdge(HIRBlock* src, HIRBlock* dest, bool conditional = false);
    void RemoveEdge(Edge* edge);
    void MergeAdjacentBlocks(HIRBlock* left, HIRBlock* right);
    bool SplitBlock(HIRBlock* new_block, HIRBlock* old_block);
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

    u16 max_local_id{};
    Function* function;
    Location begin;
    Location end;
    u16 block_order_id{};
    u16 inst_order_id{};
    u16 value_count{};
    HIRPools& pools;

    HIRBlockVector blocks{};
    HIRBlockList block_list{};
    // Reverse Post Order
    HIRBlockList blocks_rpo{};
    HIRValueMap values{};
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

    explicit HIRBuilder(u32 func_cap = 1);

    HIRFunction *AppendFunction(Location start, Location end = {});

    HIRFunctionList& GetHIRFunctions();

#define INST(name, ret, ...)                                                                       \
    template <typename... Args> ret name(const Args&... args) {                                    \
        return ret{current_function->AppendInst(OpCode::name, args...)};                           \
    }
#include "ir.inc"
#undef INST

    void SetLocation(Location location);

    void SetCurBlock(HIRBlock *block);

    void SetCurBlock(Location location);

    ElseThen If(const terminal::If& if_);

    Vector<CaseBlock> Switch(const terminal::Switch& switch_);

    HIRBlock* LinkBlock(const terminal::LinkBlock& switch_);

    void Return();

private:
    Location GetNextLocation(const Terminal& term);

    HIRPools pools;
    HIRFunctionList hir_functions{};
    Location current_location;
    HIRFunction* current_function{};
};

}  // namespace swift::runtime::ir