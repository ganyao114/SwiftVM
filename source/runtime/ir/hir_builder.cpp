//
// Created by 甘尧 on 2023/9/8.
//

#include <cstdlib>
#include <cstring>
#include <utility>
#include "hir_builder.h"
#include "hir_pools.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::ir {

Edge::Edge(const HIRBlock* src, const HIRBlock* dest) : src_block(src), dest_block(dest) {}

HIRBlock::HIRBlock(std::unique_ptr<Block> block, HIRValueMap& values, HIRPools& pools)
        : block(block.get()), unpublished_block(std::move(block)), value_map(values), pools(pools) {}

HIRValue* HIRBlock::AppendInst(Inst* inst) {
    block->CheckMutable();
    ASSERT(inst && !inst->GetBlock());
    auto pending = std::unique_ptr<Inst>{inst};
    block->AppendInst(inst);
    pending.release();
    if (function) {
        inst->SetId(function->inst_order_id++);
        return function->AppendValue(this, inst);
    }
    return nullptr;
}

HIRValue* HIRBlock::InsertFront(Inst* inst) {
    block->InsertBefore(inst,
                        block->GetInstList().empty() ? nullptr : &block->GetInstList().front());
    if (function) {
        inst->SetId(function->inst_order_id++);
        return function->AppendValue(this, inst);
    }
    return nullptr;
}

std::span<const HIRValue* const> HIRBlock::GetHIRValues() const { return value_map; }

u16 HIRBlock::GetOrderId() const { return order_id; }

bool HIRBlock::HasIncomingEdges() const { return !incoming_edges.empty(); }

bool HIRBlock::HasOutgoingEdges() const { return !outgoing_edges.empty(); }

Block* HIRBlock::GetBlock() { return block; }
const Block* HIRBlock::GetBlock() const { return block; }

HIRPoolLease HIRBlock::RetainStorage() const { return HIRPoolLease::Retain(pools); }

const InstList& HIRBlock::GetInstList() const { return std::as_const(*block).GetInstList(); }

bool HIRBlock::BeginStandalonePublication() {
    block->CheckMutable();
    if (!function || function->function_published || !function->block_list.empty() ||
        standalone_publication_pending || !function->function->DetachBlock(block)) {
        return false;
    }
    standalone_publication_pending = true;
    return true;
}

void HIRBlock::AbortStandalonePublication() {
    block->CheckMutable();
    ASSERT(standalone_publication_pending);
    function->function->AddBlock(block);
    standalone_publication_pending = false;
}

void HIRBlock::CommitStandalonePublication() {
    block->CheckMutable();
    ASSERT(standalone_publication_pending);
    function->standalone_blocks_published = true;
    standalone_publication_pending = false;
}

HIRValue::HIRValue(const Value& value, HIRBlock* block) : value(value), block(block) {}

void HIRValue::Use(const Inst* inst, u8 idx) {
    ASSERT(block && !block->GetBlock()->IsIRReadOnly());
    auto use = block->pools.uses.Create(inst, idx);
    uses.push_back(*use);
}

void HIRValue::UnUse(const Inst* inst, u8 idx) {
    ASSERT(block && !block->GetBlock()->IsIRReadOnly());
    auto itr = std::find_if(uses.begin(), uses.end(), [inst, idx](auto& itr) -> auto {
        return itr.inst == inst && itr.arg_idx == idx;
    });
    if (itr != uses.end()) {
        uses.erase(itr);
    }
}

u16 HIRValue::GetOrderId() const { return value.Def()->Id(); }

HIRUse::HIRUse(const Inst* inst, u8 arg_idx) : inst(inst), arg_idx(arg_idx) {}

HIRFunction::HIRFunction(std::unique_ptr<Function> function,
                         const Location& begin,
                         const Location& end,
                         HIRPools& pools,
                         bool ir_fast)
        : function(function.release()), begin(begin), end(end), ir_fast(ir_fast), pools(pools) {
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
    // Another region may still seal the actual persistent parent. Only this
    // HIR wrapper's own consumers must have ended before its pool is reset.
    if (entry_block) entry_block->GetBlock()->CheckMutable();
}

HIRBlock* HIRFunction::AppendBlock(Location start, Location end_) {
    CheckMutable();
    if (auto hir_block = CreateOrGetBlock(start); hir_block) {
        if (end_ > start) {
            hir_block->block->SetEndLocation(end_);
        }
        return hir_block;
    } else {
        return nullptr;
    }
}

void HIRFunction::SetCurBlock(const HIRBlock* block) { current_block = &EditBlock(block); }

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
    CheckMutable();
    HIRValue* hir_value{};
    if (inst->HasValue() && inst->ReturnType() != ValueType::VOID) {
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
    CheckMutable();
    auto* def = value->value.Def();
    // Clear the id slot before DestroyInst frees the instruction the id lives
    // in. Guarded on direct so a stale HIRValue can never blank a slot that
    // has since been handed to another instruction.
    const auto id = def->Id();
    if (id < values.size() && values[id] == value) {
        values[id] = nullptr;
    }
    auto block = value->block->block;
    block->DestroyInst(def);
    value_count--;
}

const HIRBlock* HIRFunction::GetEntryBlock() const { return entry_block; }

const HIRBlock* HIRFunction::GetCurrentBlock() const { return current_block; }

HIRBlockView HIRFunction::GetHIRBlocks() const { return blocks; }

const HIRBlockList& HIRFunction::GetHIRBlockList() const { return block_list; }

const HIRBlockList& HIRFunction::GetHIRBlocksRPO() const { return blocks_rpo; }

std::span<const HIRValue* const> HIRFunction::GetHIRValues() const { return values; }

const HIRValue* HIRFunction::GetHIRValue(const Value& value) const {
    return FindValueRecord(value);
}
HIRValue* HIRFunction::FindValueRecord(const Value& value) const {
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
    auto* record = values[id];
    return record && record->value.Def() == def ? record : nullptr;
}

HIRBlock& HIRFunction::EditBlock(const HIRBlock* block) {
    CheckMutable();
    ASSERT(block && block->function == this);
    block->GetBlock()->CheckMutable();
    return *const_cast<HIRBlock*>(block);
}

void HIRFunction::SetIRReadOnly(bool read_only) {
    ASSERT(blocks.size() > 1 && block_list.empty());
    auto* owner = blocks[1]->block->owning_function;
    ASSERT(owner);
    if (read_only) {
        CheckMutable();
        for (auto* hir : blocks) {
            if (!hir) continue;
            hir->block->CheckMutable();
            ASSERT(hir->unpublished_block || hir->block->owning_function == owner);
        }
        if (owner != function.get()) owner->AcquireIRReadOnly();
        function->AcquireIRReadOnly();
    } else {
        ASSERT(entry_block->block->IsIRReadOnly());
        ASSERT(function->IsIRReadOnly() && owner->IsIRReadOnly());
    }
    for (auto* hir : blocks)
        if (hir) hir->block->instruction_owner.sealed = read_only;
    if (!read_only) {
        function->ReleaseIRReadOnly();
        if (owner != function.get()) owner->ReleaseIRReadOnly();
    }
}

HIRPoolLease HIRFunction::RetainStorage() const { return HIRPoolLease::Retain(pools); }

Function* HIRFunction::GetFunction() { return function.get(); }
const Function* HIRFunction::GetFunction() const { return function.get(); }

void HIRFunction::MarkFunctionPublished() { function_published = true; }

void HIRFunction::AddEdge(const HIRBlock* src, const HIRBlock* dest) {
    auto& source = EditBlock(src);
    auto& target = EditBlock(dest);
    auto* edge = pools.edges.Create(src, dest);
    source.outgoing_edges.push_back(*edge);
    target.incoming_edges.push_back(*edge);
}

void HIRFunction::RegisterCallReturn(Location location) {
    CheckMutable();
    auto& source = EditBlock(current_block);
    auto* target = AppendBlock(location);
    ASSERT(target && !source.call_return_block);
    source.call_return_block = target;
    target->call_return_target = true;
}

void HIRFunction::RemoveEdge(const Edge* edge) {
    CheckMutable();
    ASSERT(edge);
    auto& source = EditBlock(edge->GetSourceBlock());
    auto& target = EditBlock(edge->GetTargetBlock());
    source.outgoing_edges.erase(*edge);
    target.incoming_edges.erase(*edge);
}

void HIRFunction::ReleaseTerminalUses(const Terminal& terminal) {
    VisitVariant<void>(terminal, [](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, terminal::If>) {
            term.cond.UnUse();
            ReleaseTerminalUses(term.then_);
            ReleaseTerminalUses(term.else_);
        } else if constexpr (std::is_same_v<T, terminal::Switch>) {
            term.value.UnUse();
            for (const auto& arm : term.cases) {
                ReleaseTerminalUses(arm.then);
            }
        } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
            ReleaseTerminalUses(term.else_);
        }
    });
}

bool HIRFunction::ResetDecodedBlock(const HIRBlock* identity) {
    auto* hir_block = &EditBlock(identity);
    ASSERT(hir_block && hir_block->function == this);
    if (hir_block->call_return_block) {
        auto* return_block = hir_block->call_return_block;
        for (auto& candidate : block_list) {
            if (&candidate != hir_block &&
                candidate.call_return_block == return_block) {
                return false;
            }
        }
        return_block->call_return_target = false;
        hir_block->call_return_block = nullptr;
    }

    std::erase_if(external_direct_links,
                  [hir_block](const auto& link) {
                      return link.source == hir_block;
                  });

    auto* block = hir_block->block;
    ReleaseTerminalUses(block->GetTerminal());
    block->SetTerminal(terminal::Invalid{});
    while (!hir_block->outgoing_edges.empty()) {
        RemoveEdge(&hir_block->outgoing_edges.front());
    }

    Vector<const Inst*> instructions;
    instructions.reserve(block->GetInstList().size());
    for (auto& inst : block->GetInstList()) {
        instructions.push_back(&inst);
    }
    for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
        EraseInst(block, *it);
    }
    block->ClearGuestCodeDependencies();
    current_block = hir_block;
    return true;
}

void HIRFunction::RegisterExternalDirectLink(Location target) {
    CheckMutable();
    ASSERT(current_block);
    external_direct_links.push_back({current_block, target});
}

void HIRFunction::RegisterExternalEntryRoot(const HIRBlock* identity) {
    auto* block = &EditBlock(identity);
    if (std::find(external_entry_roots.begin(), external_entry_roots.end(), block) ==
        external_entry_roots.end()) {
        external_entry_roots.push_back(block);
    }
}

void HIRFunction::ComputeRPO() {
    CheckMutable();
    blocks_rpo.clear();
    if (!entry_block) {
        return;
    }
    // Iterative DFS over successors producing a post-order, then reverse it.
    // Successors (not predecessors) are walked so the ordering is a valid
    // forward layout for emission and linear-scan liveness.
    //
    // The visited set is a dense flag array over HIRBlock::order_id rather than
    // the Set<const HIRBlock*> this used to use: that was a red-black tree with a
    // node allocation per block, and under lazy function compilation a unit has
    // ~2-5 blocks, so the container cost dominated the traversal it was
    // serving. The three scratch containers are small_vectors for the same
    // reason -- a typical unit now touches the heap zero times here.
    StackVector<u8, 32> visited{};
    visited.resize(MaxBlockCount(), 0);
    struct Frame {
        const HIRBlock* block;
        u32 next_succ;
    };
    StackVector<Frame, 32> stack{};
    auto mark_visited = [&visited](const HIRBlock* block) -> bool {
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
    const auto append_component = [&](const HIRBlock* root) {
        if (!root || !mark_visited(root)) {
            return;
        }
        StackVector<const HIRBlock*, 32> post_order{};
        stack.push_back({root, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            auto successors = frame.block->GetSuccessors();
            const u32 successor_count = static_cast<u32>(successors.size()) +
                    (frame.block->GetCallReturnBlock() ? 1u : 0u);
            if (frame.next_succ < successor_count) {
                auto* succ = frame.next_succ < successors.size()
                        ? successors[frame.next_succ]
                        : frame.block->GetCallReturnBlock();
                ++frame.next_succ;
                if (mark_visited(succ)) {
                    stack.push_back({succ, 0});
                }
            } else {
                post_order.push_back(frame.block);
                stack.pop_back();
            }
        }
        for (auto it = post_order.rbegin(); it != post_order.rend(); ++it) {
            if (*it != entry_block) {
                blocks_rpo.push_back(EditBlock(*it));
            }
        }
    };
    append_component(entry_block);
    for (auto* root : external_entry_roots) {
        append_component(root);
    }
}

void HIRFunction::IdByRPO() {
    CheckMutable();
    // Renumber every instruction 0..N-1 in RPO and rebuild the id-indexed value
    // table in the same walk: reading a value through its *old* id and pushing
    // it at its *new* one is exactly a permutation, so no lookup structure has
    // to be re-keyed. (The previous tree form erased and re-inserted every
    // value here, twice per unit, because its key was the id being changed.)
    //
    // Values defined in blocks outside the RPO -- unreachable ones -- drop out
    // of `values` rather than being left behind with a now-stale id that could
    // alias a renumbered instruction's slot. But the unreachable instructions
    // themselves still exist in `blocks`, and passes that iterate
    // GetHIRBlocks() (all blocks) would otherwise read inst.Id()/value.Id() as
    // a stale building id -- aliasing a renumbered instruction's alloc_result/
    // use_end slot or running past inst_order_id into an OOB write. Numbering
    // them past the reachable range keeps every instruction id unique and
    // in-bounds; the value table itself stays RPO-only, so FindValueRecord() still
    // reports unreachable defs as "not a value" (id >= values.size()).
    reid_scratch.clear();
    reid_scratch.reserve(values.size());
    StackVector<u8, 32> reachable{};
    reachable.resize(MaxBlockCount(), 0);
    u32 cur_inst_id{0};
    for (auto& block : blocks_rpo) {
        reachable[block.GetOrderId()] = 1;
        for (auto& inst : block.GetInstList()) {
            reid_scratch.push_back(FindValueRecord(Value{&inst}));
            block.GetBlock()->EditInst(&inst).SetId(cur_inst_id++);
        }
    }
    // EndFunction has already emptied the temporary intrusive list. The
    // retained vector includes the unreachable blocks that also need IDs.
    for (auto* block : blocks) {
        if (block->GetOrderId() < reachable.size() &&
            reachable[block->GetOrderId()]) {
            continue;
        }
        for (auto& inst : block->GetInstList()) {
            block->GetBlock()->EditInst(&inst).SetId(cur_inst_id++);
        }
    }
    inst_order_id = cur_inst_id;
    values.swap(reid_scratch);
}

void HIRFunction::EndBlock(Terminal terminal) {
    CheckMutable();
    current_block->block->SetTerminal(std::move(terminal));
    current_block = {};
}

void HIRFunction::EndFunction() {
    CheckMutable();
    const bool detail = PerfIRDetailEnabled();
    auto begin = detail ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
    blocks = pools.CreateBlockVector(MaxBlockCount());
    for (auto& block : block_list) {
        // Function vector
        blocks[block.order_id] = &block;

        // Block successes predecessors
        auto& incoming_edges = block.incoming_edges;
        auto& outgoing_edges = block.outgoing_edges;
        block.predecessors = pools.CreateBlockReferences(incoming_edges.size());
        block.successors = pools.CreateBlockReferences(outgoing_edges.size());

        u16 incoming_index{0};
        for (auto& edge : incoming_edges) {
            block.predecessors[incoming_index++] = edge.GetSourceBlock();
        }

        u16 outgoing_index{0};
        for (auto& edge : outgoing_edges) {
            block.successors[outgoing_index++] = edge.GetTargetBlock();
        }

        // Transfer every real decoded block to the persistent ir::Function.
        // The HIR wrappers live in HIRBuilder's pools, but ir::Function owns
        // the Block objects after the builder goes away. The synthetic entry
        // has Location::INVALID and exists only to seed CFG/RPO discovery.
        const bool decoded = block.block->HasTerminal() || !block.block->GetInstList().empty();
        if (&block != entry_block && block.unpublished_block &&
            (decoded || block.block->GetStartLocation() == function->GetStartLocation())) {
            function->AddBlock(block.GetBlock());
            block.unpublished_block.release();
        }
    }
    block_list.clear();
    if (detail) {
        PerfIRDetailRecord(
                GetPerfStats2().ir_finish_blocks, begin, std::chrono::steady_clock::now());
    }
}

u16 HIRFunction::MaxBlockCount() const { return block_order_id; }
u16 HIRFunction::MaxInstrCount() const { return inst_order_id; }
u16 HIRFunction::MaxLocalCount() const { return max_local_id + 1; }

void HIRFunction::EraseInst(Block* block, const Inst* inst) {
    CheckMutable();
    ASSERT(block && inst);
    // Order matters: drop the uses this instruction holds while its args are
    // still intact, then remove the value it defines, then free it.
    UnUseInst(inst);
    if (auto* hir_value = FindValueRecord(Value{inst}); hir_value) {
        // DestroyHIRValue also unlinks the instruction from its block and
        // deletes it (Block::DestroyInst).
        DestroyHIRValue(hir_value);
        return;
    }
    block->DestroyInst(inst);
}

// Both directions visit the same physical/nested value occurrences as SSA
// validation and ordinary IR consumers. Immediate call arguments have no use.
void HIRFunction::UnUseInst(const Inst* inst) {
    CheckMutable();
    inst->ForEachValueUse([&](Value value, u8 slot) {
        if (auto* record = FindValueRecord(value))
            record->UnUse(inst, slot == Inst::params_arg_index ? HIRUse::USE_FUNC_CALL : slot);
    });
}

void HIRFunction::UseInst(const Inst* inst) {
    CheckMutable();
    inst->ForEachValueUse([&](Value value, u8 slot) {
        if (auto* record = FindValueRecord(value))
            record->Use(inst, slot == Inst::params_arg_index ? HIRUse::USE_FUNC_CALL : slot);
    });
}

HIRBlock* HIRFunction::CreateOrGetBlock(Location location) {
    CheckMutable();
    auto itr = std::find_if(block_list.begin(), block_list.end(), [location](auto& block) -> auto {
        return block.GetBlock()->GetStartLocation() == location;
    });
    if (itr != block_list.end()) {
        return &EditBlock(itr.operator->());
    }
    auto hir_block = pools.blocks.Create(std::make_unique<Block>(location), values, pools);
    hir_block->order_id = block_order_id++;
    hir_block->block->instruction_owner.hir_order = hir_block->order_id;
    hir_block->function = this;
    block_list.push_back(*hir_block);
    return hir_block;
}

HIRBuilder::HIRBuilder(u32 func_cap, bool defer_function_end, bool ir_fast,
                       const FeatureSet& features)
        : pool_lease(func_cap), pools(pool_lease.Get()),
          defer_function_end(defer_function_end), ir_fast(ir_fast),
          advpc_coalesce(features.advpc_coalesce) {}

HIRFunction* HIRBuilder::AppendFunction(Location start, Location end) {
    current_function =
            pools.functions.Create(std::make_unique<Function>(start), start, end, pools, ir_fast);
    hir_functions.push_back(*current_function);
    SetCurBlock(current_function->AppendBlock(start, end));
    return current_function;
}

bool HIRBuilder::HasCurrentFunctionBlock(Location location) {
    if (!current_function) {
        return false;
    }
    for (auto& block : current_function->GetHIRBlockList()) {
        if (block.GetBlock()->GetStartLocation() == location) {
            return true;
        }
    }
    return false;
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

void HIRBuilder::SetCurBlock(const HIRBlock* block) {
    ASSERT(current_function);
    current_function->SetCurBlock(block);
}

void HIRBuilder::SetCurBlock(Location location) {
    ASSERT(current_function);
    auto block = current_function->CreateOrGetBlock(location);
    ASSERT(block);
    current_function->SetCurBlock(block);
}

bool HIRBuilder::ResetDecodedBlock(const HIRBlock* block) {
    ASSERT(current_function);
    if (!current_function->ResetDecodedBlock(block)) {
        return false;
    }
    current_location = block->GetBlock()->GetStartLocation();
    last_advance = nullptr;
    last_advance_block = nullptr;
    flags_since_advance = false;
    return true;
}

HIRBuilder::ElseThen HIRBuilder::If(const terminal::If& if_) {
    ASSERT_MSG(current_function, "current function is null!");
    auto pre_block = current_function->current_block;
    current_function->EndBlock(if_);
    auto else_ = GetNextLocation(if_.else_);
    auto then_ = GetNextLocation(if_.then_);
    auto else_block = current_function->AppendBlock(else_);
    if (else_block) {
        current_function->AddEdge(pre_block, else_block);
    }
    auto then_block = current_function->AppendBlock(then_);
    if (then_block) {
        current_function->AddEdge(pre_block, then_block);
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
        current_function->AddEdge(pre_block, next_block);
        result[i] = {switch_.cases[i].case_value, next_block};
    }
    return result;
}

HIRBlock* HIRBuilder::LinkBlock(const terminal::LinkBlock& link) {
    ASSERT_MSG(current_function, "current function is null!");
    auto pre_block = current_function->current_block;
    current_function->EndBlock(link);
    auto next_block = current_function->AppendBlock(link.next);
    current_function->AddEdge(pre_block, next_block);
    return next_block;
}

void HIRBuilder::ExternalLinkBlock(const terminal::ExternalLinkBlock& link) {
    ASSERT_MSG(current_function, "current function is null!");
    current_function->EndBlock(link);
}

void HIRBuilder::RegisterExternalDirectLink(Location target) {
    ASSERT_MSG(current_function, "current function is null!");
    current_function->RegisterExternalDirectLink(target);
}

void HIRBuilder::RegisterCallReturn(Location location) {
    ASSERT(current_function);
    current_function->RegisterCallReturn(location);
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

}  // namespace swift::runtime::ir
