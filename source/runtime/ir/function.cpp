//
// Created by mac on 2023/9/7.
//

#include "function.h"

namespace swift::runtime::ir {

ir::Block* Function::EntryBlock() { return (ir::Block *) blocks.begin().operator->(); }

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

Function::~Function() {
    for (auto& block : blocks) {
        blocks.erase(block);
        delete &block;
    }
}

}  // namespace swift::runtime::ir
