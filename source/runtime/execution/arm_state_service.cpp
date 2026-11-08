#include "vm.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/mman.h>

#include "runtime/arch/arm64/service.h"
#include "runtime/atomic.h"

namespace swift::runtime::execution {
namespace {
using u64 = std::uint64_t;
using Kind = arm64::StateServiceKind;

// 64-byte I/D lines, writeback and reservation granules, PIPT instruction cache.
constexpr u64 CacheType =
        (u64{1} << 31) | (u64{4} << 24) | (u64{4} << 20) | (u64{4} << 16) | (u64{3} << 14) | 4;
}  // namespace

bool VM::HandleArmStateService() {
    auto& state = thread.native.arm_state;
    const auto request = arm64::DecodeStateService(state.service_word);
    thread.bridge_failure.Clear();
    const auto read_register = [&](unsigned reg, bool stack = false) {
        return reg == 31 && !stack ? u64{0} : state.gprs[reg];
    };
    const auto write_register = [&](unsigned reg, u64 value) {
        if (reg != 31) state.gprs[reg] = value;
    };
    const auto complete = [&] {
        state.pc += 4;
        return true;
    };
    const auto fail = [&](ErrorKind kind, const char* operation, int saved_errno = 0) {
        state.reservation_valid = 0;
        if (!thread.bridge_failure) thread.bridge_failure.Set(kind, operation, saved_errno);
        return false;
    };
    const auto access_fault = [&](u64 address, const char* operation) {
        thread.faults.address.store(address, std::memory_order_relaxed);
        const auto saved = errno;
        return fail(saved && saved != EFAULT ? ErrorKind::System : ErrorKind::GuestFault,
                    operation,
                    saved && saved != EFAULT ? saved : 0);
    };
    try {
        if (request.kind == Kind::LoadExclusive || request.kind == Kind::StoreExclusive) {
            const auto bytes = request.TransferBytes();
            const auto address = read_register(request.address_register, true);
            const auto monitor = thread.arm_reservation;
            const bool matching =
                    state.reservation_valid && monitor.address == address && monitor.bytes == bytes;
            // Every store-exclusive consumes its reservation. A failed monitor
            // check performs no memory access and never becomes a CAS write cycle.
            state.reservation_valid = 0;
            if (request.kind == Kind::StoreExclusive && !matching) {
                write_register(request.status_register, 1);
                return complete();
            }
            if (address & (bytes - 1)) {
                thread.faults.address.store(address, std::memory_order_relaxed);
                return fail(ErrorKind::GuestFault, "unaligned ARM exclusive access");
            }
            const int protection =
                    request.kind == Kind::LoadExclusive ? PROT_READ : PROT_READ | PROT_WRITE;
            if (!memory.Accessible(address, bytes, protection)) {
                errno = EFAULT;
                return access_fault(address, "ARM exclusive memory access fault");
            }
            // There is no external-writer monitor. Shared mappings and file
            // truncation/ABA domains cannot acquire a software reservation.
            if (!memory.PrivateAnonymous(address, bytes))
                return fail(ErrorKind::Unsupported,
                            "ARM exclusive access requires private anonymous memory");
            if (request.kind == Kind::LoadExclusive) {
                AtomicValue observed{};
                errno = 0;
                if (!memory.CopyIn(&observed, address, bytes))
                    return access_fault(address, "read ARM exclusive value");
                if (request.ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
                if (request.pair && request.size_log2 == 2) {
                    write_register(request.first_register, std::uint32_t(observed.low));
                    write_register(request.second_register, observed.low >> 32);
                } else {
                    write_register(request.first_register, observed.low);
                    if (request.pair) write_register(request.second_register, observed.high);
                }
                thread.arm_reservation = {address, bytes, observed};
                state.reservation_valid = 1;
                return complete();
            }
            AtomicCall packet;
            packet.request.address = address;
            packet.request.operation = AtomicOperation::StoreExclusive;
            packet.request.bits = bytes * 8;
            packet.request.expected = monitor.observed;
            const auto first = read_register(request.first_register),
                       second = read_register(request.second_register);
            if (request.pair && request.size_log2 == 2)
                packet.request.desired.low =
                        std::uint32_t(first) | (u64(std::uint32_t(second)) << 32);
            else {
                packet.request.desired.low =
                        bytes < 8 ? first & ((u64{1} << (bytes * 8)) - 1) : first;
                if (request.pair) packet.request.desired.high = second;
            }
            const auto status = AtomicStatus(AtomicBridge(this, &packet));
            if (status == AtomicStatus::Fault) {
                errno = EFAULT;
                return access_fault(packet.fault_address, "write ARM exclusive value");
            }
            if (status != AtomicStatus::Completed)
                return fail(ErrorKind::Unsupported, "ARM store-exclusive service unavailable");
            const bool stored = packet.observed.low == monitor.observed.low &&
                                (bytes != 16 || packet.observed.high == monitor.observed.high);
            write_register(request.status_register, stored ? 0 : 1);
            return complete();
        }
        if (request.kind == Kind::ClearExclusive) {
            state.reservation_valid = 0;
            return complete();
        }
        if (request.kind == Kind::ReadVirtualCounter || request.kind == Kind::ReadPhysicalCounter) {
            const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
            write_register(request.first_register, u64(ticks));
            return complete();
        }
        if (request.kind == Kind::ReadCounterFrequency || request.kind == Kind::ReadCacheType) {
            write_register(request.first_register,
                           request.kind == Kind::ReadCounterFrequency ? 1000000000 : CacheType);
            return complete();
        }
        if (request.kind == Kind::DataCacheCleanPoU || request.kind == Kind::DataCacheCleanPoC ||
            request.kind == Kind::DataCacheCleanInvalidatePoC ||
            request.kind == Kind::InstructionCacheInvalidatePoU) {
            const auto address = read_register(request.address_register);
            std::uint8_t probe;
            errno = 0;
            if (!memory.CopyIn(&probe, address, 1))
                return access_fault(address, "ARM cache-maintenance address fault");
            std::atomic_thread_fence(std::memory_order_seq_cst);
            // Host native publication already owns actual I-cache maintenance.
            // Retire translations for the guest line and its executable aliases.
            if (memory.WriteAffectsCode(address & ~u64{63}, 64)) Invalidate();
            return complete();
        }
        return fail(ErrorKind::Unsupported, "unknown ARM state service");
    } catch (const std::exception& failure) {
        state.reservation_valid = 0;
        thread.bridge_failure.Capture(failure);
        return false;
    }
}
}  // namespace swift::runtime::execution
