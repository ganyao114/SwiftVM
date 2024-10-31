//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/ir/block.h"

namespace swift::runtime::ir {

class Function : public SlabObject<Function, true>,
                 public IntrusiveRefCounter<Function>,
                 public AddressNode {
public:
    using ReadLock = std::shared_lock<RwSpinLock>;
    using WriteLock = std::unique_lock<RwSpinLock>;

    explicit Function() = default;

    explicit Function(const Location& location) : AddressNode(location, AddressNode::Function) {}

    ~Function();

    ir::Block* EntryBlock();
    ir::Block* FindBlock(ir::Location location, bool block_start = true);

    [[nodiscard]] ReadLock LockRead() { return std::shared_lock{func_lock}; }

    [[nodiscard]] WriteLock LockWrite() { return std::unique_lock{func_lock}; }

    [[nodiscard]] backend::JitCache& GetJitCache() { return jit_cache; }

    [[nodiscard]] u32 GetDispatchIndex() const { return dispatch_index; }

private:
    union {
        u32 id{};
        u32 dispatch_index;
    };
    BlockMap blocks{};
    RwSpinLock func_lock{};
    u16 v_stack{};
    backend::JitCache jit_cache;
};

using FunctionList = IntrusiveList<&Function::list_node>;
using FunctionMap = IntrusiveMap<&Function::map_node>;

}  // namespace swift::runtime::ir