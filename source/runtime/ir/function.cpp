//
// Created by mac on 2023/9/7.
//

#include "function.h"

namespace swift::runtime::ir {

Location Function::GetStartLocation() { return location; }

ir::Block* Function::EntryBlock() { return blocks.begin().operator->(); }

ir::Block* Function::FindBlock(ir::Location loc) {
    if (auto itr = blocks.find(ir::Block{loc}); itr != blocks.end()) {
        return itr.operator->();
    } else {
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
