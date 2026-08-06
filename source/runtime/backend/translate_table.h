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

enum class TranslateTableHash {
    Folded,
    Direct,
};

class TranslateTable {
public:
    explicit TranslateTable(size_t hash_bits_ = HASH_TABLE_PAGE_BITS,
                            TranslateTableHash hash_mode_ = TranslateTableHash::Folded)
            : hash_bits{hash_bits_}, hash_mode{hash_mode_} {
        size = 1 << hash_bits;
        Reset();
    }

    TranslateEntry* Data() { return entries.data(); }

    [[nodiscard]] u32 Hash(size_t key) const {
        if (hash_mode == TranslateTableHash::Direct) {
            return key & (size - 1);
        }
        u64 merged = key >> 2;
        return (merged >> hash_bits ^ merged) & (size - 1);
    }

    void SetInvalidValue(size_t value) { invalid_value = value; }

    bool Put(size_t key, size_t value) {
        u32 index = Hash(key);
        bool done = false;
        u32 result{};
        std::unique_lock<TableLock> guard(lock);
        do {
            if (entries[index].key == 0 || entries[index].key == key) {
                entries[index].value = value;
                // Generated dispatchers read this table without taking the
                // host lock. Publish the code pointer before making its key
                // visible so a racing reader can only see a miss, never a key
                // paired with an uninitialized target.
                std::atomic_thread_fence(std::memory_order_release);
                entries[index].key = key;
                result = 2 * index + 1;
                done = true;
            } else {
                index++;
                if (index >= size - 1) {
                    abort();
                }
            }
        } while (!done && index < (size - 1));

        assert(done);
        return result;
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
                    std::atomic_thread_fence(std::memory_order_release);
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

    // Invalidates the *value* of the entry for `key`, keeping the key itself.
    // The normal L2 value is zero; an inline-L1 table may instead use a safe
    // dispatcher continuation. Unlike Remove(), this does not break the
    // linear probe chain for colliding keys, and the aligned value store is
    // atomic for generated lock-free readers on the supported hosts.
    // Returns true if the entry was found.
    bool Zero(size_t key) {
        u32 index = Hash(key);
        size_t c_key;

        std::shared_lock<TableLock> guard(lock);
        do {
            c_key = entries[index].key;
            if (c_key == key) {
                entries[index].value = invalid_value;
                std::atomic_thread_fence(std::memory_order_release);
                return true;
            }
            index++;
        } while (index < (size - 1) && c_key != 0);
        return false;
    }

    // --- JIT disk cache / AOT support -------------------------------------
    // A slot index is not a function of the key alone: colliding keys probe
    // forward, so which slot a key ends up in depends on insertion order. The
    // JIT dispatch indices baked into generated code are slot indices, so a
    // deserialized code unit is only valid if the table reproduces the exact
    // assignment its immediates were emitted against. PutAt installs one
    // recorded (index, key) pair with a zero value (the code pointer is filled
    // later by Put/PushCodeCache); ForEachEntry snapshots the assignment.
    //
    // PutAt returns false when `index` is already claimed by a different key,
    // which the caller must treat as "this whole cache file is unusable" --
    // a mis-assigned slot is a wild branch, not a miss.
    bool PutAt(u32 index, size_t key) {
        if (key == 0 || index >= size - 1) {
            return false;
        }
        std::unique_lock<TableLock> guard(lock);
        if (entries[index].key == key) {
            return true;
        }
        if (entries[index].key != 0) {
            return false;
        }
        entries[index].value = 0;
        std::atomic_thread_fence(std::memory_order_release);
        entries[index].key = key;
        return true;
    }

    template <typename Fn> void ForEachEntry(Fn&& fn) {
        std::shared_lock<TableLock> guard(lock);
        for (u32 index = 0; index < size - 1; ++index) {
            if (entries[index].key != 0) {
                fn(index, entries[index].key, entries[index].value);
            }
        }
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
    TranslateTableHash hash_mode;
    size_t invalid_value{};
    std::vector<TranslateEntry> entries;
};

}  // namespace swift::runtime
