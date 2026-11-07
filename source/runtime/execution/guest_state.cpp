#include "vm.h"

#include <memory>

namespace swift::runtime::execution {

void VM::ConfigureGuest(GuestISA isa) {
    if (isa == GuestISA::Arm64) {
        if (layout.isa != isa) std::construct_at(&thread.native.arm_state);
        using State = arm64::GuestState;
        layout = {isa,
                  sizeof(State),
                  offsetof(State, gprs),
                  offsetof(State, pc),
                  offsetof(State, packed_flags),
                  offsetof(State, gprs) + 31 * 8,
                  offsetof(State, q),
                  offsetof(State, numeric_control),
                  32,
                  32};
        uniform_data = &thread.native.arm_state;
    } else {
        if (layout.isa != isa) std::construct_at(&thread.native.state);
        using State = x86::GuestState;
        layout = {isa,
                  sizeof(State),
                  offsetof(State, gprs),
                  offsetof(State, pc),
                  offsetof(State, packed_flags),
                  offsetof(State, gprs) + x86::RSP * 8,
                  offsetof(State, xmm),
                  offsetof(State, mxcsr),
                  16,
                  16};
        uniform_data = &thread.native.state;
    }
    if (backend.configure) backend.configure(backend.context, layout);
    auto& native = thread.native;
    native.pinned_uniform_offset = backend.pinned_uniform_slots ? layout.gprs_offset : UINT32_MAX;
    native.pinned_vector_offset = backend.pinned_vectors ? layout.vectors_offset : UINT32_MAX;
    native.flags_offset = layout.flags_offset;
    native.pc_offset = layout.pc_offset;
    auto& fault = thread.faults;
    fault.pinned_uniform_offset = static_cast<std::uint32_t>(native.pinned_uniform_offset);
    fault.pinned_vector_offset = static_cast<std::uint32_t>(native.pinned_vector_offset);
    fault.flags_offset = layout.flags_offset;
}

}  // namespace swift::runtime::execution
