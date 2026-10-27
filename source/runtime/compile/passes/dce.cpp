#include "passes.h"
#include "runtime/compile/workspace.h"
#include "value_worklist.h"

#include <algorithm>

#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {
namespace {
void AddRoots(const ir::Block& block, StackVector<const ir::Inst*, 4>& roots) {
    for (auto* anchor : block.GetLoopHoistMetadata().anchors) roots.push_back(anchor);
    if (auto* prefix = block.GetLoopHoistMetadata().prefix_end) roots.push_back(prefix);
}
}  // namespace

RewriteContext::RewriteContext(ir::Block* block,
                               CompileWorkspace& workspace,
                               ir::HIRFunction* owner,
                               bool cleanup)
        : function(owner), workspace(workspace), single_block(true), eliminate_dead(cleanup) {
    block->CheckMutable();
    blocks.push_back(block);
    if (eliminate_dead) AddRoots(*block, implicit_roots);
}

RewriteContext::RewriteContext(ir::HIRFunction* input, CompileWorkspace& workspace, bool cleanup)
        : function(input), workspace(workspace), eliminate_dead(cleanup) {
    for (auto* current : function->GetHIRBlocks()) {
        if (!current) continue;
        auto* block = function->EditBlock(current).GetBlock();
        block->CheckMutable();
        blocks.push_back(block);
        if (eliminate_dead) AddRoots(*block, implicit_roots);
    }
}

void RewriteContext::CheckBlock(const ir::Block* block) const {
    ASSERT(block);
    if (single_block) {
        ASSERT(block == blocks.front());
    } else {
        const auto roster = std::as_const(*function).GetHIRBlocks();
        const auto order = block->instruction_owner.hir_order;
        ASSERT(order < roster.size() && roster[order] && roster[order]->GetBlock() == block);
    }
}

bool RewriteContext::FlagRelevant(const ir::Inst& inst) {
    return True(inst.GetEffects() & (ir::IREffect::ReadFlags | ir::IREffect::WriteFlags)) ||
           inst.IsPseudoOperation();
}

bool RewriteContext::Removable(const ir::Inst& inst) const {
    return inst.GetUses(false) == 0 && !inst.HasSideEffects() &&
           std::find(implicit_roots.begin(), implicit_roots.end(), &inst) == implicit_roots.end();
}

void RewriteContext::NoteFlags(ir::Block* block) {
    if (std::find(flag_changes.begin(), flag_changes.end(), block) == flag_changes.end()) {
        flag_changes.push_back(block);
    }
}

void RewriteContext::BeforeChange(const ir::Inst* inst) {
    if (values) values->BeforeChange(inst);
    if (function) function->UnUseInst(inst);
}

void RewriteContext::AfterChange(ir::Block* block,
                                 const ir::Inst* inst,
                                 const ir::Inst::Values& inputs,
                                 bool flags) {
    if (function) function->UseInst(inst);
    if (values) values->AfterChange(inst, inputs);
    if (flags || FlagRelevant(*inst)) NoteFlags(block);
    block_dead_pending = true;
    for (const auto value : inputs) QueueDead(value.Def());
    QueueDead(inst);
}

ir::Block* RewriteContext::LiveOwner(const ir::Inst* inst) const {
    if (values) return values->LiveOwner(inst);
    const auto found = owners.find(inst);
    return found == owners.end() ? nullptr : found->second;
}

void RewriteContext::ResetDeadOwners() {
    if (single_block || !dead_seeded) return;
    // Switching the membership source invalidates both queued pointers and
    // the old map. Reseeding later catches all currently removable nodes.
    dead->Entries().clear();
    owners.clear();
    dead_seeded = false;
}

void RewriteContext::QueueDead(const ir::Inst* inst) {
    if (single_block || !eliminate_dead || !dead_seeded || !LiveOwner(inst)) return;
    if (Removable(*inst)) dead->Entries().push_back(inst);
}

void RewriteContext::Erase(ir::Block* block, const ir::Inst* inst) {
    CheckBlock(block);
    block->EditInst(inst);
    const auto inputs = inst->GetValues();
    if (FlagRelevant(*inst)) NoteFlags(block);
    if (values) values->BeforeErase(inst);
    if (!single_block && !values) owners.erase(inst);
    if (function) function->EraseInst(block, inst);
    else block->DestroyInst(inst);
    if (values) values->AfterErase(inputs);
    block_dead_pending = true;
    for (const auto value : inputs) QueueDead(value.Def());
}

bool RewriteContext::EraseDead(ir::Block* block, const ir::Inst* inst) {
    if (!eliminate_dead || !Removable(*inst)) return false;
    Erase(block, inst);
    return true;
}

void RewriteContext::CollectDead() {
    if (!eliminate_dead) return;
    RefreshRoots();
    if (single_block) {
        if (!block_dead_pending) return;
        auto* block = blocks.front();
        auto& list = block->GetInstList();
        // Keep a live successor as the reverse cursor. std::reverse_iterator
        // would retain the erased node as its base after ++.
        for (auto cursor = list.end(); cursor != list.begin();) {
            const auto current = std::prev(cursor);
            if (!EraseDead(block, &*current)) cursor = current;
        }
        block_dead_pending = false;
        return;
    }
    if (!dead_seeded) {
        if (!dead) dead.emplace(workspace);
        dead_seeded = true;
        for (auto* block : blocks) {
            for (auto& inst : block->GetInstList()) {
                if (!values) owners.emplace(&inst, block);
                QueueDead(&inst);
            }
        }
    }
    // Both membership sources reject erased pointers before dereferencing
    // duplicate entries. An active value epoch includes unreachable nodes.
    auto& queue = dead->Entries();
    for (size_t next = 0; next < queue.size(); ++next) {
        if (auto* block = LiveOwner(queue[next])) EraseDead(block, queue[next]);
    }
    queue.clear();
}

void RewriteContext::RefreshRoots() {
    const auto previous = implicit_roots;
    implicit_roots.clear();
    for (const auto* block : blocks) AddRoots(*block, implicit_roots);
    if (previous.size() == implicit_roots.size() &&
        std::equal(previous.begin(), previous.end(), implicit_roots.begin())) return;
    block_dead_pending = true;
    for (auto* root : previous) QueueDead(root);
}

}  // namespace swift::runtime::compile
