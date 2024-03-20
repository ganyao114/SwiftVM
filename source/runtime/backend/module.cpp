//
// Created by 甘尧 on 2024/3/8.
//

#include "module.h"

namespace swift::runtime::backend {

Module::Module(const ir::Location& start, const ir::Location& end, bool ro)
        : module_start(start), module_end(end), read_only(ro) {}

void Module::Push(ir::Block* block) {
    ASSERT(block);
    std::unique_lock guard(lock);
    ir_blocks.insert(*block);
}

void Module::Push(ir::Function* func) {
    ASSERT(func);
    std::unique_lock guard(lock);
    ir_functions.insert(*func);
}

ir::Block* Module::GetBlock(ir::Location location) {
    std::shared_lock guard(lock);
    if (auto itr = ir_blocks.find(ir::Block{location}); itr != ir_blocks.end()) {
        return itr.operator->();
    } else {
        return {};
    }
}

std::pair<ir::Block*, ir::Block::ReadLock> Module::LockReadBlock(ir::Location location) {
    std::shared_lock guard(lock);
    if (auto itr = ir_blocks.find(ir::Block{location}); itr != ir_blocks.end()) {
        return {itr.operator->(), itr->LockRead()};
    } else {
        return {};
    }
}

ir::Function* Module::GetFunction(ir::Location location) {
    std::shared_lock guard(lock);
    if (auto itr = ir_functions.find(ir::Function{location}); itr != ir_functions.end()) {
        return itr.operator->();
    } else {
        return {};
    }
}

std::pair<ir::Function*, ir::Function::ReadLock> Module::LockReadFunction(ir::Location location) {
    std::shared_lock guard(lock);
    if (auto itr = ir_functions.find(ir::Function{location}); itr != ir_functions.end()) {
        return {itr.operator->(), itr->LockRead()};
    } else {
        return {};
    }
}

void Module::RemoveBlock(ir::Block* block) {
    std::unique_lock guard(lock);
    ir_blocks.erase(*block);
    delete block;
}

void Module::RemoveFunction(ir::Function* function) {
    std::unique_lock guard(lock);
    ir_functions.erase(*function);
    delete function;
}

}
