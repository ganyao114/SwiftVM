#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>

namespace swift::runtime::backend::riscv64 {

// Readers publish to private cache lines once per generated block, not once
// per guest access. The writer gate lives in unaligned_atomic_lock. A full
// store/load fence closes the reader-admission / writer-scan race.
struct alignas(64) MemoryParticipant;
inline std::mutex memory_participants_mutex;
inline MemoryParticipant* memory_participants{};
inline std::atomic<unsigned> anonymous_memory_readers{};
inline thread_local std::atomic<unsigned> memory_kernel_depth{};
inline void ReleaseAbandonedMemoryKernels() {
    const auto depth = memory_kernel_depth.exchange(0, std::memory_order_relaxed);
    if (depth) anonymous_memory_readers.fetch_sub(depth, std::memory_order_seq_cst);
}

struct alignas(64) MemoryParticipant {
    std::atomic<unsigned> active{};
    MemoryParticipant* next{};

    MemoryParticipant() {
        std::lock_guard lock{memory_participants_mutex};
        next = memory_participants; memory_participants = this;
    }
    ~MemoryParticipant() {
        std::lock_guard lock{memory_participants_mutex};
        auto** link = &memory_participants;
        while (*link != this) link = &(*link)->next;
        *link = next;
    }
    MemoryParticipant(const MemoryParticipant&) = delete;
    MemoryParticipant& operator=(const MemoryParticipant&) = delete;
};
static_assert(offsetof(MemoryParticipant, active) == 0);
static_assert(std::atomic<unsigned>::is_always_lock_free);

inline void DrainMemoryReaders() {
    // The gate is already set. Keep registry nodes alive until the scan
    // completes; readers clear active without taking this mutex.
    std::lock_guard lock{memory_participants_mutex};
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (auto* participant = memory_participants; participant; participant = participant->next)
        while (participant->active.load(std::memory_order_seq_cst)) std::this_thread::yield();
    while (anonymous_memory_readers.load(std::memory_order_seq_cst)) std::this_thread::yield();
}

// Dedicated C ABI copy/interpreter kernels can participate without a State
// node. They are cold paths; generated ordinary accesses use private nodes.
class MemoryKernelGuard {
public:
    explicit MemoryKernelGuard(std::atomic<unsigned>& gate) {
        for (;;) {
            anonymous_memory_readers.fetch_add(1, std::memory_order_seq_cst);
            if (!gate.load(std::memory_order_seq_cst)) { memory_kernel_depth.fetch_add(1, std::memory_order_relaxed); break; }
            anonymous_memory_readers.fetch_sub(1, std::memory_order_seq_cst);
            while (gate.load(std::memory_order_acquire)) std::this_thread::yield();
        }
    }
    MemoryKernelGuard(const MemoryKernelGuard&) = delete;
    MemoryKernelGuard& operator=(const MemoryKernelGuard&) = delete;
    ~MemoryKernelGuard() { memory_kernel_depth.fetch_sub(1, std::memory_order_relaxed); anonymous_memory_readers.fetch_sub(1, std::memory_order_seq_cst); }
};
} // namespace swift::runtime::backend::riscv64
