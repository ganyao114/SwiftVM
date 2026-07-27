//
// Created by mac on 2023/9/7.
//

#include "function.h"

namespace swift::runtime::ir {

ir::Block* Function::EntryBlock() { return FindBlock(GetStartLocation()); }

void Function::AddBlock(ir::Block* block) {
    ASSERT(block);
    blocks.insert(*block);
}

ir::Block* Function::FindBlock(ir::Location loc, bool block_start) {
    if (block_start) {
        if (auto itr = blocks.find(ir::Block{loc}); itr != blocks.end()) {
            ASSERT(itr->node_type == AddressNode::Block);
            return (ir::Block*) itr.operator->();
        } else {
            return {};
        }
    } else {
        auto start_itr = blocks.lower_bound(ir::AddressNode{loc});
        auto end_itr = blocks.upper_bound(ir::AddressNode{loc});
        for (auto itr = start_itr; itr != end_itr; itr++) {
            if (itr->Overlap(loc.Value(), loc.Value() + 1)) {
                return (ir::Block*) itr.operator->();
            }
        }
        return {};
    }
}

void Function::DestroyInstrs() {
    // Block-at-a-time is safe *because* Block::DestroyInstrs is the bulk path.
    //
    // The version that ran Inst::DestroyArgs was not: DestroyArgs -> UnUse
    // writes through the defining instruction of each argument value, so a
    // function whose value graph crosses a block boundary would write through
    // a dangling Inst* as soon as the defining block had been freed -- which
    // is exactly what ~Function did, one block at a time. It would not fault:
    // the Inst arena never returns memory, so the write lands inside a live
    // chunk and silently decrements a use count on whichever instruction has
    // since been handed the slot. See Inst::ReleaseArgs.
    for (auto& node : blocks) {
        static_cast<ir::Block&>(node).DestroyInstrs();
    }
}

Function::~Function() {
    while (!blocks.empty()) {
        auto* block = static_cast<ir::Block*>(&*blocks.begin());
        blocks.erase(*block);
        // ~Block -> Block::DestroyInstrs, i.e. the same bulk teardown.
        delete block;
    }
}

}  // namespace swift::runtime::ir
