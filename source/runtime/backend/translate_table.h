#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <shared_mutex>
#include <utility>
#include "runtime/common/types.h"
#include "runtime/common/virtual_vector.h"

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
        constexpr auto maximum = std::numeric_limits<size_t>::max();
        if (hash_bits >= std::numeric_limits<size_t>::digits) throw std::bad_alloc{};
        size = size_t{1} << hash_bits;
        if (size > maximum / sizeof(TranslateEntry) - 10) throw std::bad_alloc{};
        Reset();
    }

    TranslateEntry* Data() { return entries.get(); }

    [[nodiscard]] size_t DataAlignment() const { return storage_alignment; }

    [[nodiscard]] u32 Hash(size_t key) const {
        if (hash_mode == TranslateTableHash::Direct) {
            return key & (size - 1);
        }
        u64 merged = key >> 2;
        return (merged >> hash_bits ^ merged) & (size - 1);
    }

    void SetInvalidValue(size_t value) {
        std::unique_lock<TableLock> guard(lock);
        invalid_value = value;
        indexed_invalid_base = 0;
        entries[Hash(0)].value = value;
    }

    void SetIndexedInvalidValue(void* base) {
        std::unique_lock<TableLock> guard(lock);
        indexed_invalid_base = reinterpret_cast<size_t>(base);
        invalid_value = 0;
        const auto index = Hash(0);
        entries[index].value =
                indexed_invalid_base + index * sizeof(TranslateEntry);
    }

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
    // linear check chain for colliding keys, and the aligned value store is
    // atomic for generated lock-free readers on the supported hosts.
    // Returns true if the entry was found.
    bool Zero(size_t key) {
        u32 index = Hash(key);
        size_t c_key;

        std::shared_lock<TableLock> guard(lock);
        do {
            c_key = entries[index].key;
            if (c_key == key) {
                entries[index].value = indexed_invalid_base
                        ? indexed_invalid_base + index * sizeof(TranslateEntry)
                        : invalid_value;
                std::atomic_thread_fence(std::memory_order_release);
                return true;
            }
            index++;
        } while (index < (size - 1) && c_key != 0);
        return false;
    }

    // --- JIT disk cache / AOT support -------------------------------------
    // A slot index is not a function of the key alone: colliding keys check
    // forward, so which slot a key ends up in depends on insertion order. The
    // JIT dispatch indices baked into generated code are slot indices, so a
    // deserialized code unit is only valid if the table reproduces the exact
    // assignment its immediates were emitted against. PutAt installs one
    // recorded (index, key) pair with a zero value (the code pointer is filled
    // later by Put/PushCodeCache); ForEachEntry captures the assignment.
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

    void Clear() {
        std::memset(entries.get(), 0, entry_count * sizeof(TranslateEntry));
    }

    void Reset() {
        const auto next_entry_count = size + 10;
        const auto direct_alignment = hash_mode == TranslateTableHash::Direct
                ? size * sizeof(TranslateEntry)
                : 0;
        const auto alignment = direct_alignment ? direct_alignment : alignof(TranslateEntry);
        const auto bytes = next_entry_count * sizeof(TranslateEntry);
        const auto padding = alignment - 1;
        if (bytes > std::numeric_limits<size_t>::max() - padding) throw std::bad_alloc{};
        const auto allocation_size = bytes + padding;
        void* allocation = AllocateMemoryPages(allocation_size);
        const auto address = reinterpret_cast<std::uintptr_t>(allocation);
        auto* storage = reinterpret_cast<TranslateEntry*>((address + padding) & ~padding);
        EntryStorage next{storage, EntryDeleter{allocation, allocation_size}};
        // Anonymous pages already read as zero. Keep untouched table pages
        // uncommitted instead of clearing 128 MiB per shared table and 4 MiB
        // per runtime up front. The original allocation owns alignment padding.
        storage[next_entry_count - 1].key = size_t(-1);
        entries = std::move(next);
        entry_count = next_entry_count;
        storage_alignment = direct_alignment ? direct_alignment : alignof(TranslateEntry);
    }

private:
    struct EntryDeleter {
        void* allocation{};
        size_t bytes{};

        void operator()(TranslateEntry*) const noexcept {
            FreeMemoryPages(allocation, bytes);
        }
    };

    using EntryStorage = std::unique_ptr<TranslateEntry[], EntryDeleter>;

    TableLock lock{};
    size_t hash_bits;
    size_t size;
    TranslateTableHash hash_mode;
    size_t invalid_value{};
    size_t indexed_invalid_base{};
    size_t entry_count{};
    size_t storage_alignment{};
    EntryStorage entries{nullptr, EntryDeleter{}};
};

}  // namespace swift::runtime
