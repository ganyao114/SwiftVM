#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "decode.h"

namespace swift::runtime::x86 {
struct GuestState;
}

namespace swift::x86 {

enum class AccessStatus : std::uint8_t { Completed, Fault, UnsupportedBeforeAccess };
struct AccessResult {
    AccessStatus status{AccessStatus::Completed};
    std::uint64_t fault_address{};
    // A successful write may require leaving the current execution fragment
    // after its architectural register/PC commit. It is never retried as a miss.
    bool code_changed{};
};
struct CodeReadResult {
    AccessResult access;
    // A readable prefix is useful even when the following page is inaccessible.
    // The decoder, not the fetch service, determines the instruction length.
    std::size_t bytes{};
};
struct AtomicValue {
    std::uint64_t low{}, high{};
};
enum class AtomicOperation : std::uint8_t {
    CompareExchange,
    Exchange,
    FetchAdd,
    Add,
    Sub,
    And,
    Or,
    Xor,
    Neg,
    AddCarry,
    SubBorrow,
    CompareExchange128,
};
struct AtomicRequest {
    std::uint64_t address{};
    AtomicOperation operation{};
    unsigned bits{};
    AtomicValue expected, desired;
    bool carry{};
};
struct CpuResult {
    std::uint64_t lower{}, upper{};
};

// Actual execution services, bound once by the VM. All accesses report failure
// without unwinding across guest code. Read/write buffers carry exactly the
// access width; ordered accesses and atomics retain their real memory semantics.
// UnsupportedBeforeAccess promises no memory or architectural side effect.
struct DirectServices {
    void* opaque{};
    CodeReadResult (*fetch)(void*, std::uint64_t pc, std::span<std::uint8_t>) noexcept {};
    AccessResult (*read)(void*, std::uint64_t, std::span<std::uint8_t>, bool ordered) noexcept {};
    AccessResult (*write)(void*,
                          std::uint64_t,
                          std::span<const std::uint8_t>,
                          bool ordered) noexcept {};
    AccessResult (*atomic)(void*, const AtomicRequest&, AtomicValue& observed) noexcept {};
    CpuResult (*cpuid)(void*, std::uint32_t leaf, std::uint32_t subleaf) noexcept {};
    runtime::x86::GuestCpuModel model{runtime::x86::BaselineGuestCpu};
    AccessResult (*state_image)(void*,
                                unsigned kind,
                                std::uint64_t address,
                                std::uint64_t requested) noexcept {};
};
enum class StepKind : std::uint8_t {
    Completed,
    Service,
    Fault,
    Unsupported,
    InvalidEncoding,
    Truncated,
};
struct StepResult {
    StepKind kind{StepKind::Completed};
    std::uint64_t pc{}, next_pc{}, fault_address{};
    // Existing runtime exits: 1=syscall, 3=MXCSR rebind, 4=x87 exception.
    std::uint64_t service{};
    std::size_t instructions{}, decoded_bytes{};
    bool code_changed{};
};

// One architectural instruction, or one restartable REP iteration. No region,
// Builder, Program, allocation, native code or persistent decoded cache exists.
// All completed effects precede next_pc; faults retain the current instruction
// and its architecturally defined partial completion. Unsupported is returned
// before guest side effects and never means "retry a partly executed step".
[[nodiscard]] StepResult InterpretOne(runtime::x86::GuestState&,
                                      const DirectServices&,
                                      DecodeTiming* timing = nullptr);

}  // namespace swift::x86
