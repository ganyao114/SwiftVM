#pragma once

#include "code_image.h"

#include <string>
#include <unordered_map>

namespace swift::runtime::execution {
class DiskCache {
public:
    DiskCache(std::string directory, CodeImageKey);
    // Failure of optional cache I/O is a cache rejection, never a native
    // publication failure. The VM's normal compilation path remains available.
    bool Load() noexcept;
    const CodeUnit* Find(std::uint64_t pc, std::uint8_t fp_mode) const noexcept;
    void Record(CodeUnit);
    bool Flush() noexcept;

    void Disable() noexcept { disabled = true; }

    bool Enabled() const noexcept { return !disabled; }

    bool Dirty() const noexcept { return dirty; }

    const CodeImage& Image() const noexcept { return image; }

private:
    struct Key {
        std::uint64_t pc;
        std::uint8_t fp_mode;
        friend bool operator==(Key, Key) = default;
    };

    struct Hash {
        std::size_t operator()(Key key) const noexcept;
    };

    std::string path;
    CodeImage image;
    std::unordered_map<Key, std::size_t, Hash> index;
    bool dirty{}, disabled{};
};
}  // namespace swift::runtime::execution
