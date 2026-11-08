#include "atomic_service.h"

#include "guest_execution.h"
#include "memory.h"
#include "runtime/platform/linux/faultable_atomic.h"
#include "smc.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <sys/mman.h>

namespace swift::runtime::execution {
namespace {
using U64 = std::uint64_t;
using U128 = unsigned __int128;

U128 Bits(AtomicValue value) { return (U128{value.high} << 64) | value.low; }

AtomicValue Pair(U128 value) { return {static_cast<U64>(value), static_cast<U64>(value >> 64)}; }

U128 Mask(unsigned bits) { return bits == 128 ? ~U128{} : (U128{1} << bits) - 1; }

bool Matches(const AtomicRequest& request, U128 old) {
    return old == (Bits(request.expected) & Mask(request.bits));
}

U128 Replacement(const AtomicRequest& request, U128 old) {
    const auto mask = Mask(request.bits), desired = Bits(request.desired) & mask;
    switch (request.operation) {
        case AtomicOperation::CompareExchange:
        case AtomicOperation::CompareExchange128:
        case AtomicOperation::StoreExclusive:
            return Matches(request, old) ? desired : old;
        case AtomicOperation::Exchange:
            return desired;
        case AtomicOperation::FetchAdd:
        case AtomicOperation::Add:
            return (old + desired) & mask;
        case AtomicOperation::Sub:
            return (old - desired) & mask;
        case AtomicOperation::And:
            return old & desired;
        case AtomicOperation::Or:
            return old | desired;
        case AtomicOperation::Xor:
            return old ^ desired;
        case AtomicOperation::Neg:
            return (U128{} - old) & mask;
        case AtomicOperation::AddCarry:
            return (old + desired + (request.carry & 1)) & mask;
        case AtomicOperation::SubBorrow:
            return (old - desired - (request.carry & 1)) & mask;
        case AtomicOperation::SignedMax:
        case AtomicOperation::SignedMin: {
            const auto sign = U128{1} << (request.bits - 1);
            const bool less = (old ^ sign) < (desired ^ sign);
            return less == (request.operation == AtomicOperation::SignedMax) ? desired : old;
        }
        case AtomicOperation::UnsignedMax:
            return old < desired ? desired : old;
        case AtomicOperation::UnsignedMin:
            return old < desired ? old : desired;
    }
    return old;
}

struct Qualified {
    AtomicStatus status{AtomicStatus::Completed};
    bool private_stable{true};
};

Qualified Qualify(GuestMemory& memory, AtomicCall& call) {
    const auto address = call.request.address, bytes = U64{call.request.bits / 8};
    if (bytes > UINT64_MAX - address) {
        call.fault_address = address;
        return {AtomicStatus::Fault};
    }
    const auto end = address + bytes;
    auto cursor = address;
    Qualified qualified;
    bool device{};
    for (const auto& region : memory.Regions()) {
        if (region.address + region.size <= cursor) continue;
        if (region.address > cursor ||
            (region.protection & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE)) {
            call.fault_address = cursor;
            return {AtomicStatus::Fault};
        }
        auto tail = std::min(end, region.address + region.size);
        if (region.backing == BackingKind::FilePrivate ||
            region.backing == BackingKind::FileShared) {
            const auto file_bytes = region.file_size > region.file_offset
                                            ? region.file_size - region.file_offset
                                            : 0;
            const auto pages = file_bytes ? (file_bytes - 1) / memory.PageSize() + 1 : 0;
            const auto accessible =
                    region.address + std::min<U64>(region.size, pages * memory.PageSize());
            if (cursor >= accessible || tail > accessible) {
                call.fault_address = std::max(cursor, accessible);
                return {AtomicStatus::Fault};
            }
        }
        qualified.private_stable &= region.backing == BackingKind::Anonymous;
        device |= region.backing == BackingKind::Other;
        cursor = tail;
        if (cursor == end) {
            if (device) qualified.status = AtomicStatus::Unsupported;
            return qualified;
        }
    }
    call.fault_address = cursor;
    return {AtomicStatus::Fault};
}

AtomicStatus Fault(AtomicCall& call, U64 address) {
    call.fault_address = address;
    return AtomicStatus::Fault;
}

AtomicStatus PrivateCommit(GuestMemory& memory, Smc& smc, AtomicCall& call) {
    GuestExecutionLock execution;
    const auto& request = call.request;
    const auto bytes = request.bits / 8;
    AtomicValue observed{};
    if (!memory.CopyIn(&observed, request.address, bytes)) return Fault(call, request.address);
    const auto old = Bits(observed) & Mask(request.bits);
    call.observed = Pair(old);
    if (request.operation == AtomicOperation::StoreExclusive && !Matches(request, old))
        return AtomicStatus::Completed;
    if (!smc.PrepareWriteRange(request.address, bytes)) return Fault(call, request.address);
    const auto desired = Pair(Replacement(request, old));
    // FaultScope holds the same lock throughout JIT/direct execution, including
    // smaller hardware atomics and ordinary loads/stores. Private stable pages
    // exclude external mapping writers; this mutex cannot synchronize those.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::memcpy(reinterpret_cast<void*>(request.address), &desired, bytes);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return AtomicStatus::Completed;
}

AtomicStatus HardwareCommit(GuestMemory& memory,
                            Smc& smc,
                            AtomicCall& call,
                            unsigned width,
                            bool unaligned_container = false) {
    const auto& request = call.request;
    const auto address =
            unaligned_container ? request.address : request.address & ~(U64{width / 8} - 1);
    const auto shift = unsigned(request.address - address) * 8;
    const auto value_mask = Mask(request.bits), field_mask = value_mask << shift;
    AtomicValue word{};
    if (!memory.CopyIn(&word, address, width / 8)) return Fault(call, request.address);
    auto old_word = Bits(word);
    const bool exclusive = request.operation == AtomicOperation::StoreExclusive;
    auto old = (old_word >> shift) & value_mask;
    call.observed = Pair(old);
    if (exclusive && !Matches(request, old)) return AtomicStatus::Completed;
    if (!smc.PrepareWriteRange(request.address, request.bits / 8))
        return Fault(call, request.address);
    for (;;) {
        old = (old_word >> shift) & value_mask;
        call.observed = Pair(old);
        if (exclusive && !Matches(request, old)) return AtomicStatus::Completed;
        const auto replacement = (old_word & ~field_mask) | (Replacement(request, old) << shift);
        AtomicValue observed{};
        U64 fault_address{};
        const auto status = width == 64 ? platform::CompareExchange64(address,
                                                                      static_cast<U64>(old_word),
                                                                      static_cast<U64>(replacement),
                                                                      observed.low,
                                                                      fault_address)
                                        : platform::CompareExchange128(address,
                                                                       Pair(old_word),
                                                                       Pair(replacement),
                                                                       observed,
                                                                       fault_address);
        if (status != AtomicStatus::Completed) {
            if (status == AtomicStatus::Fault)
                call.fault_address = std::max(request.address, fault_address);
            return status;
        }
        if (Bits(observed) == old_word) return AtomicStatus::Completed;
        old_word = Bits(observed);
    }
}
}  // namespace

AtomicStatus ExecuteAtomic(GuestMemory& memory, Smc& smc, AtomicCall& call) noexcept {
    call.observed = {};
    call.fault_address = 0;
    const auto& request = call.request;
    const bool wide =
            request.bits == 128 && (request.operation == AtomicOperation::CompareExchange128 ||
                                    request.operation == AtomicOperation::StoreExclusive);
    if ((!wide && request.bits != 8 && request.bits != 16 && request.bits != 32 &&
         request.bits != 64) ||
        request.operation > AtomicOperation::StoreExclusive ||
        (!wide && request.operation == AtomicOperation::CompareExchange128) ||
        (wide && (request.address & 15)))
        return AtomicStatus::Unsupported;
    const auto qualified = Qualify(memory, call);
    if (qualified.status != AtomicStatus::Completed) return qualified.status;
    if (request.operation == AtomicOperation::StoreExclusive && !qualified.private_stable)
        return AtomicStatus::Unsupported;
    const auto bytes = request.bits / 8;
    if ((request.address & 7) + bytes <= 8) return HardwareCommit(memory, smc, call, 64);
    // On x64 a real locked 8-byte container can start at the requested address.
    // This case already spans aligned words, so widening stays in the same
    // qualified host page(s); unchanged neighbor bits participate in the CAS.
    if (bytes <= 8 && platform::HasUnalignedAtomic64())
        return HardwareCommit(memory, smc, call, 64, true);
    if ((request.address & 15) + bytes <= 16 && platform::HasAtomic128())
        return HardwareCommit(memory, smc, call, 128);
    return qualified.private_stable ? PrivateCommit(memory, smc, call) : AtomicStatus::Unsupported;
}
}  // namespace swift::runtime::execution
