//
// Created by 甘尧 on 2022/1/5.
//

#pragma once

#include <algorithm>
#include <utility>
#include "types.h"

namespace swift::runtime {

class MemArena {
public:
    explicit MemArena(size_t chunk_size = 4_KB) : new_chunk_size{chunk_size} {
        node = &chunks.emplace_back(new_chunk_size);
    }

    template <typename T, typename... Args>
        requires std::is_constructible_v<T, Args...>
    T* Create(Args&&... args) {
        return std::construct_at(Memory<T>(), std::forward<Args>(args)...);
    }

    template <typename T> T* CreateArray(size_t size) {
        return reinterpret_cast<T*>(Memory(sizeof(T) * size));
    }

    // Hands every byte back for reuse without returning it to the allocator.
    // Nothing here runs destructors -- the arena never ran them in the first
    // place (Create() constructs in place and the chunk is simply dropped), so
    // this is only valid once the caller is done with every object it carved
    // out. Reusing one arena across compilation units is exactly that case.
    //
    // The largest chunk is kept rather than the first: FreeChunk oversizes a
    // chunk when a single request does not fit (block pointer tables for wide
    // CFGs), and keeping that one lets the next unit of similar shape run
    // without touching the allocator at all.
    void Reset() {
        if (chunks.size() > 1) {
            auto largest = std::max_element(
                    chunks.begin(), chunks.end(),
                    [](const Chunk& a, const Chunk& b) { return a.num_size < b.num_size; });
            if (largest != chunks.begin()) {
                std::swap(*chunks.begin(), *largest);
            }
            chunks.resize(1);
        }
        chunks.front().used_size = 0;
        node = &chunks.front();
    }

private:
    struct NonTrivialDummy {
        NonTrivialDummy() noexcept = default;
    };

    struct Chunk {
        explicit Chunk() = default;
        explicit Chunk(size_t size) : num_size{size}, storage(size) {}

        Chunk& operator=(Chunk&& rhs) noexcept {
            used_size = std::exchange(rhs.used_size, 0);
            num_size = std::exchange(rhs.num_size, 0);
            storage = std::move(rhs.storage);
            return *this;
        }

        Chunk(Chunk&& rhs) noexcept
                : used_size{std::exchange(rhs.used_size, 0)}
                , num_size{std::exchange(rhs.num_size, 0)}
                , storage{std::move(rhs.storage)} {}

        size_t used_size{};
        size_t num_size{};
        Vector<u8> storage;
    };

    [[nodiscard]] void* Memory(size_t size) {
        Chunk* const chunk{FreeChunk(size)};
        auto result = &chunk->storage[chunk->used_size];
        chunk->used_size += size;
        return reinterpret_cast<void*>(result);
    }

    template <typename T> [[nodiscard]] T* Memory() {
        return reinterpret_cast<T*>(Memory(sizeof(T)));
    }

    [[nodiscard]] Chunk* FreeChunk(size_t size) {
        if (node->used_size + size <= node->num_size) {
            return node;
        }
        // Some arena users require one contiguous allocation larger than the
        // normal chunk size. In particular, HIRFunction::EndFunction builds a
        // pointer table for every CFG block; a function with more than 63 real
        // blocks plus its synthetic entry exceeds HIRPools' 512-byte default
        // arena chunk. Returning another default-sized chunk here let
        // Memory() advance past the backing vector and corrupted the function
        // during JIT emission. Oversize the new chunk to fit the request.
        node = &chunks.emplace_back(std::max(new_chunk_size, size));
        return node;
    }

    Chunk* node{};
    Vector<Chunk> chunks;
    size_t new_chunk_size{};
};

}  // namespace swift::runtime
