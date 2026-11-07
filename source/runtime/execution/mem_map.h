#pragma once

#include <cstddef>
#include <cstdint>

namespace swift::runtime::execution {

struct MappingRelease {
    std::size_t remaining_views{};
    int unmap_error{}, close_error{};

    bool Complete() const noexcept { return !remaining_views && !unmap_error && !close_error; }
};

// One native backing and its actual views. Guest mappings belong to GuestMemory.
// A failed Release keeps its remaining views in this same owner for a retry.
class MemMap {
public:
    MemMap() = default;
    ~MemMap();
    MemMap(const MemMap&) = delete;
    MemMap& operator=(const MemMap&) = delete;
    MemMap(MemMap&&) noexcept;
    MemMap& operator=(MemMap&&) = delete;

    // Called only after this owner has been placed in the heap's slab list.
    // A failed creation therefore leaves any unreleased kernel view owned.
    void MapNative(std::size_t bytes);

    std::uint8_t* Writable() const noexcept { return write; }

    std::uint8_t* Executable() const noexcept { return execute; }

    std::size_t Size() const noexcept { return size; }

    std::size_t ReservedBytes() const noexcept { return write ? size * (dual ? 2 : 1) : 0; }

    bool Dual() const noexcept { return dual; }

    bool SetWritable() noexcept;
    bool SetExecutable() noexcept;
    MappingRelease Release() noexcept;

private:
    std::uint8_t* write{};
    std::uint8_t* execute{};
    std::size_t size{};
    bool dual{};
    int close_error{};
};

}  // namespace swift::runtime::execution
