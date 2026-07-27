//
// Created by 甘尧 on 2023/9/6.
//

#include "block.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::ir {

Terminal Block::GetTerminal() const { return block_term; }

void Block::SetTerminal(Terminal term) {
    block_term = std::move(term);
    if (!inst_list.empty() && inst_list.begin()->Id() == Inst::invalid_id) {
        ReIdInstr();
    }
}

bool Block::HasTerminal() const { return block_term.which() != 0; }

void Block::AppendInst(Inst* inst) {
    Inst::Validate(inst);
    inst_list.push_back(*inst);
}

void Block::InsertBefore(Inst* inst, Inst* before) {
    if (!inst_list.empty()) {
        inst_list.insert(inst_list.iterator_to(*before), *inst);
    } else {
        inst_list.push_front(*inst);
    }
}

void Block::InsertAfter(Inst* inst, Inst* after) {
    inst_list.insert(std::next(inst_list.iterator_to(*after)), *inst);
}

void Block::RemoveInst(Inst* inst) { inst_list.erase(*inst); }

void Block::DestroyInst(Inst* inst) {
    inst_list.erase(*inst);
    delete inst;
}

void Block::DestroyInstrs() {
    // One pass. Every node is unlinked *before* it is freed and the successor
    // comes from erase()'s return value, which it reads while the node is
    // still live -- Inst::operator delete threads its free list through the
    // object's own first bytes, and list_node is what lives there. (Deleting
    // first and calling inst_list.clear() afterwards is a use-after-free for
    // the same reason: clear() is a pop_front loop over the freed nodes.)
    for (auto it = inst_list.begin(); it != inst_list.end();) {
        auto* inst = it.operator->();
        it = inst_list.erase(it);
        inst->ReleaseArgs();
        delete inst;
    }
}

void Block::ReIdInstr() {
    u16 index{0};
    for (auto& instr : inst_list) {
        instr.SetId(index++);
    }
    max_instr_id = index;
}

InstList& Block::GetInstList() { return inst_list; }

InstList& Block::GetInstList() const { return inst_list; }

InstList::iterator Block::GetBeginInst() { return inst_list.begin(); }

bool Block::IsJitCached() {
    return jit_cache.jit_state == backend::JitState::Cached;
}

bool Block::IsEmptyBlock() {
    return !IsJitCached() && inst_list.empty() && !HasTerminal();
}

std::string Block::ToString() const { return fmt::format("{}", *this); }

Block::~Block() {
    auto guard = LockWrite();
    DestroyInstrs();
}

}  // namespace swift::runtime::ir
