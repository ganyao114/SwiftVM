//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "runtime/ir/block.h"

namespace swift::runtime::ir {

class Function : public SlabObject<Function, true>, public IntrusiveRefCounter<Function> {
public:
    using ReadLock = std::shared_lock<RwSpinLock>;
    using WriteLock = std::unique_lock<RwSpinLock>;

    explicit Function() = default;

    explicit Function(const Location &location) : location(location) {}

    ~Function();

    Location GetStartLocation();
    ir::Block *EntryBlock();
    ir::Block *FindBlock(ir::Location location);

    [[nodiscard]] ReadLock LockRead() {
        return std::shared_lock{func_lock};
    }

    [[nodiscard]] WriteLock LockWrite() {
        return std::unique_lock{func_lock};
    }

    [[nodiscard]] backend::JitCache& GetJitCache() {
        return jit_cache;
    }

    [[nodiscard]] u32 GetDispatchIndex() const {
        return dispatch_index;
    }

    union {
        NonTriviallyDummy dummy{};
        IntrusiveMapNode map_node;
        IntrusiveListNode list_node;
    };

    // for rbtree compare
    static NOINLINE int Compare(const Function &lhs, const Function &rhs) {
        if (rhs.location > lhs.location) {
            return 1;
        } else if (rhs.location < lhs.location) {
            return -1;
        } else {
            return 0;
        }
    }
private:
    union {
        u32 id{};
        u32 dispatch_index;
    };
    Location location;
    BlockMap blocks{};
    RwSpinLock func_lock{};
    u16 v_stack{};
    backend::JitCache jit_cache;
};

using FunctionList = IntrusiveList<&Function::list_node>;
using FunctionMap = IntrusiveMap<&Function::map_node>;

}  // namespace swift::runtime::ir