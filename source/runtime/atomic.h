#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace swift::runtime {

struct AtomicValue {
    std::uint64_t low{}, high{};
};

enum class AtomicOperation : std::uint32_t {
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
    SignedMax,
    SignedMin,
    UnsignedMax,
    UnsignedMin,
    StoreExclusive,
};

struct AtomicRequest {
    std::uint64_t address{};
    AtomicOperation operation{};
    std::uint32_t bits{};
    AtomicValue expected, desired;
    std::uint64_t carry{};
};

enum class AtomicStatus : std::uint64_t { Completed, Fault, Unsupported };

// One transient call packet, shared by direct execution and native cold paths.
// The owner qualifies the entire access before committing any guest bytes.
// Ordinary CAS completes a write cycle even on a comparison mismatch.
// StoreExclusive instead leaves memory untouched on a comparison mismatch.
struct AtomicCall {
    AtomicRequest request;
    AtomicValue observed;
    std::uint64_t fault_address{};
};

using AtomicService = std::uint64_t (*)(void*, AtomicCall*) noexcept;

static_assert(std::is_standard_layout_v<AtomicCall> && std::is_trivially_copyable_v<AtomicCall>);
static_assert(sizeof(AtomicRequest) == 56 && sizeof(AtomicCall) == 80);
static_assert(offsetof(AtomicRequest, address) == 0 && offsetof(AtomicRequest, operation) == 8 &&
              offsetof(AtomicRequest, bits) == 12);
static_assert(offsetof(AtomicRequest, expected) == 16 && offsetof(AtomicRequest, desired) == 32 &&
              offsetof(AtomicRequest, carry) == 48);
static_assert(offsetof(AtomicCall, observed) == 56 && offsetof(AtomicCall, fault_address) == 72);

}  // namespace swift::runtime
