//
// Created by 甘尧 on 2024/3/8.
//

#pragma once

#include <map>
#include <shared_mutex>
#include "base/common_funcs.h"
#include "runtime/backend/code_cache.h"
#include "runtime/common/range_mutex.h"
#include "runtime/common/types.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {

class Module : DeleteCopyAndMove {
public:
    explicit Module(const ir::Location& start, const ir::Location& end, bool ro);

    void Push(ir::Block* block);

    void Push(ir::Function* func);

    [[nodiscard]] ir::Function* GetFunction(ir::Location location);

    [[nodiscard]] ir::Block* GetBlock(ir::Location location);

    [[nodiscard]] std::pair<ir::Block*, ir::Block::ReadLock> LockReadBlock(ir::Location location);

    [[nodiscard]] std::pair<ir::Function*, ir::Function::ReadLock> LockReadFunction(
            ir::Location location);

    void RemoveBlock(ir::Block* block);

    void RemoveFunction(ir::Function* function);

    [[nodiscard]] ScopedRangeLock LockAddress(ir::Location start, ir::Location end) {
        return ScopedRangeLock{address_lock, start.Value(), end.Value()};
    }

    [[nodiscard]] bool ReadOnly() const { return read_only; }

private:
    ir::Location module_start;
    ir::Location module_end;
    const bool read_only;
    std::shared_mutex lock;
    RangeMutex address_lock{};
    ir::BlockMap ir_blocks{};
    ir::FunctionMap ir_functions{};
    std::map<u16, CodeCache> code_caches{};
};

}  // namespace swift::runtime::backend
