#pragma once

#include <atomic>
#include <memory>
#include "runtime/common/mem_arena.h"
#include "runtime/common/object_pool.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {
struct HIRPoolCache;

// Internal storage layout. Consumers retain HIRPoolLease, not these allocators.
struct HIRPools {
    explicit HIRPools(u32 function_capacity = 1);
    ~HIRPools();
    void Reset();

    HIRBlockVector CreateBlockVector(size_t size) {
        return {mem_arena.CreateArray<HIRBlock*>(size), size};
    }
    std::span<const HIRBlock*> CreateBlockReferences(size_t size) {
        return {mem_arena.CreateArray<const HIRBlock*>(size), size};
    }

    MemArena mem_arena;
    ObjectPool<HIRFunction, true> functions;
    ObjectPool<HIRBlock, true> blocks;
    ObjectPool<HIRValue> values;
    ObjectPool<Edge> edges;
    ObjectPool<HIRUse> uses;

private:
    friend class HIRPoolLease;
    std::atomic<u32> references{};
    std::weak_ptr<HIRPoolCache> cache;
};
}  // namespace swift::runtime::ir
