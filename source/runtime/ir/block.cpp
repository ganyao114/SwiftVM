//
// Created by 甘尧 on 2023/9/6.
//

#include "block.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::ir {

const Terminal& Block::GetTerminal() const { return block_term; }

void Block::SetTerminal(Terminal term) {
    CheckMutable();
    block_term = std::move(term);
    if (!inst_list.empty() && inst_list.begin()->Id() == Inst::invalid_id) {
        ReIdInstr();
    }
}

bool Block::HasTerminal() const { return block_term.which() != 0; }

void Block::AppendInst(const Inst* inst) {
    Inst::Validate(inst);
    AppendInstUnchecked(inst);
}

void Block::AppendInstUnchecked(const Inst* inst) {
    CheckMutable();
    ASSERT(inst && !inst->owner);
    inst_list.push_back(*const_cast<Inst*>(inst));
    inst->owner = &instruction_owner;
}

void Block::InsertBefore(const Inst* inst, const Inst* before) {
    CheckMutable();
    ASSERT(inst && !inst->owner);
    if (!inst_list.empty()) {
        ASSERT(before && before->GetBlock() == this);
        inst_list.insert(inst_list.iterator_to(*before), *const_cast<Inst*>(inst));
    } else {
        inst_list.push_front(*const_cast<Inst*>(inst));
    }
    inst->owner = &instruction_owner;
}

void Block::InsertAfter(const Inst* inst, const Inst* after) {
    CheckMutable();
    ASSERT(inst && !inst->owner && after && after->GetBlock() == this);
    inst_list.insert(std::next(inst_list.iterator_to(*after)), *const_cast<Inst*>(inst));
    inst->owner = &instruction_owner;
}

void Block::RemoveInst(const Inst* inst) {
    CheckMutable();
    ASSERT(inst && inst->GetBlock() == this);
    inst_list.erase(*inst);
    inst->owner = nullptr;
}

void Block::MoveBefore(const Inst* inst, const Inst* before) {
    auto& moving = EditInst(inst);
    ASSERT(before && before->GetBlock() == this);
    if (inst == before) return;
    inst_list.erase(*inst);
    inst_list.insert(inst_list.iterator_to(*before), moving);
}

void Block::DestroyInst(const Inst* inst) {
    RemoveInst(inst);
    delete inst;
}

void Block::DestroyInstrs() {
    CheckMutable();
    loop_hoist_metadata = {};
    // One pass. Every node is unlinked *before* it is freed and the successor
    // comes from erase()'s return value, which it reads while the node is
    // still live -- Inst::operator delete threads its free list through the
    // object's own first bytes, and list_node is what lives there. (Deleting
    // first and calling inst_list.clear() afterwards is a use-after-free for
    // the same reason: clear() is a pop_front loop over the freed nodes.)
    for (auto it = inst_list.begin(); it != inst_list.end();) {
        auto* inst = it.operator->();
        it = inst_list.erase(it);
        inst->owner = nullptr;
        inst->ReleaseArgs();
        delete inst;
    }
}

void Block::ReIdInstr() {
    CheckMutable();
    u16 index{0};
    for (auto& instr : inst_list) {
        instr.SetId(index++);
    }
    max_instr_id = index;
}

const InstList& Block::GetInstList() const { return inst_list; }

bool Block::IsJitCached() const {
    return jit_cache.jit_state.get<backend::JitState>() == backend::JitState::Cached;
}

bool Block::IsEmptyBlock() const { return !IsJitCached() && inst_list.empty() && !HasTerminal(); }

std::string Block::ToString() const { return fmt::format("{}", *this); }

Block::~Block() {
    ASSERT(!owning_function);
    auto guard = LockWrite();
    DestroyInstrs();
}

}  // namespace swift::runtime::ir
