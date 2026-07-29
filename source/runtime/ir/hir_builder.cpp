//
// Created by 甘尧 on 2023/9/8.
//

#include <cstdlib>
#include <cstring>
#include <utility>
#include "hir_builder.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::ir {

Edge::Edge(HIRBlock* src, HIRBlock* dest) : src_block(src), dest_block(dest) {}

HIRBlock::HIRBlock(Block* block, HIRValueMap& values, HIRPools& pools)
        : block(block), value_map(values), pools(pools) {}

HIRValue* HIRBlock::AppendInst(Inst* inst) {
    block->AppendInst(inst);
    if (function) {
        inst->SetId(function->inst_order_id++);
        return function->AppendValue(this, inst);
    }
    return nullptr;
}

HIRValue* HIRBlock::InsertFront(Inst* inst) {
    block->InsertBefore(inst, block->GetBeginInst().operator->());
    if (function) {
        inst->SetId(function->inst_order_id++);
        return function->AppendValue(this, inst);
    }
    return nullptr;
}

HIRValueMap& HIRBlock::GetHIRValues() { return value_map; }

u16 HIRBlock::GetOrderId() const { return order_id; }

void HIRBlock::AddIncomingEdge(Edge* edge) { incoming_edges.push_back(*edge); }

void HIRBlock::AddOutgoingEdge(Edge* edge) { outgoing_edges.push_back(*edge); }

void HIRBlock::AddBackEdge(HIRBlock* target) {
    auto back_edge = pools.mem_arena.Create<BackEdge>(target);
    back_edges.push_back(*back_edge);
}

void HIRBlock::PushDominance(HIRBlock* hir_block) {
    auto dominance = pools.mem_arena.Create<Dominance>(hir_block);
    dom_frontier.push_back(*dominance);
}

bool HIRBlock::HasIncomingEdges() { return !incoming_edges.empty(); }

bool HIRBlock::HasOutgoingEdges() { return !outgoing_edges.empty(); }

Block* HIRBlock::GetBlock() { return block; }

InstList& HIRBlock::GetInstList() { return block->GetInstList(); }

u16 HIRBlock::MaxInstrCount() { return 0; }
u16 HIRBlock::MaxBlockCount() { return 0; }
u16 HIRBlock::MaxLocalCount() { return 0; }

HIRValue::HIRValue(const Value& value, HIRBlock* block) : value(value), block(block) {}

void HIRValue::Use(Inst* inst, u8 idx) {
    auto use = block->pools.uses.Create(inst, idx);
    uses.push_back(*use);
}

void HIRValue::UnUse(Inst* inst, u8 idx) {
    auto itr = std::find_if(uses.begin(), uses.end(), [inst, idx](auto& itr) -> auto {
        return itr.inst == inst && itr.arg_idx == idx;
    });
    if (itr != uses.end()) {
        uses.erase(itr);
    }
}

u16 HIRValue::GetOrderId() const { return value.Def()->Id(); }

HIRUse::HIRUse(Inst* inst, u8 arg_idx) : inst(inst), arg_idx(arg_idx) {}

HIRFunction::HIRFunction(Function* function,
                         const Location& begin,
                         const Location& end,
                         HIRPools& pools,
                         bool ir_fast)
        : function(function), begin(begin), end(end), ir_fast(ir_fast), pools(pools) {
    if (ir_fast) {
        // Most lazy-compiled units fit in 64 instructions. Avoid the geometric
        // 1/2/4/... growth of the append-time dense id table on that common
        // path. reid_scratch is intentionally not reserved here: IdByRPO knows
        // its exact final size and already reserves once.
        values.reserve(64);
    }
    entry_block = AppendBlock(Location::INVALID);
    auto first_block = AppendBlock(begin);
    AddEdge(entry_block, first_block);
    entry_block->block->SetTerminal(terminal::LinkBlock{begin});
    current_block = first_block;
}

HIRFunction::~HIRFunction() {
    if (owns_function) {
        delete function;
    }
}

HIRBlock* HIRFunction::AppendBlock(Location start, Location end_) {
    if (auto hir_block = CreateOrGetBlock(start); hir_block) {
        if (end_ > start) {
            hir_block->block->SetEndLocation(end_);
        }
        return hir_block;
    } else {
        return nullptr;
    }
}

void HIRFunction::SetCurBlock(HIRBlock* block) { current_block = block; }

HIRValue* HIRFunction::AppendValue(HIRBlock* hir_block, Inst* inst) {
    ASSERT(hir_block);
    const bool detail = PerfIRDetailEnabled();
    auto begin = detail ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
    HIRValue* hir_value = AppendValueRecord(hir_block, inst);
    if (detail) {
        const auto end = std::chrono::steady_clock::now();
        PerfIRDetailRecord(GetPerfStats2().ir_value, begin, end);
        begin = end;
    }
    UseInst(inst);
    if (detail) {
        const auto end = std::chrono::steady_clock::now();
        PerfIRDetailRecord(GetPerfStats2().ir_use, begin, end);
    }
    TrackLocal(inst);
    return hir_value;
}

HIRValue* HIRFunction::AppendValueRecord(HIRBlock* hir_block, Inst* inst) {
    HIRValue* hir_value{};
    if (inst->HasValue()) {
        hir_value = pools.values.Create(Value{inst}, hir_block);
        value_count++;
    }
    // `values` is indexed by instruction id. Every caller today assigns the
    // next dense id immediately before calling here, so the push_back is the
    // only path taken -- replacing this whole block with a bare push_back
    // survives the suites unchanged. The indexed form is kept anyway because
    // "values[i] is the value defined by instruction i" is the contract the
    // table's other users read it under, not an assumption about callers; the
    // cost is one perfectly predicted compare.
    const auto id = inst->Id();
    if (id == values.size()) {
        values.push_back(hir_value);
    } else {
        if (values.size() <= id) {
            values.resize(id + 1, nullptr);
        }
        values[id] = hir_value;
    }
    return hir_value;
}

void HIRFunction::TrackLocal(Inst* inst) {
    switch (inst->GetOp()) {
        case OpCode::StoreLocal:
        case OpCode::LoadLocal:
        case OpCode::DefineLocal: {
            auto local = inst->GetArg<Local>(0);
            max_local_id = std::max(local.id, max_local_id);
            break;
        }
        default:
            break;
    }
}

void HIRFunction::DestroyHIRValue(HIRValue* value) {
    auto* def = value->value.Def();
    // Clear the id slot before DestroyInst frees the instruction the id lives
    // in. Guarded on identity so a stale HIRValue can never blank a slot that
    // has since been handed to another instruction.
    const auto id = def->Id();
    if (id < values.size() && values[id] == value) {
        values[id] = nullptr;
    }
    auto block = value->block->block;
    block->DestroyInst(def);
    value_count--;
}

HIRBlock* HIRFunction::GetEntryBlock() { return entry_block; }

HIRBlock* HIRFunction::GetCurrentBlock() { return current_block; }

HIRBlockVector& HIRFunction::GetHIRBlocks() { return blocks; }

HIRBlockList& HIRFunction::GetHIRBlockList() { return block_list; }

HIRBlockList& HIRFunction::GetHIRBlocksRPO() { return blocks_rpo; }

HIRValueMap& HIRFunction::GetHIRValues() { return values; }

HIRValue* HIRFunction::GetHIRValue(const Value& value) {
    auto* def = value.Def();
    if (!def) {
        return {};
    }
    // invalid_id is checked explicitly rather than relying on it being out of
    // range: it is UINT16_MAX, and ids are u16, so a maximally sized unit could
    // otherwise make it a legal index.
    const auto id = def->Id();
    if (id == Inst::invalid_id || id >= values.size()) {
        return {};
    }
    return values[id];
}

HIRPools& HIRFunction::GetMemPool() { return pools; }

Function* HIRFunction::GetFunction() {
    return function;
}

void HIRFunction::ReleaseFunctionOwnership() {
    owns_function = false;
}

void HIRFunction::AddEdge(HIRBlock* src, HIRBlock* dest, bool conditional) {
    ASSERT(src && dest);
    bool dest_was_dominated = dest->HasIncomingEdges();
    auto edge = pools.edges.Create(src, dest);
    if (conditional) {
        edge->flags |= Edge::CONDITIONAL;
    }
    src->AddOutgoingEdge(edge);
    dest->AddIncomingEdge(edge);
    if (dest_was_dominated) {
        for (auto& incoming : dest->GetIncomingEdges()) {
            incoming.flags &= ~Edge::DOMINATES;
        }
    }
}

void HIRFunction::RemoveEdge(Edge* edge) {}

void HIRFunction::MergeAdjacentBlocks(HIRBlock* left, HIRBlock* right) {}

bool HIRFunction::SplitBlock(HIRBlock* new_block, HIRBlock* old_block) { return false; }

void HIRFunction::ComputeRPO() {
    blocks_rpo.clear();
    if (!entry_block) {
        return;
    }
    // Iterative DFS over successors producing a post-order, then reverse it.
    // Successors (not predecessors) are walked so the ordering is a valid
    // forward layout for emission and linear-scan liveness.
    //
    // The visited set is a dense flag array over HIRBlock::order_id rather than
    // the Set<HIRBlock*> this used to use: that was a red-black tree with a
    // node allocation per block, and under lazy function compilation a unit has
    // ~2-5 blocks, so the container cost dominated the traversal it was
    // serving. The three scratch containers are small_vectors for the same
    // reason -- a typical unit now touches the heap zero times here.
    StackVector<u8, 32> visited{};
    visited.resize(MaxBlockCount(), 0);
    StackVector<HIRBlock*, 32> post_order{};
    struct Frame {
        HIRBlock* block;
        u32 next_succ;
    };
    StackVector<Frame, 32> stack{};
    auto mark_visited = [&visited](HIRBlock* block) -> bool {
        const auto id = block->GetOrderId();
        if (id >= visited.size()) {
            visited.resize(id + 1, 0);
        }
        if (visited[id]) {
            return false;
        }
        visited[id] = 1;
        return true;
    };
    stack.push_back({entry_block, 0});
    mark_visited(entry_block);
    while (!stack.empty()) {
        auto& frame = stack.back();
        auto successors = frame.block->GetSuccessors();
        if (frame.next_succ < successors.size()) {
            auto* succ = successors[frame.next_succ++];
            if (mark_visited(succ)) {
                stack.push_back({succ, 0});
            }
        } else {
            post_order.push_back(frame.block);
            stack.pop_back();
        }
    }
    // Reverse → RPO; drop the synthetic entry block (no guest instructions).
    for (auto it = post_order.rbegin(); it != post_order.rend(); ++it) {
        if (*it != entry_block) {
            blocks_rpo.push_back(**it);
        }
    }
}

void HIRFunction::IdByRPO() {
    // Renumber every instruction 0..N-1 in RPO and rebuild the id-indexed value
    // table in the same walk: reading a value through its *old* id and pushing
    // it at its *new* one is exactly a permutation, so no lookup structure has
    // to be re-keyed. (The previous tree form erased and re-inserted every
    // value here, twice per unit, because its key was the id being changed.)
    //
    // Values defined in blocks outside the RPO -- unreachable ones -- drop out
    // rather than being left behind with a now-stale id that could alias a
    // renumbered instruction's slot.
    reid_scratch.clear();
    reid_scratch.reserve(values.size());
    u32 cur_inst_id{0};
    for (auto& block : GetHIRBlocksRPO()) {
        for (auto& inst : block.GetInstList()) {
            reid_scratch.push_back(GetHIRValue(Value{&inst}));
            inst.SetId(cur_inst_id++);
        }
    }
    inst_order_id = cur_inst_id;
    values.swap(reid_scratch);
}

void HIRFunction::EndBlock(Terminal terminal) {
    current_block->block->SetTerminal(std::move(terminal));
    current_block = {};
}

void HIRFunction::EndFunction() {
    const bool detail = PerfIRDetailEnabled();
    auto begin = detail ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
    blocks = pools.CreateBlockVector(MaxBlockCount());
    for (auto& block : block_list) {
        // Function vector
        blocks[block.order_id] = &block;

        // Block successes predecessors
        auto& incoming_edges = block.GetIncomingEdges();
        auto& outgoing_edges = block.GetOutgoingEdges();
        block.predecessors = pools.CreateBlockVector(incoming_edges.size());
        block.successors = pools.CreateBlockVector(outgoing_edges.size());

        u16 incoming_index{0};
        for (auto& edge : incoming_edges) {
            block.predecessors[incoming_index++] = edge.src_block;
        }

        u16 outgoing_index{0};
        for (auto& edge : outgoing_edges) {
            block.successors[outgoing_index++] = edge.dest_block;
        }

        // Transfer every real decoded block to the persistent ir::Function.
        // The HIR wrappers live in HIRBuilder's pools, but ir::Function owns
        // the Block objects after the builder goes away. The synthetic entry
        // has Location::INVALID and exists only to seed CFG/RPO discovery.
        if (&block != entry_block) {
            function->AddBlock(block.GetBlock());
        }
    }
    block_list.clear();
    if (detail) {
        PerfIRDetailRecord(
                GetPerfStats2().ir_finish_blocks, begin, std::chrono::steady_clock::now());
    }
}

u16 HIRFunction::MaxBlockCount() { return block_order_id; }
u16 HIRFunction::MaxInstrCount() { return inst_order_id; }
u16 HIRFunction::MaxLocalCount() { return max_local_id + 1; }

void HIRFunction::EraseInst(Block* block, Inst* inst) {
    ASSERT(block && inst);
    // Order matters: drop the uses this instruction holds while its args are
    // still intact, then remove the value it defines, then free it.
    UnUseInst(inst);
    if (auto* hir_value = GetHIRValue(Value{inst}); hir_value) {
        // DestroyHIRValue also unlinks the instruction from its block and
        // deletes it (Block::DestroyInst).
        DestroyHIRValue(hir_value);
        return;
    }
    block->DestroyInst(inst);
}

// Exact mirror of UseInst: same logical/physical slot walk, unregistering
// instead of registering. Keep the two in lockstep.
void HIRFunction::UnUseInst(Inst* inst) {
    const auto& info = GetIRMetaInfo(inst->GetOp());
    u8 physical = 0;
    for (const auto arg_type : info.arg_types) {
        switch (arg_type) {
            case ArgType::Value: {
                auto& arg = inst->ArgAt(physical);
                if (!arg.IsValue()) {
                    break;
                }
                if (auto hir_value = GetHIRValue(arg.Get<Value>()); hir_value) {
                    hir_value->UnUse(inst, physical);
                }
                break;
            }
            case ArgType::Lambda: {
                auto& arg = inst->ArgAt(physical);
                if (!arg.IsLambda()) {
                    break;
                }
                auto lambda = arg.Get<Lambda>();
                if (lambda.IsValue()) {
                    if (auto hir_value = GetHIRValue(lambda.GetValue()); hir_value) {
                        hir_value->UnUse(inst, physical);
                    }
                }
                break;
            }
            case ArgType::Params: {
                auto& arg = inst->ArgAt(physical);
                if (!arg.IsParams()) {
                    break;
                }
                for (auto param : arg.Get<Params>()) {
                    if (auto data = param.data; data.IsValue()) {
                        if (auto hir_value = GetHIRValue(data.value); hir_value) {
                            hir_value->UnUse(inst, HIRUse::USE_FUNC_CALL);
                        }
                    }
                }
                break;
            }
            case ArgType::Operand: {
                for (u8 nested = 1; nested <= 2; ++nested) {
                    auto& data = inst->ArgAt(physical + nested);
                    if (data.IsValue()) {
                        if (auto hir_value = GetHIRValue(data.Get<Value>()); hir_value) {
                            hir_value->UnUse(inst, physical + nested);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
        physical += PhysicalSlots(arg_type);
    }
}

void HIRFunction::UseInst(Inst* inst) {
    // HIRUse::arg_idx is a physical Arg slot (the local-elimination passes feed
    // it back to ArgAt/SetArg). Walk logical metadata while carrying the
    // physical offset explicitly; Operand consumes op + left + right slots.
    const auto& info = GetIRMetaInfo(inst->GetOp());
    u8 physical = 0;
    for (const auto arg_type : info.arg_types) {
        switch (arg_type) {
            case ArgType::Value: {
                auto& arg = inst->ArgAt(physical);
                if (inst->GetOp() == OpCode::CallLambda && arg.IsVoid()) {
                    break;  // optional trailing host-call argument
                }
                ASSERT_MSG(arg.IsValue(),
                           "{} physical arg {} is {}, expected Value",
                           inst->GetOp(),
                           physical,
                           arg.GetType());
                auto value = arg.Get<Value>();
                if (auto hir_value = GetHIRValue(value); hir_value) {
                    hir_value->Use(inst, physical);
                }
                break;
            }
            case ArgType::Lambda: {
                auto lambda = inst->ArgAt(physical).Get<Lambda>();
                if (lambda.IsValue()) {
                    if (auto hir_value = GetHIRValue(lambda.GetValue()); hir_value) {
                        hir_value->Use(inst, physical);
                    }
                }
                break;
            }
            case ArgType::Params: {
                auto params = inst->ArgAt(physical).Get<Params>();
                for (auto param : params) {
                    if (auto data = param.data; data.IsValue()) {
                        if (auto hir_value = GetHIRValue(data.value); hir_value) {
                            hir_value->Use(inst, HIRUse::USE_FUNC_CALL);
                        }
                    }
                }
                break;
            }
            case ArgType::Operand: {
                for (u8 nested = 1; nested <= 2; ++nested) {
                    auto& data = inst->ArgAt(physical + nested);
                    if (data.IsValue()) {
                        if (auto hir_value = GetHIRValue(data.Get<Value>()); hir_value) {
                            hir_value->Use(inst, physical + nested);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
        physical += PhysicalSlots(arg_type);
    }
}

HIRBlock* HIRFunction::CreateOrGetBlock(Location location) {
    auto itr = std::find_if(block_list.begin(), block_list.end(), [location](auto& block) -> auto {
        return block.GetBlock()->GetStartLocation() == location;
    });
    if (itr != block_list.end()) {
        return itr.operator->();
    }
    auto hir_block = pools.blocks.Create(new Block(location), values, pools);
    hir_block->order_id = block_order_id++;
    hir_block->function = this;
    block_list.push_back(*hir_block);
    return hir_block;
}

HIRPools::HIRPools(u32 func_cap)
        : functions(func_cap)
        , blocks(func_cap * 8)
        , edges(func_cap * 16)
        , values(func_cap * 256)
        , uses(func_cap * 256)
        , mem_arena(func_cap * 512) {}

namespace {
// The per-thread pool set the lease hands out, plus the flag that makes a
// nested lease fall back to a private one. A unique_ptr rather than a plain
// object so the pools are released at thread exit; by then Reset() has already
// destroyed every object in them.
thread_local std::unique_ptr<HIRPools> tls_pools{};
thread_local bool tls_pools_in_use{false};
}  // namespace

HIRPoolLease::HIRPoolLease(u32 func_cap) {
    if (tls_pools_in_use) {
        owned = std::make_unique<HIRPools>(func_cap);
        pools = owned.get();
        return;
    }
    if (!tls_pools) {
        // func_cap only sizes the initial chunks; a cached set reached by a
        // later builder with a larger cap simply grows on demand.
        tls_pools = std::make_unique<HIRPools>(func_cap);
    }
    tls_pools_in_use = true;
    pools = tls_pools.get();
}

HIRPoolLease::~HIRPoolLease() {
    if (owned) {
        return;
    }
    pools->Reset();
    tls_pools_in_use = false;
}

HIRBuilder::HIRBuilder(u32 func_cap, bool defer_function_end, bool ir_fast)
        : pool_lease(func_cap), pools(pool_lease.Get()),
          defer_function_end(defer_function_end), ir_fast(ir_fast) {}

HIRFunction* HIRBuilder::AppendFunction(Location start, Location end) {
    current_function =
            pools.functions.Create(new Function(start), start, end, pools, ir_fast);
    hir_functions.push_back(*current_function);
    SetCurBlock(current_function->AppendBlock(start, end));
    return current_function;
}

HIRFunctionList& HIRBuilder::GetHIRFunctions() { return hir_functions; }

bool HIRBuilder::AdvancePCCoalesceEnabled() {
    // Escape hatch for bisection; see the header for what this does. Cached in
    // a member by the constructor so the guard-variable load stays off the
    // per-instruction path.
    static const bool enabled = [] {
        const char* env = PerfGetenv("SVM_ADVPC_COALESCE");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

bool HIRBuilder::FoldAdvancePC(const Imm& imm) {
    // A flag producer since the last boundary makes this AdvancePC a real
    // flush point.
    if (flags_since_advance) {
        return false;
    }
    // Nothing to fold into: the first AdvancePC of a block is always kept, so
    // a block that has any AdvancePC at all keeps at least one.
    if (!last_advance || last_advance_block != current_function->GetCurrentBlock()) {
        return false;
    }
    const auto prev = last_advance->GetArg<Imm>(0);
    last_advance->SetArg(0, Imm{prev.Get() + imm.Get(), prev.GetType()});
    return true;
}

void HIRBuilder::SetLocation(Location location) { current_location = location; }

void HIRBuilder::SetCurBlock(HIRBlock* block) {
    ASSERT(current_function);
    current_function->SetCurBlock(block);
}

void HIRBuilder::SetCurBlock(Location location) {
    ASSERT(current_function);
    auto block = current_function->CreateOrGetBlock(location);
    ASSERT(block);
    current_function->SetCurBlock(block);
}

HIRBuilder::ElseThen HIRBuilder::If(const terminal::If& if_) {
    ASSERT_MSG(current_function, "current function is null!");
    auto pre_block = current_function->current_block;
    current_function->EndBlock(if_);
    auto else_ = GetNextLocation(if_.else_);
    auto then_ = GetNextLocation(if_.then_);
    auto else_block = current_function->AppendBlock(else_);
    if (else_block) {
        current_function->AddEdge(pre_block, else_block, true);
    }
    auto then_block = current_function->AppendBlock(then_);
    if (then_block) {
        current_function->AddEdge(pre_block, then_block, true);
    }
    return {else_block, then_block};
}

Vector<HIRBuilder::CaseBlock> HIRBuilder::Switch(const terminal::Switch& switch_) {
    ASSERT_MSG(current_function, "current function is null!");
    auto pre_block = current_function->current_block;
    current_function->EndBlock(switch_);
    auto case_size = switch_.cases.size();
    Vector<HIRBuilder::CaseBlock> result{case_size};
    for (int i = 0; i < case_size; i++) {
        auto next_location = GetNextLocation(switch_.cases[i].then);
        auto next_block = current_function->AppendBlock(next_location);
        current_function->AddEdge(pre_block, next_block, true);
        result[i] = {switch_.cases[i].case_value, next_block};
    }
    return result;
}

HIRBlock* HIRBuilder::LinkBlock(const terminal::LinkBlock& link) {
    ASSERT_MSG(current_function, "current function is null!");
    auto pre_block = current_function->current_block;
    current_function->EndBlock(link);
    auto next_block = current_function->AppendBlock(link.next);
    current_function->AddEdge(pre_block, next_block, false);
    return next_block;
}

// A function-ending terminal clears current_function. The x86 decoder can emit
// a second one after the function already ended (e.g. `hlt` after an exit
// syscall); in function mode that is dead, so treat it as a no-op instead of
// asserting (the translator driver falls back to block compilation for such
// complex functions).

void HIRBuilder::ReturnToDispatcher() {
    if (!current_function) {
        return;
    }
    current_function->EndBlock(ir::terminal::ReturnToDispatch{});
    if (defer_function_end) {
        return;
    }
    current_function->EndFunction();
    current_function = {};
}

void HIRBuilder::ReturnToHost() {
    if (!current_function) {
        return;
    }
    current_function->EndBlock(ir::terminal::ReturnToHost{});
    if (defer_function_end) {
        return;
    }
    current_function->EndFunction();
    current_function = {};
}

void HIRBuilder::Return() {
    if (!current_function) {
        return;
    }
    current_function->EndBlock(terminal::PopRSBHint{});
    if (defer_function_end) {
        return;
    }
    current_function->EndFunction();
    current_function = {};
}

void HIRBuilder::End() {
    if (!current_function) {
        return;
    }
    current_function->EndBlock(terminal::ReturnToDispatch{});
    current_function->EndFunction();
    current_function = {};
}

Location HIRBuilder::GetNextLocation(const terminal::Terminal& term) {
    return VisitVariant<Location>(term, [](auto x) -> Location {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, terminal::LinkBlock>) {
            return x.next;
        } else if constexpr (std::is_same_v<T, terminal::LinkBlockFast>) {
            return x.next;
        } else {
            return {};
        }
    });
}

void DfsHIRBlock(HIRBlock* start, HIRBlock* end, HIRBlockSet& visited) {
    if (start == end || visited.count(start)) {
        return;
    }
    visited.insert(start);
    for (auto pred : start->GetPredecessors()) {
        DfsHIRBlock(pred, end, visited);
    }
}

}  // namespace swift::runtime::ir
