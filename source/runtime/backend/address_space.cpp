//
// Created by 甘尧 on 2023/9/8.
//

#include "address_space.h"

namespace swift::runtime::backend {

void AddressSpace::Push(ir::Block* block) {
    ASSERT(block);
    ir_blocks.insert(*block);
}

void AddressSpace::Push(ir::Function* func) {
    ASSERT(func);
    ir_functions.insert(*func);
}

}
