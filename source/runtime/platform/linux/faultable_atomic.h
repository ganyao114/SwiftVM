#pragma once

#include "runtime/atomic.h"

namespace swift::runtime::platform {

bool HasAtomic128() noexcept;
bool HasUnalignedAtomic64() noexcept;
AtomicStatus CompareExchange64(std::uint64_t address,
                               std::uint64_t expected,
                               std::uint64_t desired,
                               std::uint64_t& observed,
                               std::uint64_t& fault_address) noexcept;
AtomicStatus CompareExchange128(std::uint64_t address,
                                const AtomicValue& expected,
                                const AtomicValue& desired,
                                AtomicValue& observed,
                                std::uint64_t& fault_address) noexcept;

// Called only by the installed guest-thread FaultScope. The static leaf
// epilogue returns an ordinary status; no C++ stack or host lock is abandoned.
bool RecoverAtomicFault(int signal, std::uint64_t address, void* ucontext) noexcept;

}  // namespace swift::runtime::platform
