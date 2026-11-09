#include "dispatch.h"
#include "fault.h"

namespace swift::runtime::execution {
bool IsDispatchInterrupt(const FaultContext& context, std::uintptr_t address) noexcept {
    const auto base = context.dispatch_interrupt_base.load(std::memory_order_acquire);
    // A lookup may have loaded the interrupt view just before a pending event
    // was cancelled. Its exact registered probe still returns normally to VM.
    return context.native && base && address >= base &&
           address - base < context.dispatch_interrupt_bytes;
}

void RecordMemoryFault(FaultContext& context,
                       std::uint64_t address,
                       GuestExceptionAccess access,
                       std::size_t bytes) noexcept {
    context.address.store(address, std::memory_order_relaxed);
    context.signal_number = context.signal_code = 0;
    context.host_pc.store(0, std::memory_order_relaxed);
    auto& hint = context.exception;
    hint.kind = GuestExceptionKind::Memory;
    hint.origin = GuestExceptionOrigin::Semantic;
    hint.access = access;
    hint.access_bytes = bytes <= UINT8_MAX ? static_cast<std::uint8_t>(bytes) : 0;
    hint.address = address;
    hint.address_valid = true;
    hint.signal_number = hint.signal_code = 0;
    hint.host_pc = 0;
}

void RecordGuestFault(FaultContext& context,
                      const backend::FaultSite& site,
                      int signal,
                      int code,
                      std::uintptr_t host_pc,
                      std::uintptr_t address) noexcept {
    *context.guest_pc = site.guest_pc;
    context.signal_number = signal;
    context.signal_code = code;
    context.host_pc = host_pc;
    context.address = address;
    auto& event = context.exception;
    event = {};
    event.isa = context.guest_isa;
    event.instruction_pc = site.guest_pc;
    event.address = address;
    event.address_valid = true;
    event.host_pc = host_pc;
    event.signal_number = signal;
    event.signal_code = code;
    event.kind = signal == SIGBUS && code == BUS_ADRALN ? GuestExceptionKind::Alignment
                                                        : GuestExceptionKind::Memory;
    if (context.guest_isa == GuestISA::X86_64)
        event.exception_vector = event.kind == GuestExceptionKind::Memory ? 14 : 13;
    event.access = static_cast<GuestExceptionAccess>(site.recovery[4] & 7);
    event.access_bytes = site.recovery[5];
    context.exception_pending = true;
}

}  // namespace swift::runtime::execution
