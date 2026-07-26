//
// Created by 甘尧 on 2023/9/8.
//

#include <utility>
#include "hir_builder.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::ir {

Edge::Edge(HIRBlock* src, HIRBlock* dest) : src_block(src), dest_block(dest) {}

HIRLoop* HIRLoop::Create(HIRFunction* function, HIRBlock* header, size_t length) {
    return function->pools.mem_arena.Create<HIRLoop>(function, header, length);
}

HIRLoop::HIRLoop(HIRFunction* function, HIRBlock* header, size_t length) {
    loop = function->pools.CreateBlockVector(length);
    std::memcpy(loop.data(), (void*)header, sizeof(HIRBlock*) * length);
}

HIRBlock* HIRLoop::GetHeader() const { return loop[0]; }

HIRBlockVector HIRLoop::GetLoopVector() const { return loop; }

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
                         HIRPools& pools)
        : function(function), begin(begin), end(end), pools(pools) {
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
    HIRValue* hir_value{};
    if (inst->HasValue()) {
        hir_value = pools.values.Create(Value{inst}, hir_block);
        values.insert(*hir_value);
        value_count++;
    }
    UseInst(inst);
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
    return hir_value;
}

void HIRFunction::DestroyHIRValue(HIRValue* value) {
    values.erase(*value);
    auto block = value->block->block;
    block->DestroyInst(value->value.Def());
    value_count--;
}

HIRBlock* HIRFunction::GetEntryBlock() { return entry_block; }

HIRBlock* HIRFunction::GetCurrentBlock() { return current_block; }

HIRBlockVector& HIRFunction::GetHIRBlocks() { return blocks; }

HIRBlockList& HIRFunction::GetHIRBlockList() { return block_list; }

HIRBlockList& HIRFunction::GetHIRBlocksRPO() { return blocks_rpo; }

HIRLoopList& HIRFunction::GetHIRLoop() { return loops; }

HIRValueMap& HIRFunction::GetHIRValues() { return values; }

HIRValue* HIRFunction::GetHIRValue(const Value& value) {
    if (auto itr = values.find(value); itr != values.end()) {
        return itr.operator->();
    } else {
        return {};
    }
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

void HIRFunction::AddLoop(HIRLoop* loop) { loops.push_back(*loop); }

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
    HIRBlockSet visited{};
    Vector<HIRBlock*> post_order{};
    post_order.reserve(MaxBlockCount());
    struct Frame {
        HIRBlock* block;
        size_t next_succ;
    };
    Vector<Frame> stack{};
    stack.push_back({entry_block, 0});
    visited.insert(entry_block);
    while (!stack.empty()) {
        auto& frame = stack.back();
        auto successors = frame.block->GetSuccessors();
        if (frame.next_succ < successors.size()) {
            auto* succ = successors[frame.next_succ++];
            if (visited.insert(succ).second) {
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
    u32 cur_inst_id{0};
    // Re id inst
    StackVector<HIRValue*, 32> function_values{};
    function_values.reserve(value_count);
    for (auto& block : GetHIRBlocksRPO()) {
        for (auto& inst : block.GetInstList()) {
            if (auto value = GetHIRValue(&inst); value) {
                function_values.push_back(value);
                values.erase(*value);
            }
            inst.SetId(cur_inst_id++);
        }
    }
    inst_order_id = cur_inst_id;
    // Re insert map
    for (auto value : function_values) {
        values.insert(*value);
    }
}

void HIRFunction::EndBlock(Terminal terminal) {
    current_block->block->SetTerminal(std::move(terminal));
    current_block = {};
}

void HIRFunction::EndFunction() {
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

HIRBuilder::HIRBuilder(u32 func_cap, bool defer_function_end)
        : pools(func_cap), defer_function_end(defer_function_end) {}

HIRFunction* HIRBuilder::AppendFunction(Location start, Location end) {
    current_function = pools.functions.Create(new Function(start), start, end, pools);
    hir_functions.push_back(*current_function);
    SetCurBlock(current_function->AppendBlock(start, end));
    return current_function;
}

HIRFunctionList& HIRBuilder::GetHIRFunctions() { return hir_functions; }

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
