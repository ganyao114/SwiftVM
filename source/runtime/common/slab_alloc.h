#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include "logging.h"

namespace swift::runtime {

class SlabAllocator {
public:
    struct Node {
        Node* next{};
    };

    constexpr SlabAllocator() = default;

    void Initialize(size_t size) { size_ = size; }

    [[nodiscard]] std::size_t GetSize() const { return size_; }

    [[nodiscard]] Node* GetHead() const { return head; }

    void* Allocate();

    void Free(void* obj);

private:
    std::atomic<Node*> head{};
    size_t size_{};
};

template <typename T> class SlabHeap {
public:
    constexpr SlabHeap() = default;

    constexpr SlabHeap(size_t count) {
        slab_memory = malloc(sizeof(T) * count);
        if (slab_memory) {
            Initialize(slab_memory, sizeof(T) * count);
        }
    }

    ~SlabHeap() {
        if (slab_memory) {
            free(slab_memory);
        }
    }

    [[nodiscard]] constexpr bool Contains(uintptr_t addr) const { return start <= addr && addr < end; }

    [[nodiscard]] constexpr std::size_t GetSlabHeapSize() const { return (end - start) / GetObjectSize(); }

    [[nodiscard]] constexpr std::size_t GetObjectSize() const { return allocator.GetSize(); }

    std::size_t GetObjectIndexImpl(const void* obj) const {
        return (reinterpret_cast<uintptr_t>(obj) - start) / GetObjectSize();
    }

    template <typename... Args> T* New(Args... args) {
        T* obj = Allocate();
        if (obj != nullptr) {
            new (obj) T(args...);
        }
        return obj;
    }

    void Delete(T* obj) {
        obj->~T();
        Free(obj);
    }

    T* Allocate() {
        T* obj = static_cast<T*>(allocator.Allocate());
        return obj;
    }

    void Free(void* obj) {
        ASSERT(Contains(reinterpret_cast<uintptr_t>(obj)));
        allocator.Free(obj);
    }

    // First initialization wins; later calls are ignored.
    //
    // The slab is a process-global, one-shot resource and SlabObject::TryFree
    // tells a slab pointer from a malloc'd one *only* by testing [start, end).
    // Re-initializing therefore cannot be honoured: it would (a) orphan every
    // object still living in the previous region, so freeing one hands libc an
    // interior pointer of a live malloc block ("pointer being freed was not
    // allocated"), and (b) chain the new region's free nodes on top of the old
    // list -- SlabAllocator::Initialize only sets the object size, it does not
    // reset `head` -- so once the new region drains, Allocate() starts handing
    // out addresses from the orphaned region and every free of one aborts.
    void Initialize(void* memory, size_t memory_size) {
        ASSERT(memory != nullptr);

        if (initialized) {
            return;
        }
        initialized = true;

        auto object_size = sizeof(T);

        allocator.Initialize(object_size);

        const std::size_t num_obj = (memory_size / object_size);
        start = reinterpret_cast<uintptr_t>(memory);
        end = start + num_obj * object_size;

        auto* cur = reinterpret_cast<uint8_t*>(end);

        for (std::size_t i{}; i < num_obj; i++) {
            cur -= object_size;
            allocator.Free(cur);
        }
    }

    void Initialize(size_t object_size) {
        if (initialized) {
            return;
        }
        slab_memory = malloc(sizeof(T) * object_size);
        if (slab_memory) {
            Initialize(slab_memory, sizeof(T) * object_size);
        }
    }

private:
    SlabAllocator allocator;
    uintptr_t start{};
    uintptr_t end{};
    void* slab_memory{};
    bool initialized{};
};

template <class T, bool override = false> class SlabObject {
private:
    static inline SlabHeap<T> slab_heap;

public:
    constexpr SlabObject() = default;

    static void InitializeSlabHeap(size_t object_size) { slab_heap.Initialize(object_size); }

    static void InitializeSlabHeap(void* memory, size_t memory_size) {
        slab_heap.Initialize(memory, memory_size);
    }

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
        if (override && slab_heap.Contains(reinterpret_cast<uintptr_t>(obj))) {
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

}  // namespace swift::runtime