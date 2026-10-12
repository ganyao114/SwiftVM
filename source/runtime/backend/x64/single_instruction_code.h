#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "guest_access.h"
#include "runtime/include/sruntime.h"

namespace swift::runtime::backend::x64 {

class PrivateSingleInstructionCode;

// Immutable execution metadata, except the independently published frame
// anchor. The owner and all its ranges must remain live while native code or
// a signal handler can reference this descriptor. No lookup invokes callbacks.
class SingleInstructionFaultDescriptor final {
public:
    [[nodiscard]] bool ClassifyFault(std::uintptr_t host_pc,
            std::uintptr_t fault_address, std::uintptr_t recovery_slot,
            std::uintptr_t frame_slot, const State* active_state,
            std::uintptr_t r14, std::uintptr_t r13,
            std::uintptr_t rsp) const noexcept;

    [[nodiscard]] std::uintptr_t RecoveryPc() const noexcept { return recovery_pc; }
    [[nodiscard]] std::uintptr_t RxBegin() const noexcept { return rx_begin; }
    [[nodiscard]] std::uintptr_t RxEnd() const noexcept { return rx_end; }
    [[nodiscard]] u32 FrameSize() const noexcept { return frame_size; }
    [[nodiscard]] u64 OriginalPc() const noexcept { return original_pc; }
    [[nodiscard]] const State* OwningState() const noexcept { return owning_state; }
    [[nodiscard]] std::uintptr_t ExpectedFrame() const noexcept {
        return expected_frame.load(std::memory_order_acquire);
    }

private:
    friend class PrivateSingleInstructionCode;
    SingleInstructionFaultDescriptor(State* state, u32 frame_size,
            u64 original_pc, std::span<const GuestAccessRange> sites);

    const State* owning_state;
    std::uintptr_t rx_begin{};
    std::uintptr_t rx_end{};
    std::uintptr_t recovery_pc{};
    const std::uintptr_t guest_bias;
    const u64 guest_mask;
    const u64 guest_limit;
    const u32 frame_size;
    const u64 original_pc;
    const std::vector<GuestAccessRange> sites;
    alignas(std::uintptr_t) std::atomic<std::uintptr_t> expected_frame{};
};

// Linux x64 private RW staging -> RX mapping. It never publishes a module,
// dispatch entry or SMC node. The finished source uses internal relative
// branches/RIP labels and absolute helper addresses, so copying preserves them.
// This stationary owner cannot move while Runtime exposes its descriptor.
class PrivateSingleInstructionCode final {
public:
    PrivateSingleInstructionCode(State* state, std::span<const u8> finished_code,
            u32 recovery_offset, u32 frame_size, u64 original_pc,
            std::span<const GuestAccessRange> sites,
            GuestAccessCoverage coverage);
    ~PrivateSingleInstructionCode() noexcept;

    PrivateSingleInstructionCode(const PrivateSingleInstructionCode&) = delete;
    PrivateSingleInstructionCode& operator=(const PrivateSingleInstructionCode&) = delete;
    PrivateSingleInstructionCode(PrivateSingleInstructionCode&&) = delete;
    PrivateSingleInstructionCode& operator=(PrivateSingleInstructionCode&&) = delete;

    [[nodiscard]] const SingleInstructionFaultDescriptor& Descriptor() const noexcept {
        return descriptor;
    }
    // Runtime must install Descriptor() in its active execution scope before
    // calling this, and clear that reference before destroying this owner.
    // The wrapper independently records the native frame before code entry.
    [[nodiscard]] HaltReason Run();

private:
    SingleInstructionFaultDescriptor descriptor;
    void* mapping{};
    std::size_t mapping_size{};
};

} // namespace swift::runtime::backend::x64
