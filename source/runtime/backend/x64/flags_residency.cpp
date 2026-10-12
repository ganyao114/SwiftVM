#include "translator.h"

namespace swift::runtime::backend::x64 {
using namespace Xbyak;

void Translator::ReadGuestFlags(const Reg64& destination) {
    if (flags_resident && flags_residency_active) {
        if (destination.getIdx() != rbx.getIdx()) {
            mov(destination, rbx);
            ++statistics.flags_resident_read_sites;
        }
    } else {
        mov(destination, qword[r12 + state_offset_host_flags]);
        ++statistics.flags_state_load_sites;
    }
}

void Translator::WriteGuestFlags(const Reg64& source) {
    if (flags_resident && flags_residency_active) {
        if (source.getIdx() != rbx.getIdx()) {
            mov(rbx, source);
            ++statistics.flags_resident_write_sites;
        }
    } else {
        mov(qword[r12 + state_offset_host_flags], source);
        ++statistics.flags_state_store_sites;
    }
}

void Translator::FlushGuestFlags() {
    if (!flags_resident || !flags_residency_active) return;
    // Never truncate: unused high bits, AF and the saved parity byte are
    // part of State's packed guest-flags representation.
    mov(qword[r12 + state_offset_host_flags], rbx);
    ++statistics.flags_state_store_sites;
}

void Translator::ReloadGuestFlags() {
    if (!flags_resident) return;
    // Also used before activating the prologue's resident register and at
    // recovery entry, where a fault may have abandoned a helper's own RBX.
    mov(rbx, qword[r12 + state_offset_host_flags]);
    ++statistics.flags_state_load_sites;
}

} // namespace swift::runtime::backend::x64
