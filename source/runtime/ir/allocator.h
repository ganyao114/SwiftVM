//
// Created by 甘尧 on 2025/1/4.
//

#pragma once

#include <concepts>
#include "base/common_funcs.h"
#include "function.h"

namespace swift::runtime::ir {

enum class SvmObjType {
    Instruction,
    Params,
    Block,
    Function
};

class SvmObjectsAllocator;

template <typename T>
concept SvmObjClass = requires(T) {
    { T::TYPE } -> std::convertible_to<SvmObjType>;
};

template <SvmObjClass T> class SvmObject {
public:
    constexpr SvmObject() = default;

    constexpr static T* Allocate() { return slab_heap.Allocate(); }

    constexpr static void Free(T* obj) { slab_heap.Free(obj); }

    template <typename... Args> constexpr static T* New(Args... args) {
        return slab_heap.New(args...);
    }

    constexpr static void Delete(T* obj) { slab_heap.Delete(obj); }

    constexpr static T* TryAllocate(size_t size = sizeof(T)) {
        if constexpr (override) {
            auto res = slab_heap.Allocate();
            if (res) {
                return res;
            }
        }
        return reinterpret_cast<T*>(malloc(size));
    }

    constexpr static void TryFree(void* obj) {
        if (slab_heap.Contains(reinterpret_cast<uintptr_t>(obj))) {
            slab_heap.Free(obj);
        } else {
            free(obj);
        }
    }

    void* operator new(size_t sz) {
        ASSERT(sz == sizeof(T));
        return TryAllocate();
    }
    void operator delete(void* p) { TryFree(p); }
};

class SvmObjectsAllocator {
public:
    SvmObjectsAllocator() = default;

    static SvmObjectsAllocator *Current();

    static void SetCurrent(SvmObjectsAllocator *allocator);

    template <SvmObjClass T> T* Allocate() {
        return Allocate(T::TYPE);
    }

    template <SvmObjClass T> void Free(T* obj) {
        Free(T::TYPE, obj);
    }

    void *Allocate(size_t size);

    void Free(void *ptr);

    void *Allocate(SvmObjType type);

    void Free(SvmObjType type, void *ptr);
};

}
