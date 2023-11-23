#pragma once

#include <array>
#include <atomic>
#include <shared_mutex>
#include <vector>
#include "runtime/common/types.h"

namespace swift::runtime {

struct TranslateEntry {
    size_t key;
    size_t value;
};

static constexpr size_t HASH_TABLE_PAGE_BITS = 23UL;

using TableLock = std::shared_mutex;

class TranslateTable {
public:
    explicit TranslateTable(size_t hash_bits_ = HASH_TABLE_PAGE_BITS) : hash_bits{hash_bits_} {
        size = 1 << hash_bits;
        Reset();
    }

    TranslateEntry* Data() { return entries.data(); }

    [[nodiscard]] u32 Hash(size_t key) const {
        u64 merged = key >> 2;
        return (merged >> hash_bits ^ merged) & (size - 1);
    }

    bool Put(size_t key, size_t value) {
        u32 index = Hash(key);
        bool done = false;

        std::unique_lock<TableLock> guard(lock);
        do {
            if (entries[index].key == 0 || entries[index].key == key) {
                entries[index].value = value;
                std::atomic_thread_fence(std::memory_order_acquire);
                entries[index].key = key;
                done = true;
            } else {
                index++;
                if (index >= size - 1) {
                    abort();
                }
            }
        } while (!done && index < (size - 1));

        assert(done);
        return done;
    }

    u32 GetOrPut(size_t key, size_t value) {
        u32 index = Hash(key);
        u32 result{};
        std::unique_lock<TableLock> guard(lock);
        do {
            if (entries[index].key == 0 || entries[index].key == key) {
                if (entries[index].key == key && entries[index].value) {
                    result = 2 * index + 1;
                } else {
                    entries[index].value = value;
                    std::atomic_thread_fence(std::memory_order_acquire);
                    entries[index].key = key;
                    result = 2 * index + 1;
                }
            } else {
                index++;
                if (index >= size - 1) {
                    abort();
                }
            }
        } while (!result && index < (size - 1));

        return result;
    }

    size_t Lookup(size_t key) {
        u32 index = Hash(key);
        bool found = false;
        size_t entry = 0;
        size_t c_key;

        std::shared_lock<TableLock> guard(lock);
        do {
            c_key = entries[index].key;
            if (c_key == key) {
                entry = entries[index].value;
                found = true;
            } else {
                index++;
            }
        } while (!found && index < (size - 1) && c_key != 0);
        return entry;
    }

    void Replace(size_t key, size_t value) {
        u32 index = Hash(key);
        bool found = false;
        size_t* entry = nullptr;
        size_t c_key;

        std::unique_lock<TableLock> guard(lock);
        do {
            c_key = entries[index].key;
            if (c_key == key) {
                entry = &entries[index].value;
                found = true;
            } else {
                index++;
            }
        } while (!found && index < (size - 1) && c_key != 0);
        if (entry) {
            *entry = value;
        } else {
            Put(key, value);
        }
    }

    void Remove(u64 key) {
        u32 index = Hash(key);
        u32 end = index - 1;
        bool found = false;
        size_t c_key;

        std::unique_lock<TableLock> guard(lock);
        do {
            c_key = entries[index].key;
            if (c_key == key) {
                entries[index].key = 0;
                found = true;
            } else {
                index = (index + 1) & size;
            }
        } while (!found && index != end && c_key != 0);
    }

    void Clear() { std::memset(entries.data(), 0, entries.size() * sizeof(TranslateEntry)); }

    void Reset() {
        entries.resize(size + 10);
        entries.back().key = size_t(-1);
    }

private:
    TableLock lock{};
    size_t hash_bits;
    size_t size;
    std::vector<TranslateEntry> entries;
};

}  // namespace swift::runtime