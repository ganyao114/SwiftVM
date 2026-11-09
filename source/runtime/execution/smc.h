#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "runtime/code_source.h"

namespace swift::runtime::execution {
class VM;

// Only armed-page state lives here. All VA/backing/protection facts are read
// from GuestMemory's one region ledger, including after map replacements.
class Smc {
public:
    explicit Smc(VM& vm) noexcept : vm(vm) {}

    void Arm(std::uint64_t, std::size_t);
    // Actual submitted source ranges, not a decoded address envelope. Prepare
    // every page/known writable alias before applying any new protection.
    void Arm(std::span<const SourceSpan>);
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
    bool fully_armed{true};  // False also after a partial protection failure.
};
}  // namespace swift::runtime::execution
