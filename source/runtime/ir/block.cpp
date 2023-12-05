//
// Created by 甘尧 on 2023/9/6.
//

#include "block.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::ir {

Terminal Block::GetTerminal() const { return block_term; }

void Block::SetTerminal(Terminal term) { block_term = std::move(term); }

bool Block::HasTerminal() const { return !block_term.empty(); }

void Block::AppendInst(Inst* inst) {
    Inst::Validate(inst);
    inst_list.push_back(*inst);
}

void Block::InsertBefore(Inst* inst, Inst* before) {
    if (!inst_list.empty() && inst_list.iterator_to(*before) != inst_list.begin()) {
        inst_list.insert(std::prev(inst_list.iterator_to(*before)), *inst);
    } else {
        inst_list.push_front(*inst);
    }
}

void Block::InsertAfter(Inst* inst, Inst* after) {
    inst_list.insert(inst_list.iterator_to(*after), *inst);
}

void Block::RemoveInst(Inst* inst) { inst_list.erase(*inst); }

void Block::DestroyInst(Inst* inst) {
    inst_list.erase(*inst);
    delete inst;
}

void DestroyInst(InstList::iterator& itr);

void Block::SetEndLocation(Location end_) { this->end = end_; }

Location Block::GetStartLocation() { return location; }

InstList& Block::GetInstList() { return inst_list; }

InstList::iterator Block::GetBeginInst() { return inst_list.begin(); }

Block::~Block() {
    for (auto& inst : inst_list) {
        DestroyInst(&inst);
    }
}

}  // namespace swift::runtime::ir