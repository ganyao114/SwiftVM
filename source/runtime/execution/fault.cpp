#include "fault.h"

namespace swift::runtime::execution {
[[noreturn]] void RaiseGuestFault(FaultContext& context, const backend::FaultSite& site,
                                int signal, int code, std::uintptr_t host_pc,
                                std::uintptr_t address) noexcept {
    context.state->pc = site.guest_pc;
    context.signal_number = signal;
    context.signal_code = code;
    context.host_pc = host_pc;
    context.address = address;
    context.active.store(nullptr);
    siglongjmp(context.jump, 1);
}
} // namespace swift::runtime::execution
