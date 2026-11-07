#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swift::runtime::execution {
class VM;

// Only armed-page state lives here. All VA/backing/protection facts are read
// from GuestMemory's one region ledger, including after map replacements.
class Smc {
public:
    explicit Smc(VM& vm) noexcept : vm(vm) {}

    void Arm(std::uint64_t, std::size_t);
    bool Recover(std::uintptr_t) noexcept;
    // Caller has qualified the complete writable range. Retains native code
    // while removing any actual write watches before a fault-intolerant helper.
    bool PrepareWriteRange(std::uint64_t, std::size_t) noexcept;
    void Clear();
    void Invalidate();

private:
    struct Page {
        std::uint64_t address;
        bool armed;
    };

    VM& vm;
    std::vector<Page> pages;
};
}  // namespace swift::runtime::execution
