#include "vm.h"

#include <atomic>
#include <cerrno>
#include <sys/mman.h>
#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/service.h"
#include "runtime/atomic.h"

namespace swift::runtime::execution {
bool VM::HandleArm32StateService() {
    using Kind = arm32::StateServiceKind;
    auto& state = Arm32State();
    const auto request = arm32::DecodeStateService(state.service_word);
    thread.bridge_failure.Clear();
    const auto complete = [&] {
        state.pc = arm32::Location{state.service_pc}
                           .Advance(request.instruction_bytes)
                           .AdvanceIT()
                           .Key();
        return true;
    };
    const auto fail = [&](ErrorKind kind, const char* operation, int error = 0) {
        state.reservation_valid = 0;
        thread.bridge_failure.Set(kind, operation, error);
        return false;
    };
    const auto fault = [&](std::uint64_t address, GuestExceptionAccess access, unsigned bytes) {
        RecordMemoryFault(thread.faults, address, access, bytes);
        return fail(ErrorKind::GuestFault, "ARM32 exclusive memory access fault");
    };
    try {
        if (request.kind == Kind::LoadExclusive || request.kind == Kind::StoreExclusive) {
            const auto bytes = 1u << request.size_log2;
            const auto address =
                    std::uint32_t(state.r[request.address_register] + 4u * request.offset_words);
            const auto access = request.kind == Kind::LoadExclusive ? GuestExceptionAccess::Read
                                                                    : GuestExceptionAccess::Atomic;
            const auto monitor = thread.arm_reservation;
            const bool matching =
                    state.reservation_valid && monitor.address == address && monitor.bytes == bytes;
            state.reservation_valid = 0;
            if (request.kind == Kind::StoreExclusive && !matching) {
                state.r[request.status_register] = 1;
                return complete();
            }
            if (address & (bytes - 1)) {
                fault(address, access, bytes);
                thread.faults.exception.kind = GuestExceptionKind::Alignment;
                return false;
            }
            const auto protection =
                    request.kind == Kind::LoadExclusive ? PROT_READ : PROT_READ | PROT_WRITE;
            if (!memory.Accessible(address, bytes, protection))
                return fault(address, access, bytes);
            if (!memory.PrivateAnonymous(address, bytes))
                return fail(ErrorKind::Unsupported,
                            "ARM32 exclusive access requires private anonymous memory");
            if (request.kind == Kind::LoadExclusive) {
                AtomicValue observed{};
                if (!memory.CopyIn(&observed, address, bytes)) return fault(address, access, bytes);
                if (request.ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
                state.r[request.first_register] = std::uint32_t(observed.low);
                if (bytes == 8) state.r[request.second_register] = observed.low >> 32;
                thread.arm_reservation = {address, bytes, observed};
                state.reservation_valid = 1;
                return complete();
            }
            AtomicCall packet;
            packet.request.address = address;
            packet.request.bits = bytes * 8;
            packet.request.operation = AtomicOperation::StoreExclusive;
            packet.request.expected = monitor.observed;
            packet.request.desired.low = std::uint32_t(state.r[request.first_register]);
            if (bytes == 8)
                packet.request.desired.low |=
                        std::uint64_t(std::uint32_t(state.r[request.second_register])) << 32;
            else if (bytes < 4)
                packet.request.desired.low &= (std::uint64_t{1} << (bytes * 8)) - 1;
            const auto status = static_cast<AtomicStatus>(AtomicBridge(this, &packet));
            if (status == AtomicStatus::Fault) return fault(packet.fault_address, access, bytes);
            if (status != AtomicStatus::Completed)
                return fail(ErrorKind::Unsupported, "ARM32 store-exclusive service unavailable");
            state.r[request.status_register] = packet.observed.low == monitor.observed.low ? 0 : 1;
            return complete();
        }
        if (request.kind == Kind::ClearExclusive) {
            state.reservation_valid = 0;
            return complete();
        }
        if (request.kind == Kind::ReadTLS) {
            state.r[request.first_register] =
                    request.offset_words ? state.tpidruro : state.tpidrurw;
            return complete();
        }
        if (request.kind == Kind::WriteTLS) {
            state.tpidrurw = std::uint32_t(state.r[request.first_register]);
            return complete();
        }
        if (request.kind == Kind::WriteCPSR) {
            const auto fields = request.offset_words;
            const std::uint32_t mask = ((fields & 8) ? 0xf8000000u : 0u) |
                                       ((fields & 4) ? 0x000f0000u : 0u) |
                                       ((fields & 2) ? 0x00000200u : 0u);
            const auto cpsr = (state.Cpsr() & ~mask) |
                              (std::uint32_t(state.r[request.first_register]) & mask);
            if (cpsr & 512u)
                return fail(ErrorKind::Unsupported, "ARM32 big-endian execution is unsupported");
            state.SetCpsr(cpsr);
            return complete();
        }
        if (request.kind == Kind::WriteFPSCR) {
            const auto value = std::uint32_t(state.r[request.first_register]);
            if (!arm32::IsSupportedFPSCR(value))
                return fail(ErrorKind::Unsupported,
                            "ARM32 FPSCR exception enables are unsupported");
            state.fpscr = value;
            complete();
            UpdateFPEnvironment(true);
            return true;
        }
        if (request.kind == Kind::CacheFlush) {
            Invalidate();
            return complete();
        }
        return fail(ErrorKind::Unsupported, "unknown ARM32 state service");
    } catch (const std::exception& error) {
        state.reservation_valid = 0;
        thread.bridge_failure.Capture(error);
        return false;
    }
}
}  // namespace swift::runtime::execution
