#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "test_mode.h"

namespace swift::tests {
// Test-only native publisher. It owns the actual RW/RX views, never an IR
// interpreter or a replacement VM/cache. Production lifetime/link tests use
// the real CodeHeap explicitly instead.
class NativeCode {
public:
    NativeCode(std::span<const std::uint32_t>, NativeISA);
    ~NativeCode();
    NativeCode(const NativeCode&) = delete;
    NativeCode& operator=(const NativeCode&) = delete;

    void* Entry() const noexcept { return execute; }

    std::uint8_t* Writable() const noexcept { return write; }

    std::size_t Bytes() const noexcept { return size; }

    std::uint64_t Run(std::uint8_t*) const;

private:
    void Release() noexcept;
    std::uint8_t* write{};
    void* execute{};
    std::size_t size{};
    int fd{-1};
};
}  // namespace swift::tests
