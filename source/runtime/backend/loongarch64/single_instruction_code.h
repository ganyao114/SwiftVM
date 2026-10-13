#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "guest_access.h"
#include "jit/fault_scope.h"
#include "runtime/ir/block.h"
#include "runtime/include/sruntime.h"

namespace swift::runtime::backend::loongarch64 {

struct SingleFaultContext {
    std::uintptr_t host_pc{};
    std::uintptr_t fault_address{};
    std::uintptr_t recovery_slot{};
    std::uintptr_t frame_slot{};
    const State* active_state{};
    std::array<std::uintptr_t, 32> gprs{};
};

class PrivateSingleInstructionCode;

// Immutable RX/State/access metadata. Only the bridge's separately published
// native-frame anchor changes while executing. Signal classification performs
// no allocation, locking, module lookup, map oracle or semantic callback.
class SingleInstructionFaultDescriptor final {
public:
    [[nodiscard]] GuestFaultKind ClassifyFault(const SingleFaultContext& context) const noexcept;
    [[nodiscard]] std::uintptr_t RecoveryPc() const noexcept { return recovery_pc; }
    [[nodiscard]] std::uintptr_t RxBegin() const noexcept { return rx_begin; }
    [[nodiscard]] std::uintptr_t RxEnd() const noexcept { return rx_end; }
    [[nodiscard]] std::uintptr_t ExpectedFrame() const noexcept {
        return expected_frame.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 HomeBytes() const noexcept { return home_bytes; }
    [[nodiscard]] u64 OriginalPc() const noexcept { return original_pc; }
    [[nodiscard]] const State* OwningState() const noexcept { return owning_state; }

private:
    friend class PrivateSingleInstructionCode;
    SingleInstructionFaultDescriptor(State* state, u32 homes, u64 original,
            std::span<const GuestAccessRange> sites, bool typed);
    State* const owning_state;
    const std::uintptr_t guest_bias;
    const u64 guest_mask;
    const u64 guest_limit;
    const u32 home_bytes;
    const u64 original_pc;
    const std::vector<GuestAccessRange> sites;
    const bool audited_typed_guest_accesses;
    std::uintptr_t rx_begin{}, rx_end{}, recovery_pc{};
    alignas(std::uintptr_t) std::atomic<std::uintptr_t> expected_frame{};
};

// Stationary Linux LP64D owner: offline bytes -> private RW staging -> RX.
// Neither this object nor its retained IR publishes any Module, dispatch,
// disk-cache or SMC entry. Every embedded Block*/Inst* remains owned through
// execution; V256 legalization is deliberately outside this first contract.
class PrivateSingleInstructionCode final {
public:
    PrivateSingleInstructionCode(State* state, std::span<const u8> finished_code,
            u32 recovery_offset, u32 home_bytes, u64 original_pc,
            std::span<const GuestAccessRange> sites, GuestAccessCoverage coverage,
            IntrusivePtr<ir::Block> retained_ir, bool audited_typed_guest_accesses = false);
    ~PrivateSingleInstructionCode() noexcept;
    PrivateSingleInstructionCode(const PrivateSingleInstructionCode&) = delete;
    PrivateSingleInstructionCode& operator=(const PrivateSingleInstructionCode&) = delete;
    PrivateSingleInstructionCode(PrivateSingleInstructionCode&&) = delete;
    PrivateSingleInstructionCode& operator=(PrivateSingleInstructionCode&&) = delete;
    [[nodiscard]] const SingleInstructionFaultDescriptor& Descriptor() const noexcept {
        return descriptor;
    }
    // Runtime must publish Descriptor() before Run and remove that reference
    // before destruction. Construction and an inactive descriptor never modify
    // guest State. The bridge independently owns and clears ExpectedFrame().
    [[nodiscard]] HaltReason Run();

private:
    const IntrusivePtr<ir::Block> retained_ir;
    SingleInstructionFaultDescriptor descriptor;
    void* mapping{};
    size_t mapping_size{};
};

} // namespace swift::runtime::backend::loongarch64
